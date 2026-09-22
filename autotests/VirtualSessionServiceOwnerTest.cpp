// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionServiceOwner.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <csignal>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
using namespace KRdp;
using namespace Qt::StringLiterals;
class VirtualSessionServiceOwnerTest : public QObject {
    Q_OBJECT
    static QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    const QString helper = QString::fromLocal8Bit(SERVICE_OWNER_CHILD);
    const VirtualSessionServiceOwner::Timing timing{300, 200, 300, 10, 30};
    struct Fixture {
        QTemporaryDir directory;
        VirtualSessionJournal::Record record{1000, id(), id(), id(), id(), QByteArray(32, 's')};
        std::optional<bool> gone = true;
        bool validKeeper = true;
        pid_t keeper = 0;
        int result = -1, signals = 0, emergencies = 0;
        QStringList events() const {
            QFile file(directory.filePath(u"events"_s)); if (!file.open(QIODevice::ReadOnly)) return {};
            QStringList result;
            for (const auto &line : file.readAll().split('\n')) {
                if (line.isEmpty()) continue;
                const auto obj = QJsonDocument::fromJson(line).object();
                result.append(obj.value(u"valid"_s).toBool() ? obj.value(u"event"_s).toString() : u"INVALID"_s);
            }
            return result;
        }
        VirtualSessionServiceOwner::Checks checks() {
            return {[this](pid_t pid, const QString &login) {
                keeper = pid; return validKeeper && pid > 1 && login == u"fixture1"_s;
            }, [this] { return gone; }, [this](int signal) { if (signal == SIGKILL) ++signals; return false; },
                [this] { ++emergencies; }};
        }
        QString keeperPath(const QString &helper, const QString &mode) {
            const auto path = directory.filePath(mode);
            return QFile::link(helper, path) ? path : QString();
        }
        VirtualSessionServicePlan plan(const QString &helper, const QString &mode = u"normal"_s) const {
            return {helper, {u"desktop"_s, directory.path(), mode}, {}, record.token};
        }
    };
private Q_SLOTS:
    void normalOrderedShutdownAndCleanExec() {
        Fixture f; QVERIFY(f.directory.isValid());
        sigset_t mask, oldMask;
        sigemptyset(&mask); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGINT);
        QVERIFY(!sigprocmask(SIG_BLOCK, &mask, &oldMask));
        const auto restoreMask = qScopeGuard([&] { sigprocmask(SIG_SETMASK, &oldMask, nullptr); });
        // Explicitly inheritable sentinel: neither child may retain it.
        const int source = open("/dev/null", O_RDONLY); QVERIFY(source >= 0);
        QVERIFY(dup2(source, 200) == 200); close(source);
        const auto cleanup = qScopeGuard([] { close(200); });
        VirtualSessionServiceOwner owner(f.checks(), [&](int result) { f.result = result; });
        QVERIFY(owner.start(f.record, f.keeperPath(helper, u"normal"_s), f.plan(helper), timing));
        QTRY_VERIFY_WITH_TIMEOUT(f.events().contains(u"desktop-start"_s), 2000);
        QVERIFY(!f.events().contains(u"INVALID"_s));
        owner.stop(); owner.stop();
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 0, 2000);
        const auto events = f.events();
        QVERIFY(events.indexOf(u"keeper-ready"_s) < events.indexOf(u"desktop-start"_s));
        QVERIFY(events.indexOf(u"desktop-exit"_s) < events.indexOf(u"keeper-stop"_s));
        QCOMPARE(events.count(u"keeper-stop"_s), 1); QCOMPARE(f.signals, 0);
        QCOMPARE(owner.phase(), VirtualSessionServiceOwner::Phase::Finished);
    }
    void rejectedReadyNeverStartsDesktop_data() {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"bad-launch", "bad-uid", "bad-leader", "oversized", "malformed"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void rejectedReadyNeverStartsDesktop() {
        QFETCH(QString, mode); Fixture f;
        VirtualSessionServiceOwner owner(f.checks(), [&](int result) { f.result = result; });
        QVERIFY(owner.start(f.record, f.keeperPath(helper, mode), f.plan(helper), timing));
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 1, 2000);
        QVERIFY(!f.events().contains(u"desktop-start"_s)); QCOMPARE(f.signals, 0);
    }
    void fragmentAndStopDuringOpening() {
        Fixture f;
        VirtualSessionServiceOwner owner(f.checks(), [&](int result) { f.result = result; });
        QVERIFY(owner.start(f.record, f.keeperPath(helper, u"fragment"_s), f.plan(helper), timing));
        owner.stop();
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 0, 2000);
        QVERIFY(f.events().contains(u"keeper-stop"_s));
        QVERIFY(!f.events().contains(u"desktop-start"_s)); QCOMPARE(f.signals, 0);
    }
    void guardianExitDoesNotProveExtinction() {
        Fixture f;
        VirtualSessionServiceOwner owner(f.checks(), [&](int result) { f.result = result; });
        QVERIFY(owner.start(f.record, f.keeperPath(helper, u"normal"_s), f.plan(helper), timing));
        QTRY_VERIFY_WITH_TIMEOUT(f.events().contains(u"desktop-start"_s), 2000);
        f.gone = std::nullopt; owner.stop();
        QTRY_VERIFY_WITH_TIMEOUT(f.events().contains(u"desktop-exit"_s), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(f.signals > 0, 1000);
        QCOMPARE(f.result, -1); QVERIFY(!f.events().contains(u"keeper-stop"_s));
        f.gone = false;
        QTest::qWait(40); QCOMPARE(f.result, -1); QVERIFY(!f.events().contains(u"keeper-stop"_s));
        f.gone = true;
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 1, 2000);
        QVERIFY(f.events().contains(u"keeper-stop"_s));
    }
    void loginRemovalStopsDesktop() {
        Fixture f;
        VirtualSessionServiceOwner owner(f.checks(), [&](int result) { f.result = result; });
        QVERIFY(owner.start(f.record, f.keeperPath(helper, u"normal"_s), f.plan(helper), timing));
        QTRY_VERIFY_WITH_TIMEOUT(f.events().contains(u"desktop-start"_s), 2000);
        f.validKeeper = false;
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 1, 2000);
        const auto events = f.events();
        QVERIFY(events.contains(u"desktop-exit"_s));
        QVERIFY(events.indexOf(u"desktop-exit"_s) < events.indexOf(u"keeper-stop"_s));
    }
    void boundedUncertainExtinctionRequestsEmergency() {
        Fixture f;
        VirtualSessionServiceOwner owner(f.checks(), [&](int result) { f.result = result; });
        auto fast = timing; fast.graceful = 50; fast.emergency = 150;
        QVERIFY(owner.start(f.record, f.keeperPath(helper, u"normal"_s), f.plan(helper), fast));
        QTRY_VERIFY_WITH_TIMEOUT(f.events().contains(u"desktop-start"_s), 2000);
        f.gone = std::nullopt; owner.stop();
        QTRY_COMPARE_WITH_TIMEOUT(f.emergencies, 1, 1000);
        QTest::qWait(40); QCOMPARE(f.emergencies, 1);
        QCOMPARE(f.result, -1); QVERIFY(!f.events().contains(u"keeper-stop"_s));
        // Fixture restores certainty; production emergency instead asks
        // systemd to stop and has a bounded failed-exit fallback.
        f.gone = true;
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 1, 2000);
    }
    void slowReadyValidationCannotBypassDeadline() {
        Fixture f; auto checks = f.checks();
        checks.keeper = [](pid_t, const QString &) { usleep(100000); return true; };
        VirtualSessionServiceOwner owner(checks, [&](int result) { f.result = result; });
        auto fast = timing; fast.opening = 50;
        QVERIFY(owner.start(f.record, f.keeperPath(helper, u"normal"_s), f.plan(helper), fast));
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 1, 2000);
        QVERIFY(!f.events().contains(u"desktop-start"_s));
    }
    void keeperLossStopsDesktop() {
        Fixture f;
        VirtualSessionServiceOwner owner(f.checks(), [&](int result) { f.result = result; });
        QVERIFY(owner.start(f.record, f.keeperPath(helper, u"exit-retained"_s), f.plan(helper), timing));
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 1, 2000);
        QVERIFY(f.events().contains(u"desktop-exit"_s));
    }
    void stubbornDesktopEscalatesBeforeClosingKeeper() {
        Fixture f;
        int childFd = -1;
        const auto cleanup = qScopeGuard([&] { if (childFd >= 0) close(childFd); });
        auto checks = f.checks();
        checks.signalDescendants = [&](int signal) {
            ++f.signals;
            return signal == SIGKILL && childFd >= 0 && !syscall(SYS_pidfd_send_signal, childFd, signal, nullptr, 0);
        };
        VirtualSessionServiceOwner owner(checks, [&](int result) { f.result = result; });
        QVERIFY(owner.start(f.record, f.keeperPath(helper, u"normal"_s), f.plan(helper, u"ignore"_s), timing));
        QTRY_VERIFY_WITH_TIMEOUT(f.events().contains(u"desktop-start"_s), 2000);
        QFile events(f.directory.filePath(u"events"_s)); QVERIFY(events.open(QIODevice::ReadOnly));
        pid_t pid = 0;
        for (const auto &line : events.readAll().split('\n')) {
            const auto object = QJsonDocument::fromJson(line).object();
            if (object.value(u"event"_s) == u"desktop-start"_s) pid = pid_t(object.value(u"pid"_s).toInteger());
        }
        QVERIFY(pid > 1);
        childFd = int(syscall(SYS_pidfd_open, pid, 0)); QVERIFY(childFd >= 0);
        owner.stop();
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 1, 2000);
        QVERIFY(f.signals > 0); QVERIFY(f.events().contains(u"keeper-stop"_s));
        QVERIFY(!f.events().contains(u"desktop-exit"_s));
        QCOMPARE(kill(pid, 0), -1); QCOMPARE(errno, ESRCH);
    }
    void failedOwnershipGatePreventsDesktop_data() {
        QTest::addColumn<bool>("badKeeper");
        QTest::newRow("keeper") << true;
        QTest::newRow("scope") << false;
    }
    void failedOwnershipGatePreventsDesktop() {
        QFETCH(bool, badKeeper); Fixture f;
        if (badKeeper) f.validKeeper = false; else f.gone = std::nullopt;
        VirtualSessionServiceOwner owner(f.checks(), [&](int result) { f.result = result; });
        QVERIFY(owner.start(f.record, f.keeperPath(helper, u"normal"_s), f.plan(helper), timing));
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 1, 2000);
        QVERIFY(!f.events().contains(u"desktop-start"_s)); QCOMPARE(f.signals, 0);
    }
    void boundedKeeperHang_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("opening") << u"hang-open"_s;
        QTest::newRow("closing") << u"hang-close"_s;
    }
    void boundedKeeperHang() {
        QFETCH(QString, mode); Fixture f;
        VirtualSessionServiceOwner owner(f.checks(), [&](int result) { f.result = result; });
        QVERIFY(owner.start(f.record, f.keeperPath(helper, mode), f.plan(helper), timing));
        if (mode == u"hang-close"_s) {
            QTRY_VERIFY_WITH_TIMEOUT(f.events().contains(u"desktop-start"_s), 2000);
            owner.stop();
        }
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 1, 2500);
        QCOMPARE(f.signals, 0);
    }
    void failedExecutables() {
        Fixture f;
        VirtualSessionServiceOwner owner(f.checks(), [&](int result) { f.result = result; });
        auto plan = f.plan(helper); plan.program = f.directory.filePath(u"missing"_s);
        QVERIFY(owner.start(f.record, f.keeperPath(helper, u"normal"_s), plan, timing));
        QTRY_COMPARE_WITH_TIMEOUT(f.result, 1, 2000);
        QVERIFY(f.events().contains(u"keeper-stop"_s));
        Fixture other;
        VirtualSessionServiceOwner missing(other.checks(), [&](int result) { other.result = result; });
        QVERIFY(missing.start(other.record, other.directory.filePath(u"missing"_s), other.plan(helper), timing));
        QTRY_COMPARE_WITH_TIMEOUT(other.result, 1, 2000);
        QVERIFY(other.events().isEmpty());
    }
    void earlyDestructionDoesNotInvokeCompletion() {
        Fixture f;
        {
            VirtualSessionServiceOwner owner(f.checks(), [&](int result) { f.result = result; });
            QVERIFY(owner.start(f.record, f.keeperPath(helper, u"normal"_s), f.plan(helper), timing));
            QTRY_VERIFY_WITH_TIMEOUT(f.events().contains(u"desktop-start"_s), 2000);
            QTest::ignoreMessage(QtWarningMsg, "Virtual service owner destroyed before ordered completion; cleanup unproven");
            // QProcess itself also warns when its destructor kills a child;
            // those diagnostics are expected, but must not call owner methods.
        }
        QCOMPARE(f.result, -1);
    }
};
QTEST_GUILESS_MAIN(VirtualSessionServiceOwnerTest)
#include "VirtualSessionServiceOwnerTest.moc"
