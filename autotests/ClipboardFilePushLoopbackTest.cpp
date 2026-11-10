// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 3: client-to-host file copy end to end in one process pair. A real KRdp::Server and Clipboard, the worker's real file stager
// and staging file system behind them (fake host clipboard, temporary cache), and krdpctl-probe --clipboard push (libfreerdp's cliprdr client)
// copying: a directory tree byte for byte with Windows-hostile names renamed, the lock, no lock, a hostile name, the planner's denial, a full
// disk, a read that fails mid-way, and a larger file through the window. Nothing here is larger than 64 MiB (the 5 GiB case is on Sol).

#include <QDirIterator>

#include <sys/stat.h>

#include "ClipboardFilePushLoopbackHarness.h"
#include "HostClipboard.h"

using namespace PushLoopback;
using namespace Qt::StringLiterals;

class ClipboardFilePushLoopbackTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_certificate;
    QString m_key;

    QString fresh(const char *name)
    {
        const QString dir = m_dir.filePath(QString::fromLatin1(name));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        return dir;
    }
    bool begin(PushRun &run, const QString &from, const QStringList &arguments = {}) { return run.start(this, m_certificate, m_key, m_dir.path(), from, arguments); }
    static bool noH264(const PushRun &run) { return run.log.contains("does not support H.264"); }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        if (QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty()) QSKIP("openssl is needed to make the test certificate");
        m_certificate = makeCertificate(m_dir.path(), &m_key);
        QVERIFY(!m_certificate.isEmpty());
    }

    void aDirectoryTreeArrivesByteForByteWithTheLockTakenAndReleased()
    {
        const QString src = fresh("src1");
        writeFile(src + u"/album/a.txt"_s, "alpha");
        writeFile(src + u"/album/empty"_s, {});
        writeFile(src + u"/album/one.bin"_s, QByteArray(1, '\x7f'));
        writeFile(src + u"/album/sub/deep/b.bin"_s, pattern(300 * 1024 + 11, 1));
        writeFile(src + u"/album/sub/c.bin"_s, pattern(1024 * 1024 + 17, 2));
        writeFile(src + u"/CON.txt"_s, "device name");
        writeFile(src + u"/what?.txt"_s, "question");
        QDir().mkpath(src + u"/album/emptydir"_s);
        PushRun run;
        QVERIFY(begin(run, src));
        QVERIFY(run.letTheClipboardGo());
        QVERIFY2(run.waitFor(60s, [&run] { return !run.states.isEmpty() && run.states.last().phase == ConsoleWorkerWire::FilePushPhase::Done; }), run.log.right(3000).constData());
        qInfo().noquote() << run.log.right(800);
        QVERIFY2(!noH264(run), "this probe build cannot connect");
        QCOMPARE(run.replies.size(), 1);
        QVERIFY(run.replies.first().accepted);
        QCOMPARE(run.replies.first().files, quint32(7));
        // lock before the descriptor, every range request names it, released when the bytes were all in
        QCOMPARE(run.count("PUSH lock id="), 1);
        const auto lockLine = run.lines("PUSH lock id=").first();
        const QByteArray lockId = PushRun::field(lockLine, "id");
        QVERIFY(!lockId.isEmpty());
        QVERIFY2(run.log.indexOf("PUSH lock id=") < run.log.indexOf("PUSH descriptor"), "the clipboard is locked before the descriptor is requested");
        const auto requests = run.lines("PUSH request");
        QVERIFY(requests.size() >= 5);
        for (const QByteArray &request : requests) {
            QCOMPARE(PushRun::field(request, "lock"), lockId);
            QCOMPARE(PushRun::field(request, "flags"), QByteArray("2")); // FILECONTENTS_RANGE only
            QVERIFY(PushRun::field(request, "length").toUInt() <= 256 * 1024);
        }
        QVERIFY2(run.waitFor(5s, [&run] { return run.count("PUSH unlock id=") >= 1; }), "the lock is released");
        QCOMPARE(run.count("PUSH unlock id="), 1);
        QCOMPARE(PushRun::field(run.lines("PUSH unlock id=").first(), "id"), lockId);
        // every byte, with the names a Linux host keeps and the ones Windows would mangle renamed
        QCOMPARE(run.uriWrites.size(), 1);
        QStringList names;
        for (const QString &path : run.uriWrites.first()) names.append(QFileInfo(path).fileName());
        names.sort();
        QStringList expectedNames{u"_CON.txt"_s, u"album"_s, u"what_.txt"_s};
        expectedNames.sort();
        QCOMPARE(names, expectedNames);
        const QString album = run.staged(u"album"_s);
        QVERIFY(!album.isEmpty());
        QCOMPARE(readAt(album + u"/a.txt"_s, 0, 100), QByteArray("alpha"));
        QCOMPARE(readAt(album + u"/one.bin"_s, 0, 100), QByteArray(1, '\x7f'));
        QCOMPARE(QFileInfo(album + u"/empty"_s).size(), qint64(0));
        QCOMPARE(readAt(album + u"/sub/deep/b.bin"_s, 0, 400 * 1024), pattern(300 * 1024 + 11, 1));
        QCOMPARE(readAt(album + u"/sub/c.bin"_s, 0, 2 * 1024 * 1024), pattern(1024 * 1024 + 17, 2));
        QVERIFY(QFileInfo(album + u"/emptydir"_s).isDir());
        QCOMPARE(readAt(run.staged(u"_CON.txt"_s), 0, 100), QByteArray("device name"));
        QCOMPARE(readAt(run.staged(u"what_.txt"_s), 0, 100), QByteArray("question"));
        QCOMPARE(run.notes.size(), 1); // the user was told once when staging started
        QCOMPARE(run.board->counters().clientFileLists, quint64(1));
        // nothing but permitted modes
        QDirIterator it(run.staged(QString()), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            struct stat st {};
            QCOMPARE(::lstat(QFile::encodeName(it.filePath()).constData(), &st), 0);
            QVERIFY2(!(st.st_mode & 077), qPrintable(it.filePath()));
            if (S_ISREG(st.st_mode)) QVERIFY2(!(st.st_mode & 0111), qPrintable(it.filePath()));
        }
    }

    void withoutLockNegotiatedTheRequestsCarryNoLock()
    {
        const QString src = fresh("src2");
        writeFile(src + u"/f.bin"_s, pattern(600 * 1024, 3));
        PushRun run;
        // long names, stream, no paths, huge: no CB_CAN_LOCK_CLIPDATA
        QVERIFY(begin(run, src, {u"--files-flags"_s, u"2e"_s}));
        QVERIFY(run.letTheClipboardGo());
        QVERIFY2(run.waitFor(60s, [&run] { return !run.states.isEmpty() && run.states.last().phase == ConsoleWorkerWire::FilePushPhase::Done; }), run.log.right(3000).constData());
        QCOMPARE(run.count("PUSH lock id="), 0);
        QCOMPARE(run.count("PUSH unlock id="), 0);
        const auto requests = run.lines("PUSH request");
        QVERIFY(!requests.isEmpty());
        for (const QByteArray &request : requests) QCOMPARE(PushRun::field(request, "lock"), QByteArray("none"));
        QCOMPARE(readAt(run.staged(u"f.bin"_s), 0, 700 * 1024), pattern(600 * 1024, 3));
    }

    void aHostileNameIsRefusedBeforeAnythingIsStagedOrRequested()
    {
        const QString src = fresh("src3");
        writeFile(src + u"/fine.txt"_s, "fine");
        PushRun run;
        QVERIFY(begin(run, src, {u"--push-hostile-name"_s}));
        QVERIFY(run.letTheClipboardGo());
        QVERIFY2(run.waitFor(30s, [&run] { return run.log.contains("PUSH descriptor"); }), run.log.right(2000).constData());
        run.wait(1500ms);
        QCOMPARE(run.count("PUSH request"), 0);
        QVERIFY(run.replies.isEmpty()); // the broker refused it: the worker never saw it
        QVERIFY(!run.anyStaging());
        QCOMPARE(run.board->counters().clientFileRefused, quint64(1));
        QVERIFY2(run.waitFor(5s, [&run] { return run.count("PUSH unlock id=") >= 1; }), "the lock is released");
        QCOMPARE(run.count("PUSH unlock id="), 1); // the lock is released
        QVERIFY(run.uriWrites.isEmpty());
        QCOMPARE(run.probe.state(), QProcess::Running); // the session goes on
    }

    void theLockedDesktopDeniesANewTransferAndNothingIsRequested()
    {
        const QString src = fresh("src4");
        writeFile(src + u"/fine.txt"_s, "fine");
        PushRun run;
        run.clientAllowed = false;
        QVERIFY(begin(run, src));
        QVERIFY(run.letTheClipboardGo());
        run.wait(1500ms);
        QCOMPARE(run.count("PUSH descriptor"), 0);
        QCOMPARE(run.count("PUSH lock id="), 0);
        QVERIFY(!run.anyStaging());
    }

    void aFullDiskIsRefusedUpFrontWithTheNumbers()
    {
        const QString src = fresh("src5");
        writeFile(src + u"/big.bin"_s, pattern(2 * 1024 * 1024, 5));
        PushRun run;
        run.space = [](int) { return std::optional(HostFileStaging::SpaceInfo{1ULL << 20, 100ULL << 30}); };
        QVERIFY(begin(run, src));
        QVERIFY(run.letTheClipboardGo());
        QVERIFY2(run.waitFor(30s, [&run] { return !run.replies.isEmpty(); }), run.log.right(2000).constData());
        QVERIFY(!run.replies.first().accepted);
        QCOMPARE(run.replies.first().reason, ConsoleWorkerWire::FilePushReason::NoSpace);
        QCOMPARE(run.replies.first().freeBytes, quint64(1ULL << 20));
        run.wait(500ms);
        QCOMPARE(run.count("PUSH request"), 0); // not one byte was asked of the client
        QVERIFY(!run.anyStaging());
        QCOMPARE(run.notes.size(), 1);
        QVERIFY(run.notes.first().second.contains(u"space"_s, Qt::CaseInsensitive));
        QVERIFY2(run.waitFor(5s, [&run] { return run.count("PUSH unlock id=") >= 1; }), "the lock is released");
        QCOMPARE(run.count("PUSH unlock id="), 1);
    }

    void aReadThatFailsOnTheClientEndsTheTransferAndRemovesTheStaging()
    {
        const QString src = fresh("src6");
        writeFile(src + u"/big.bin"_s, pattern(2 * 1024 * 1024, 6));
        PushRun run;
        QVERIFY(begin(run, src, {u"--push-fail-read"_s}));
        QVERIFY(run.letTheClipboardGo());
        QVERIFY2(run.waitFor(30s, [&run] { return run.board && run.board->counters().clientFileFailed > 0; }), run.log.right(2000).constData());
        QTRY_VERIFY_WITH_TIMEOUT(!run.board->clientFileTransferActive(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!run.anyStaging(), 5000);
        QVERIFY(run.uriWrites.isEmpty());
        QVERIFY(run.waitFor(5s, [&run] { return run.count("PUSH unlock id=") >= 1; }));
        QCOMPARE(run.count("PUSH unlock id="), 1);
        QCOMPARE(run.probe.state(), QProcess::Running);
    }

    // OPT-078 echo guard: the files a real transfer staged come back as a host clipboard change that somebody else owns (a clipboard
    // manager, KDE Connect). The worker's HostClipboard must not offer them to the client again: no offer, so no FileContents request.
    void reownedStagedFilesAreNotOfferedBackAfterATransfer()
    {
        const QString src = fresh("src8");
        writeFile(src + u"/album/a.txt"_s, "alpha");
        writeFile(src + u"/album/sub/b.bin"_s, pattern(70 * 1024 + 3, 8));
        PushRun run;
        QVERIFY(begin(run, src));
        QVERIFY(run.letTheClipboardGo());
        QVERIFY2(run.waitFor(60s, [&run] { return !run.states.isEmpty() && run.states.last().phase == ConsoleWorkerWire::FilePushPhase::Done; }), run.log.right(3000).constData());
        QVERIFY2(!noH264(run), "this probe build cannot connect");
        QCOMPARE(run.uriWrites.size(), 1);
        const QString stagedAlbum = run.staged(u"album"_s);
        QVERIFY(QFileInfo(stagedAlbum).isDir());
        const int requestsBefore = run.count("PUSH request");

        // The worker's host-side clipboard, as the consoleworker builds it, over the same cache directory.
        QByteArray list;
        for (const QString &path : run.uriWrites.first()) list += QUrl::fromLocalFile(path).toEncoded() + "\r\n";
        int frames = 0;
        ConsoleWorkerWire::Deframer outgoing;
        int offers = 0;
        HostClipboard::Backend backend;
        backend.hasText = [] { return false; };
        backend.readText = [] { return QString(); };
        backend.writeText = [](const QString &, bool) { return false; };
        backend.hasUriList = [&list] { return !list.isEmpty(); };
        backend.readUriList = [&list] { return list; };
        backend.ownsUriList = [] { return false; }; // somebody else owns the list now
        backend.cacheDir = [&run] { return run.cache.path(); };
        HostClipboard hostBoard(backend, [&](const QByteArray &frame) {
            ++frames;
            outgoing.feed(frame);
            while (const auto record = outgoing.next()) {
                if (ConsoleWorkerWire::clipboardOffer(*record)) ++offers;
            }
        }, true);
        hostBoard.setControl({1, true});
        ConsoleWorkerWire::ClipboardPolicy policy;
        policy.revision = 1;
        policy.controlGeneration = 1;
        policy.hostToClient = true;
        policy.clientToHost = true;
        policy.filesHostToClient = true;
        policy.filesClientToHost = true;
        hostBoard.setPolicy(policy);
        hostBoard.hostChanged();
        run.wait(std::chrono::milliseconds(HostClipboard::SettleMs * 4));
        QCOMPARE(offers, 0);
        QCOMPARE(frames, 0);
        QCOMPARE(run.count("PUSH request"), requestsBefore); // and the client was asked for nothing more
        // the same host clipboard with a file from outside the staging folder is a copy like any other
        const QString outside = m_dir.filePath(u"outside.txt"_s);
        writeFile(outside, "x");
        list = QUrl::fromLocalFile(outside).toEncoded() + "\r\n";
        hostBoard.hostChanged();
        QVERIFY(run.waitFor(5s, [&offers] { return offers == 1; }));
    }

    void aLargerFileMovesThroughTheWindowByteForByte()
    {
        const QString src = fresh("src7");
        const qsizetype size = 40 * 1024 * 1024 + 333;
        writeFile(src + u"/large.bin"_s, pattern(size, 7));
        PushRun run;
        QVERIFY(begin(run, src));
        QVERIFY(run.letTheClipboardGo());
        QVERIFY2(run.waitFor(120s, [&run] { return !run.states.isEmpty() && run.states.last().phase == ConsoleWorkerWire::FilePushPhase::Done; }), run.log.right(1500).constData());
        QCOMPARE(run.states.last().totalBytes, quint64(size));
        QCOMPARE(sha256(readAt(run.staged(u"large.bin"_s), 0, quint32(size))), sha256(pattern(size, 7)));
        QCOMPARE(run.board->counters().clientFileBytes, quint64(size));
        QVERIFY(run.count("PUSH request") >= size / (256 * 1024));
    }
};

QTEST_MAIN(ClipboardFilePushLoopbackTest)
#include "ClipboardFilePushLoopbackTest.moc"
