// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <QProcess>
#include <QScopeGuard>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <csignal>
using namespace Qt::StringLiterals;
class VirtualSessionOwnerWatchTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void deadlinesAndRetention_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("crash");
        QTest::newRow("startup") << u"startup"_s << true;
        QTest::newRow("closing") << u"closing"_s << true;
        QTest::newRow("retained") << u"retained"_s << false;
    }
    void deadlinesAndRetention() {
        QFETCH(QString, mode); QFETCH(bool, crash);
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + u"/OwnerWatchProbe"_s, {mode});
        QVERIFY(process.waitForFinished(3000));
        QCOMPARE(process.exitStatus(), crash ? QProcess::CrashExit : QProcess::NormalExit);
        QCOMPARE(process.exitCode(), crash ? int(SIGKILL) : 0);
    }
    void parentExitKillsOnlyMonitor() {
        // Reap our disposable grandchild instead of depending on PID1 behavior.
        QVERIFY(!prctl(PR_SET_CHILD_SUBREAPER, 1));
        const auto cleanup = qScopeGuard([] { prctl(PR_SET_CHILD_SUBREAPER, 0); });
        QProcess parent;
        parent.start(QCoreApplication::applicationDirPath() + u"/OwnerWatchProbe"_s, {u"parent-exit"_s});
        QVERIFY(parent.waitForFinished(3000)); QCOMPARE(parent.exitCode(), 0);
        bool valid = false; const auto monitor = parent.readAllStandardOutput().trimmed().toInt(&valid);
        QVERIFY(valid && monitor > 1);
        int status = 0;
        bool reaped = false;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            if (!reaped) reaped = waitpid(monitor, &status, WNOHANG) == monitor;
            return reaped;
        })(), 3000);
        QVERIFY(WIFSIGNALED(status)); QCOMPARE(WTERMSIG(status), SIGKILL);
        QVERIFY(!prctl(PR_SET_CHILD_SUBREAPER, 0));
    }
};
QTEST_GUILESS_MAIN(VirtualSessionOwnerWatchTest)
#include "VirtualSessionOwnerWatchTest.moc"
