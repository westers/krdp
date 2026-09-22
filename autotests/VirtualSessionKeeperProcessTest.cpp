// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionKeeperProcess.h"
#include "VirtualSessionProcessIdentity.h"
#include <QTest>
#include <QProcess>
#include <QStandardPaths>
#include <sys/syscall.h>
#include <unistd.h>
using namespace KRdp;
class VirtualSessionKeeperProcessTest : public QObject {
    Q_OBJECT
    static std::optional<VirtualSessionJournal::Keeper> identity(pid_t pid) {
        const int fd = int(syscall(SYS_pidfd_open, pid, 0));
        const auto inode = virtualPidfdIdentity(fd); if (fd >= 0) close(fd);
        const auto ticks = virtualProcessStartTime(pid);
        if (!inode || !ticks) return {};
        return VirtualSessionJournal::Keeper{pid, *ticks, *inode};
    }
private Q_SLOTS:
    void pinsOnlyExactLiveProcess() {
        QProcess child;
        child.start(QStandardPaths::findExecutable(QStringLiteral("sleep")), {QStringLiteral("10")});
        QVERIFY(child.waitForStarted(1000));
        const auto record = identity(pid_t(child.processId())); QVERIFY(record);
        auto pinned = VirtualSessionKeeperProcess::pin(*record); QVERIFY(pinned);
        QCOMPARE(pinned->gone(), std::optional<bool>(false));
        auto wrongTicks = *record; ++wrongTicks.startTicks;
        QVERIFY(!VirtualSessionKeeperProcess::pin(wrongTicks));
        // Same PID/ticks, different kernel generation: original is gone and
        // the replacement must neither be killed nor waited on.
        auto wrongGeneration = *record; ++wrongGeneration.pidInode;
        auto replaced = VirtualSessionKeeperProcess::pin(wrongGeneration); QVERIFY(replaced);
        QCOMPARE(replaced->gone(), std::optional<bool>(true)); QVERIFY(replaced->kill());
        QVERIFY(!child.waitForFinished(50)); QCOMPARE(child.state(), QProcess::Running);
        QVERIFY(pinned->kill()); QVERIFY(child.waitForFinished(1000));
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);
        QCOMPARE(pinned->gone(), std::optional<bool>(true));
        // Reopening after reaping never rediscovers a replacement as original.
        auto gone = VirtualSessionKeeperProcess::pin(*record); QVERIFY(gone);
        QCOMPARE(gone->gone(), std::optional<bool>(true)); QVERIFY(gone->kill());
    }
    void rejectsMissingIdentityAndSelf() {
        QVERIFY(!VirtualSessionKeeperProcess::pin({}));
        QVERIFY(!VirtualSessionKeeperProcess::pin({1, 1, 2}));
        const auto self = identity(getpid()); QVERIFY(self);
        QVERIFY(!VirtualSessionKeeperProcess::pin(*self));
    }
};
QTEST_GUILESS_MAIN(VirtualSessionKeeperProcessTest)
#include "VirtualSessionKeeperProcessTest.moc"
