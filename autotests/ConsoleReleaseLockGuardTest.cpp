// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "ConsoleReleaseLockGuard.h"

using namespace KRdp::ConsoleReleaseLock;
Q_DECLARE_METATYPE(std::optional<qint64>)

class ConsoleReleaseLockGuardTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // OPT-060 D1: the kernel truncates comm to 15 characters, so the greeter shows up as `kscreenlocker_g`.
    void greeterNameIsComparedAtTheKernelCommLength_data()
    {
        QTest::addColumn<QByteArray>("comm");
        QTest::addColumn<bool>("matches");
        QTest::newRow("truncated, as the kernel reports it on Sol") << QByteArray("kscreenlocker_g") << true;
        QTest::newRow("the full name (a stand-in or a longer comm)") << QByteArray("kscreenlocker_greet") << true;
        QTest::newRow("another process with the same prefix and a different length") << QByteArray("kscreenlocker_gX") << false;
        QTest::newRow("shorter prefix") << QByteArray("kscreenlocker_") << false;
        QTest::newRow("longer than the name") << QByteArray("kscreenlocker_greeter") << false;
        QTest::newRow("empty") << QByteArray() << false;
        QTest::newRow("unrelated") << QByteArray("kwin_wayland") << false;
    }
    void greeterNameIsComparedAtTheKernelCommLength()
    {
        QFETCH(QByteArray, comm); QFETCH(bool, matches);
        QCOMPARE(greeterCommMatches(std::string_view(comm.constData(), size_t(comm.size()))), matches);
        // The real name is 19 characters: it can never equal the 15-character comm (the D1 bug).
        QVERIFY(GreeterName.size() > KernelCommLength);
    }
    void greeterCmdlineConfirmsTheTruncatedMatch()
    {
        const auto matches = [](const QByteArray &cmdline) { return greeterCmdlineMatches(std::string_view(cmdline.constData(), size_t(cmdline.size()))); };
        QVERIFY(matches(QByteArray("/usr/lib/x86_64-linux-gnu/libexec/kscreenlocker_greet\0--graceTime\0 5000\0", 62)));
        QVERIFY(matches(QByteArray("kscreenlocker_greet")));
        QVERIFY(!matches(QByteArray("/usr/bin/kscreenlocker_g\0", 25)));
        QVERIFY(!matches(QByteArray("/usr/bin/other\0/kscreenlocker_greet", 36))); // only argv[0] counts
        QVERIFY(!matches(QByteArray()));
    }
    // The scan itself, on a /proc-shaped tree: comm truncated like the kernel's, cmdline with the real name.
    void greeterScanFindsTheTruncatedComm()
    {
        QTemporaryDir root; QVERIFY(root.isValid());
        const auto make = [&root](const QString &pid, const QByteArray &comm, const QByteArray &cmdline) {
            QVERIFY(QDir().mkpath(root.filePath(pid)));
            QFile c(root.filePath(pid + QStringLiteral("/comm"))); QVERIFY(c.open(QIODevice::WriteOnly)); c.write(comm + '\n'); c.close();
            QFile l(root.filePath(pid + QStringLiteral("/cmdline"))); QVERIFY(l.open(QIODevice::WriteOnly)); l.write(cmdline); l.close();
        };
        const std::filesystem::path path(root.path().toStdString());
        QVERIFY(!greeterRunning(path, getuid())); // empty tree
        make(QStringLiteral("100"), "kwin_wayland", "/usr/bin/kwin_wayland");
        make(QStringLiteral("self"), "kscreenlocker_g", "/usr/lib/libexec/kscreenlocker_greet"); // not a pid directory
        QVERIFY(!greeterRunning(path, getuid()));
        make(QStringLiteral("200"), "kscreenlocker_g", "/usr/bin/impostor"); // right comm, wrong program
        QVERIFY(!greeterRunning(path, getuid()));
        make(QStringLiteral("300"), "kscreenlocker_g", QByteArray("/usr/lib/x86_64-linux-gnu/libexec/kscreenlocker_greet\0--graceTime\0" "5000\0", 71));
        QVERIFY(greeterRunning(path, getuid()));
        QVERIFY(!greeterRunning(path, getuid() + 1)); // another user's greeter does not count
        QVERIFY(!greeterRunning(std::filesystem::path(root.filePath(QStringLiteral("missing")).toStdString()), getuid()));
    }
    // And against the real kernel: a process whose program is called kscreenlocker_greet shows up in /proc as the
    // 15-character comm, and is found; it is gone once it exits. (Anything else of ours called that would also count.)
    void greeterScanFindsARealProcessAndNotAfterItExits()
    {
        if (!QFileInfo::exists(QStringLiteral("/usr/bin/perl"))) QSKIP("no perl to stand in for a native program");
        if (greeterRunning("/proc", getuid())) QSKIP("a real kscreenlocker_greet of this user is running; the negative half cannot be checked");
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString fake = dir.filePath(QStringLiteral("kscreenlocker_greet"));
        QVERIFY(QFile::copy(QStringLiteral("/usr/bin/perl"), fake));
        QVERIFY(QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QProcess process; process.start(fake, {QStringLiteral("-e"), QStringLiteral("sleep 30")});
        QVERIFY(process.waitForStarted(3000));
        QFile comm(QStringLiteral("/proc/%1/comm").arg(process.processId())); QVERIFY(comm.open(QIODevice::ReadOnly));
        QCOMPARE(comm.readAll().trimmed(), QByteArray("kscreenlocker_g")); // the kernel truncation the old comparison missed
        QVERIFY(greeterRunning("/proc", getuid()));
        process.kill(); QVERIFY(process.waitForFinished(3000));
        QVERIFY(!greeterRunning("/proc", getuid()));
    }
    // With the greeter recognised (it is alive after a release) no re-lock is requested and nothing is warned about.
    void aLiveGreeterNeedsNoRelockAfterTheRelease()
    {
        Context context; context.lockedBefore = true;
        const bool alive = greeterCommMatches("kscreenlocker_g");
        const auto verdict = afterRestore(context, Observation{true, alive}, 0, FirstCheckDelayMs);
        QCOMPARE(int(verdict.step), int(Step::Done));
        // The old behaviour (a never-matching name reads as "gone") would have requested all three re-locks and then warned.
        const auto before = afterRestore(context, Observation{true, false}, 0, FirstCheckDelayMs);
        QCOMPARE(int(before.step), int(Step::Relock));
        QCOMPARE(int(afterRestore(context, Observation{true, false}, MaxRelockAttempts, FirstCheckDelayMs + 3 * RelockWaitMs).step), int(Step::GiveUp));
    }

    void hold_data()
    {
        QTest::addColumn<std::optional<qint64>>("since");
        QTest::addColumn<qint64>("held");
        QTest::addColumn<qint64>("wait");
        QTest::newRow("no lock edge seen") << std::optional<qint64>() << qint64(0) << qint64(0);
        QTest::newRow("lock 0 ms ago waits the full settle") << std::optional<qint64>(0) << qint64(0) << qint64(2000);
        QTest::newRow("lock 500 ms ago") << std::optional<qint64>(500) << qint64(0) << qint64(1500);
        QTest::newRow("lock 1999 ms ago") << std::optional<qint64>(1999) << qint64(0) << qint64(1);
        QTest::newRow("lock exactly 2 s ago: proceed") << std::optional<qint64>(2000) << qint64(0) << qint64(0);
        QTest::newRow("lock long ago: proceed") << std::optional<qint64>(60000) << qint64(0) << qint64(0);
        QTest::newRow("clock went backwards: proceed") << std::optional<qint64>(-5) << qint64(0) << qint64(0);
        QTest::newRow("total hold is capped") << std::optional<qint64>(0) << qint64(1000) << qint64(1500);
        QTest::newRow("already held to the cap: no more") << std::optional<qint64>(0) << qint64(2500) << qint64(0);
    }
    void hold()
    {
        QFETCH(std::optional<qint64>, since); QFETCH(qint64, held); QFETCH(qint64, wait);
        QCOMPARE(holdBeforeRelease(since, held), wait);
    }

    void verdict_data()
    {
        QTest::addColumn<bool>("before"); QTest::addColumn<bool>("during");
        QTest::addColumn<int>("locked"); // -1 unknown, 0 no, 1 yes
        QTest::addColumn<int>("greeter"); // -1 unknown, 0 dead, 1 alive
        QTest::addColumn<int>("attempts"); QTest::addColumn<qint64>("elapsed");
        QTest::addColumn<int>("step");
        const int done = int(Step::Done), relock = int(Step::Relock), giveUp = int(Step::GiveUp);
        QTest::newRow("never locked, unlocked") << false << false << 0 << 1 << 0 << qint64(0) << done;
        QTest::newRow("never locked, greeter dead is irrelevant") << false << false << 0 << 0 << 0 << qint64(0) << done;
        QTest::newRow("locked before, still locked, greeter alive") << true << false << 1 << 1 << 0 << qint64(0) << done;
        QTest::newRow("locked before, greeter state unknown") << true << false << 1 << -1 << 0 << qint64(0) << done;
        QTest::newRow("locked before, greeter dead (OPT-049)") << true << false << 1 << 0 << 0 << qint64(0) << relock;
        QTest::newRow("locked before, now unlocked") << true << false << 0 << 1 << 0 << qint64(0) << relock;
        QTest::newRow("lock started during the window, greeter killed") << false << true << 0 << 0 << 0 << qint64(300) << relock;
        QTest::newRow("lock started during the window, holds") << false << true << 1 << 1 << 0 << qint64(300) << done;
        QTest::newRow("no answer from any source: try to lock") << true << false << -1 << -1 << 0 << qint64(0) << relock;
        QTest::newRow("second attempt still failing") << true << false << 0 << 0 << 1 << qint64(1500) << relock;
        QTest::newRow("third attempt still failing") << true << false << 0 << 0 << 2 << qint64(3000) << relock;
        QTest::newRow("attempts exhausted: give up") << true << false << 0 << 0 << 3 << qint64(4500) << giveUp;
        QTest::newRow("time bound reached: give up") << true << false << 0 << 0 << 1 << qint64(9000) << giveUp;
        QTest::newRow("holds on the last attempt: done not give up") << true << false << 1 << 1 << 3 << qint64(9500) << done;
    }
    void verdict()
    {
        QFETCH(bool, before); QFETCH(bool, during); QFETCH(int, locked); QFETCH(int, greeter);
        QFETCH(int, attempts); QFETCH(qint64, elapsed); QFETCH(int, step);
        const auto toOptional = [](int value) { return value < 0 ? std::optional<bool>() : std::optional<bool>(value == 1); };
        const auto verdict = afterRestore({before, during}, {toOptional(locked), toOptional(greeter)}, attempts, elapsed);
        QCOMPARE(int(verdict.step), step);
        QVERIFY(qstrlen(verdict.reason) > 0);
    }

    void neverLoopsForever()
    {
        // A permanently failing re-lock ends in GiveUp after exactly MaxRelockAttempts requests.
        int attempts = 0; qint64 elapsed = 0; int guard = 0;
        while (guard++ < 100) {
            const auto verdict = afterRestore({true, false}, {false, false}, attempts, elapsed);
            if (verdict.step == Step::GiveUp) break;
            QCOMPARE(int(verdict.step), int(Step::Relock));
            ++attempts; elapsed += RelockWaitMs;
        }
        QCOMPARE(attempts, MaxRelockAttempts);
        QVERIFY(elapsed <= TotalBoundMs);
    }
};

QTEST_GUILESS_MAIN(ConsoleReleaseLockGuardTest)
#include "ConsoleReleaseLockGuardTest.moc"
