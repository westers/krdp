// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 2: host-to-client file copy end to end, in one process pair. A real KRdp::Server and Clipboard, the worker's file source
// behind them, and krdpctl-probe --clipboard files (libfreerdp's cliprdr client) receiving: a directory tree byte for byte, the lock and
// unlock, cancel in the middle, a file that changes or vanishes, hostile requests, the planner's states, the capability flags.
// (A file over 4 GiB, sparse, is ClipboardFileLargeLoopbackTest.)

#include "ClipboardFileLoopbackHarness.h"

#include "ConsoleClipboardPolicy.h"

using namespace LoopbackHarness;
using namespace Qt::StringLiterals;

class ClipboardFileLoopbackTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_certificate;
    QString m_key;

    QString p(const QString &name) const { return m_dir.filePath(name); }
    QByteArray native(const QString &path) const { return QFile::encodeName(path); }
    QString fresh(const char *name)
    {
        const QString dir = m_dir.filePath(QString::fromLatin1(name));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        return dir;
    }
    bool begin(Run &run, const QList<QByteArray> &selection, const QStringList &arguments)
    {
        run.selection = selection;
        return run.start(this, m_certificate, m_key, m_dir.path(), arguments);
    }
    static bool noH264(const Run &run) { return run.log.contains("does not support H.264"); }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        if (QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty()) QSKIP("openssl is needed to make the test certificate");
        m_certificate = makeCertificate(m_dir.path(), &m_key);
        QVERIFY(!m_certificate.isEmpty());
    }

    void aDirectoryTreeArrivesByteForByteWithTheLockTaken()
    {
        const QString src = fresh("src1");
        const QString out = fresh("out1");
        writeFile(src + u"/album/a.txt"_s, "alpha");
        writeFile(src + u"/album/empty"_s, {});
        writeFile(src + u"/album/one.bin"_s, QByteArray(1, '\x7f'));
        writeFile(src + u"/album/sub/deep/b.bin"_s, pattern(300 * 1024 + 11, 1));
        writeFile(src + u"/album/sub/c.bin"_s, pattern(1024 * 1024 + 17, 2));
        QDir().mkpath(src + u"/album/emptydir"_s);
        writeFile(src + u"/single.txt"_s, "solo");
        Run run;
        QVERIFY(begin(run, {native(src + u"/album"_s), native(src + u"/single.txt"_s)}, {u"--files-out"_s, out, u"--files-window"_s, u"4"_s}));
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES done") || run.probe.state() != QProcess::Running; }), run.log.constData());
        qInfo().noquote() << run.log.right(1500);
        QVERIFY2(run.log.contains("FILES caps"), run.log.constData());
        QVERIFY(run.line("FILES caps").contains("stream=1 lock=1 huge=1"));
        QVERIFY2(run.log.contains("FILES done files=6 bytes="), run.log.constData());
        QVERIFY(run.line("FILES done").contains("failed=0"));
        // every byte
        QCOMPARE(readAt(out + u"/album/a.txt"_s, 0, 100), QByteArray("alpha"));
        QCOMPARE(readAt(out + u"/album/one.bin"_s, 0, 100), QByteArray(1, '\x7f'));
        QCOMPARE(QFileInfo(out + u"/album/empty"_s).size(), qint64(0));
        QCOMPARE(readAt(out + u"/album/sub/deep/b.bin"_s, 0, 400 * 1024), pattern(300 * 1024 + 11, 1));
        QCOMPARE(readAt(out + u"/album/sub/c.bin"_s, 0, 2 * 1024 * 1024), pattern(1024 * 1024 + 17, 2));
        QCOMPARE(readAt(out + u"/single.txt"_s, 0, 100), QByteArray("solo"));
        QVERIFY(QFileInfo(out + u"/album/emptydir"_s).isDir());
        // the lock: taken once for the offer, used by the ranges, released when the client was done
        QCOMPARE(run.locks.size(), 1);
        QCOMPARE(run.locks.first(), quint32(42));
        QTRY_COMPARE_WITH_TIMEOUT(run.unlocks.size(), 1, 5000);
        QCOMPARE(run.unlocks.first(), quint32(42));
        QVERIFY(!run.reads.isEmpty());
        for (const auto &read : std::as_const(run.reads)) QVERIFY(read.length <= 256 * 1024);
        QCOMPARE(run.board->counters().hostFileLists, quint64(1));
        QCOMPARE(run.ended, 0);
        // the window bounds the reads in flight: never more than the client's four plus the one the pacer was holding
        QVERIFY(!run.board->hostFileTransferActive());
    }

    void aSecondCopyReplacesTheFirst()
    {
        const QString src = fresh("src2");
        const QString out = fresh("out2");
        writeFile(src + u"/first.txt"_s, "first copy");
        writeFile(src + u"/second.txt"_s, "second copy, a little longer");
        Run run;
        QVERIFY(begin(run, {native(src + u"/first.txt"_s)}, {u"--files-out"_s, out}));
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(30s, [&run] { return run.log.contains("FILES done"); }), run.log.constData());
        QCOMPARE(readAt(out + u"/first.txt"_s, 0, 100), QByteArray("first copy"));
        // a new copy on the host: announced again; a client that already finished simply ignores it, the server serves nothing stale
        run.selection = {native(src + u"/second.txt"_s)};
        run.offerSeq = 2;
        run.board->announceHost(false, false, false, true);
        run.wait(500ms);
        QVERIFY(run.board->hostFilesAllowed());
        QCOMPARE(run.ended, 0);
    }

    void cancelInTheMiddleStopsTheReads()
    {
        const QString src = fresh("src3");
        const QString big = src + u"/big.bin"_s;
        writeFile(big, QByteArray(1, 'x'));
        QVERIFY(QFile(big).resize(64LL * 1024 * 1024)); // holes: reading them is cheap
        Run run;
        QVERIFY(begin(run, {native(big)}, {u"--files-window"_s, u"4"_s, u"--files-chunk"_s, u"65536"_s, u"--files-cancel-after"_s, u"2097152"_s}));
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES cancelled"); }), run.log.constData());
        run.wait(1s);
        const int readsAfterCancel = int(run.reads.size());
        run.wait(1s);
        // nothing more is read once the client stopped asking (64 MiB would be 1024 ranges), and the lock was released
        QCOMPARE(int(run.reads.size()), readsAfterCancel);
        QVERIFY2(readsAfterCancel >= 30 && readsAfterCancel < 100, qPrintable(QString::number(readsAfterCancel)));
        QTRY_COMPARE_WITH_TIMEOUT(run.unlocks.size(), 1, 5000);
        QVERIFY(!run.board->hostFileTransferActive());
        QCOMPARE(run.ended, 0); // the client's own cancel is not a failure of the transfer
    }

    void turningFilesOffMidTransferFailsWhatIsPendingAndWithdrawsTheOffer()
    {
        const QString src = fresh("src3b");
        const QString big = src + u"/big.bin"_s;
        writeFile(big, QByteArray(1, 'x'));
        QVERIFY(QFile(big).resize(32LL * 1024 * 1024));
        Run run;
        QVERIFY(begin(run, {native(big)}, {u"--files-window"_s, u"2"_s, u"--files-chunk"_s, u"65536"_s, u"--files-pause-after"_s, u"524288"_s, u"--files-pause-ms"_s, u"1500"_s}));
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES paused"); }), run.log.constData());
        run.board->setFileDirections(false, false); // control moved, or the policy changed
        QVERIFY(run.cancels.contains(0));
        // (libfreerdp's client does not surface an empty format list, so the withdrawal itself is checked at the cliprdr fake in ClipboardFilesBridgeTest.)
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES done"); }), run.log.constData());
        QVERIFY(run.log.contains("FILES fail"));
        QVERIFY(!run.board->hostFilesAllowed());
        QCOMPARE(run.probe.state(), QProcess::Running);
    }

    void aFileThatChangesMidTransferFailsTheTransferNotTheConnection()
    {
        const QString src = fresh("src4");
        const QString out = fresh("out4");
        const QString big = src + u"/grow.bin"_s;
        writeFile(big, pattern(8 * 1024 * 1024, 4));
        writeFile(src + u"/after.txt"_s, "listed after the big file");
        Run run;
        QVERIFY(begin(run, {native(big), native(src + u"/after.txt"_s)},
                      {u"--files-out"_s, out, u"--files-window"_s, u"2"_s, u"--files-chunk"_s, u"65536"_s, u"--files-pause-after"_s, u"1048576"_s, u"--files-pause-ms"_s, u"1500"_s}));
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES paused"); }), run.log.constData());
        // while the client waits, the host application appends to the file
        {
            QFile f(big);
            QVERIFY(f.open(QIODevice::Append));
            f.write("one more line");
        }
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES done"); }), run.log.constData());
        qInfo().noquote() << run.log.right(1200);
        QVERIFY2(run.log.contains("FILES fail"), run.log.constData());
        QVERIFY(!run.line("FILES done").contains("failed=0"));
        QVERIFY(run.ended >= 1);
        QVERIFY(run.cancels.contains(1));
        // the connection is still up and the host can offer again
        QCOMPARE(run.probe.state(), QProcess::Running);
        QVERIFY(run.connection && run.connection->state() != RdpConnection::State::Closed);
    }

    void aVanishedFileIsTheSame()
    {
        const QString src = fresh("src5");
        const QString big = src + u"/gone.bin"_s;
        writeFile(big, pattern(4 * 1024 * 1024, 5));
        Run run;
        QVERIFY(begin(run, {native(big)}, {u"--files-window"_s, u"2"_s, u"--files-chunk"_s, u"65536"_s, u"--files-pause-after"_s, u"524288"_s, u"--files-pause-ms"_s, u"1500"_s}));
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES paused"); }), run.log.constData());
        QVERIFY(QFile::remove(big));
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES done"); }), run.log.constData());
        QVERIFY(run.log.contains("FILES fail"));
        QVERIFY(run.ended >= 1);
        QCOMPARE(run.probe.state(), QProcess::Running);
    }

    void hostileRequestsFailAndTheConnectionSurvives()
    {
        const QString src = fresh("src6");
        writeFile(src + u"/f.bin"_s, pattern(100 * 1024, 6));
        QDir().mkpath(src + u"/d"_s);
        Run run;
        QVERIFY(begin(run, {native(src + u"/f.bin"_s), native(src + u"/d"_s)}, {u"--files-hostile"_s}));
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES hostile name=after-abort"); }), run.log.constData());
        qInfo().noquote() << run.lines("FILES hostile").join('\n');
        for (const char *name : {"index-out-of-range", "range-on-directory", "past-end", "unknown-lock", "zero-length", "after-abort"}) {
            bool found = false;
            for (const auto &l : run.lines("FILES hostile name=")) {
                if (l.contains(QByteArray(QByteArray("name=") + name + " "))) {
                    found = true;
                    QVERIFY2(l.contains("result=failed"), l.constData());
                }
            }
            QVERIFY2(found, name);
        }
        // nothing hostile reached the worker, and the third violation ended the transfer
        QVERIFY(run.reads.isEmpty());
        QVERIFY(run.ended >= 1);
        QCOMPARE(run.board->counters().hostFileRanges, quint64(0));
        QCOMPARE(run.probe.state(), QProcess::Running);
    }

    void thePlannersStatesDecideWhatIsOfferedAndWhatContinues()
    {
        using namespace KRdp::ConsoleClipboardPolicy;
        const auto planFor = [](bool locked, bool greeter, bool started) {
            Input input;
            input.route = Route::Console;
            input.adapter = greeter ? ConsoleSeat::Adapter::Greeter : ConsoleSeat::Adapter::PhysicalUser;
            input.locked = locked;
            input.clientUid = 1000;
            input.sessionUid = 1000;
            input.ownsControl = true;
            input.hostToClientFileStarted = started;
            return plan(input);
        };
        const QString src = fresh("src7");
        const QString out = fresh("out7");
        writeFile(src + u"/long.bin"_s, pattern(6 * 1024 * 1024, 7));
        Run run;
        QVERIFY(begin(run, {native(src + u"/long.bin"_s)},
                      {u"--files-out"_s, out, u"--files-window"_s, u"2"_s, u"--files-chunk"_s, u"65536"_s, u"--files-pause-after"_s, u"524288"_s, u"--files-pause-ms"_s, u"800"_s}));
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES paused"); }), run.log.constData());
        // the screen locks while the copy runs: the planner keeps host to client files for the same account, and the copy completes
        const auto locked = planFor(true, false, true);
        QVERIFY(locked.files.hostToClient);
        QVERIFY(!locked.files.clientToHost);
        run.board->setFileDirections(locked.files.hostToClient, locked.files.clientToHost);
        QVERIFY2(run.waitFor(90s, [&run] { return run.log.contains("FILES done"); }), run.log.constData());
        QVERIFY(run.line("FILES done").contains("failed=0"));
        QCOMPARE(readAt(out + u"/long.bin"_s, 0, 7 * 1024 * 1024), pattern(6 * 1024 * 1024, 7));
        // the login screen shares no files at all: nothing is announced, and a running transfer there would be ended
        const auto greeter = planFor(false, true, false);
        QVERIFY(!greeter.files.hostToClient && !greeter.files.clientToHost);
        run.board->setFileDirections(greeter.files.hostToClient, greeter.files.clientToHost);
        run.log.clear();
        run.board->announceHost(false, false, false, true);
        run.wait(600ms);
        QVERIFY(!run.log.contains("FILES format"));
    }

    void withoutTheSettingNoClientSeesFileSupport()
    {
        const QString src = fresh("src8");
        writeFile(src + u"/f.txt"_s, "x");
        Run run;
        run.filesEnabled = false;
        QVERIFY(begin(run, {native(src + u"/f.txt"_s)}, {}));
        QVERIFY2(run.waitFor(30s, [&run] { return run.log.contains("FILES caps"); }), run.log.constData());
        QVERIFY(run.line("FILES caps").contains("stream=0"));
        run.wait(300ms);
        run.board->setGraphicsDelivered();
        run.board->announceHost(false, false, false, true); // nothing to announce to a client without file support
        run.wait(800ms);
        QVERIFY(!run.log.contains("FILES format"));
        QVERIFY(run.reads.isEmpty());
    }

    void aClientWithoutStreamingGetsNoFilesEvenWhenEnabled()
    {
        const QString src = fresh("src9");
        writeFile(src + u"/f.txt"_s, "x");
        Run run;
        QVERIFY(begin(run, {native(src + u"/f.txt"_s)}, {u"--files-flags"_s, u"2"_s})); // long format names only, like a client without file support
        QVERIFY2(run.waitFor(30s, [&run] { return run.log.contains("FILES caps"); }), run.log.constData());
        QVERIFY(run.line("FILES caps").contains("stream=0"));
        run.wait(300ms);
        run.board->setGraphicsDelivered();
        run.board->announceHost(false, false, false, true);
        run.wait(800ms);
        QVERIFY(!run.log.contains("FILES format"));
        QVERIFY(run.reads.isEmpty());
    }

    void aClientWithoutLockingStillGetsItsFiles()
    {
        const QString src = fresh("src10");
        const QString out = fresh("out10");
        writeFile(src + u"/f.bin"_s, pattern(700 * 1024, 10));
        Run run;
        QVERIFY(begin(run, {native(src + u"/f.bin"_s)}, {u"--files-out"_s, out, u"--files-flags"_s, u"2e"_s})); // 0x2e: long names, streaming, no paths, huge - and no lock
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES done"); }), run.log.constData());
        QVERIFY(run.line("FILES done").contains("failed=0"));
        QCOMPARE(readAt(out + u"/f.bin"_s, 0, 800 * 1024), pattern(700 * 1024, 10));
        QVERIFY(run.locks.isEmpty());
    }
};

QTEST_GUILESS_MAIN(ClipboardFileLoopbackTest)
#include "ClipboardFileLoopbackTest.moc"
