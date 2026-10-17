// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 2: a file over 4 GiB through the whole stack (libfreerdp client, cliprdr, the broker's state machine, the worker's file
// source). The file is SPARSE (ftruncate plus a few marker writes: no multi-GB file is ever written); a file system without sparse support
// ends the test with exit code 77 (ctest SKIP_RETURN_CODE). The client samples the first chunk, the chunk across 4 GiB and the last chunk,
// and the tests compare their SHA-256 with the same ranges read from the file. A second run proves the huge-file flag is what allows it.
// Then the whole file goes through the stack (about a minute in a Debug build on loopback; KRDP_LOOPBACK_SKIP_FULL=1 leaves it out).

#include "ClipboardFileLoopbackHarness.h"

using namespace LoopbackHarness;

class ClipboardFileLargeLoopbackTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_certificate;
    QString m_key;
    QString m_big;
    static constexpr quint64 Size = (quint64(5) << 30) + 12345; // 5 GiB and a bit
    static constexpr quint32 Chunk = 256 * 1024;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_certificate = makeCertificate(m_dir.path(), &m_key);
        QVERIFY(!m_certificate.isEmpty());
        m_big = m_dir.filePath(QStringLiteral("huge.bin"));
        QFile file(m_big);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(qint64(Size)));
        const auto mark = [&file](quint64 at, const QByteArray &bytes) {
            return file.seek(qint64(at)) && file.write(bytes) == bytes.size();
        };
        QVERIFY(mark(0, "HEAD-OF-THE-FILE"));
        QVERIFY(mark((quint64(4) << 30) - 3, "ABCDEFG")); // crosses 4 GiB
        QVERIFY(mark((quint64(4) << 30) + 777, "past 4 GiB"));
        QVERIFY(mark(Size - 9, "TAIL-DATA"));
    }

    void aFileOverFourGiBArrivesInSampledRangesWithTheirOffsetsIntact()
    {
        Run run;
        run.selection = {QFile::encodeName(m_big), QFile::encodeName(m_dir.filePath(QStringLiteral("huge.bin")))};
        run.selection.removeLast(); // one file
        QVERIFY(run.start(this, m_certificate, m_key, m_dir.path(), {QStringLiteral("--files-chunk"), QString::number(Chunk)}));
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(120s, [&run] { return run.log.contains("FILES done"); }), run.log.constData());
        qInfo().noquote() << run.log.right(1500);
        QVERIFY(run.line("FILES caps").contains("huge=1"));
        QVERIFY2(run.line("FILES descriptor").contains(QByteArray(QByteArray("bytes=") + QByteArray::number(Size))), run.line("FILES descriptor").constData());
        QVERIFY(run.line("FILES done").contains("failed=0"));
        const auto samples = run.lines("FILES sample");
        QCOMPARE(samples.size(), 3);
        const quint64 offsets[3] = {0, (quint64(1) << 32) - Chunk / 2, Size - Chunk};
        for (int i = 0; i < 3; ++i) {
            const auto offset = Run::field(samples.at(i), "offset").toULongLong();
            QCOMPARE(offset, offsets[i]);
            QCOMPARE(Run::field(samples.at(i), "sha256"), sha256(readAt(m_big, offset, Chunk)));
        }
        // the markers really are in the sampled ranges (the hash of zeros would also match an empty read, so check the content too)
        QVERIFY(readAt(m_big, offsets[1], Chunk).contains("ABCDEFG"));
        QVERIFY(readAt(m_big, offsets[2], Chunk).contains("TAIL-DATA"));
        // the broker asked the worker for offsets beyond 2^32, and the worker was asked for exactly the sampled ranges
        bool beyond = false;
        for (const auto &read : std::as_const(run.reads)) beyond = beyond || read.offset + read.length > (quint64(1) << 32);
        QVERIFY(beyond);
        QCOMPARE(run.reads.size(), 3);
        QCOMPARE(run.bytesRead, quint64(3) * Chunk);
        QCOMPARE(run.board->counters().hostFileRanges, quint64(3));
    }

    void withoutHugeSupportTheFileIsLeftOutNotTruncated()
    {
        Run run;
        run.selection = {QFile::encodeName(m_big)};
        const QString small = m_dir.filePath(QStringLiteral("small.txt"));
        writeFile(small, "fits");
        run.selection.append(QFile::encodeName(small));
        // a client that does not offer huge-file support: the worker is told, skips the file, and counts it
        QVERIFY(run.start(this, m_certificate, m_key, m_dir.path(), {QStringLiteral("--files-flags"), QStringLiteral("1e")}));
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(60s, [&run] { return run.log.contains("FILES done"); }), run.log.constData());
        QVERIFY(run.line("FILES caps").contains("huge=0"));
        QVERIFY(run.line("FILES descriptor").contains("files=1"));
        QCOMPARE(run.walked.skipped, 1u);
        QVERIFY(run.line("FILES done").contains("failed=0"));
    }

    void theWholeFileThroughTheStack()
    {
        if (qEnvironmentVariableIntValue("KRDP_LOOPBACK_SKIP_FULL") == 1) QSKIP("KRDP_LOOPBACK_SKIP_FULL=1");
        Run run;
        run.selection = {QFile::encodeName(m_big)};
        QVERIFY(run.start(this, m_certificate, m_key, m_dir.path(), {QStringLiteral("--files-sample-over"), QString::number(Size + 1), QStringLiteral("--files-window"), QStringLiteral("8")}));
        QVERIFY(run.announceFiles());
        QVERIFY2(run.waitFor(std::chrono::minutes(8), [&run] { return run.log.contains("FILES done"); }), run.log.right(2000).constData());
        QVERIFY(run.line("FILES done").contains("failed=0"));
        QCOMPARE(run.bytesRead, Size);
    }
};

int main(int argc, char **argv)
{
    // A file system without sparse files would turn this into a 5 GiB write: skip (ctest: SKIP_RETURN_CODE 77).
    QTemporaryDir probeDir;
    if (!probeDir.isValid() || !sparseSupported(probeDir.path())) {
        fprintf(stderr, "SKIP: the temporary directory's file system has no sparse file support\n");
        return 77;
    }
    if (QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty()) {
        fprintf(stderr, "SKIP: openssl is needed to make the test certificate\n");
        return 77;
    }
    QCoreApplication app(argc, argv);
    ClipboardFileLargeLoopbackTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ClipboardFileLargeLoopbackTest.moc"
