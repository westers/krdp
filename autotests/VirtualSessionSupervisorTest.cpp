// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <csignal>
#include <QTemporaryDir>
#include <QFile>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <cstring>
#include "VirtualSessionSupervisor.h"
#include "VirtualSessionGuardian.h"

using Supervisor = KRdp::VirtualSessionSupervisor;
using Phase = KRdp::VirtualSessionState::Phase;

class VirtualSessionSupervisorTest : public QObject
{
    Q_OBJECT
    static std::optional<Supervisor::Launch> sleeper(quint32, const Supervisor::Handle &)
    {
        return Supervisor::Launch{QStringLiteral("/usr/bin/sleep"), {QStringLiteral("60")}, {}, {}};
    }
private Q_SLOTS:
    void independentServiceMayAppearAfterReservation()
    {
        if (!getuid()) QSKIP("Nonroot guardian");
        QTemporaryDir directory;
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        const auto id = uuid(), incarnation = uuid();
        const auto socket = directory.filePath(QStringLiteral("guardian.sock"));
        const QByteArray token(32, 'p');
        Supervisor broker({}, 1500);
        int available = 0;
        broker.setGuardianAvailableCallback([&](const auto &) { ++available; });
        const auto handle = broker.adopt({quint32(getuid()), id, incarnation, socket, token}, true);
        QVERIFY(handle);
        QCOMPARE(broker.list(getuid()).first().phase, Phase::Starting);
        QVERIFY(!broker.attach(getuid(), id, 1));
        KRdp::VirtualSessionGuardian guardian;
        QVERIFY(guardian.start(getuid(), id, token, socket, *sleeper(getuid(), {}), nullptr, incarnation));
        QTRY_COMPARE(available, 1);
        QCOMPARE(broker.list(getuid()).first().phase, Phase::Starting);
        QVERIFY(broker.captureReady(*handle));
        QVERIFY(broker.attach(getuid(), id, 1));
        QCOMPARE(available, 1);
    }
    void independentStartupTimeoutCannotAcceptLateGuardian()
    {
        if (!getuid()) QSKIP("Nonroot guardian");
        QTemporaryDir directory;
        const auto uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
        const auto id = uuid(), incarnation = uuid();
        const auto socket = directory.filePath(QStringLiteral("guardian.sock"));
        const QByteArray token(32, 'p');
        Supervisor broker({}, 20);
        int available = 0;
        broker.setGuardianAvailableCallback([&](const auto &) { ++available; });
        const auto handle = broker.adopt({quint32(getuid()), id, incarnation, socket, token}, true);
        QVERIFY(handle);
        QTRY_COMPARE(broker.list(getuid()).first().phase, Phase::Failed);
        KRdp::VirtualSessionGuardian guardian;
        QVERIFY(guardian.start(getuid(), id, token, socket, *sleeper(getuid(), {}), nullptr, incarnation));
        QTRY_COMPARE(guardian.phase(), QStringLiteral("running"));
        QVERIFY(!broker.captureReady(*handle));
        QVERIFY(!broker.attach(getuid(), id, 1));
        QVERIFY(!broker.recreate(getuid(), id));
        QVERIFY(!broker.forget(getuid(), id));
        QCOMPARE(available, 0);
    }
    void adoptAcrossBrokerLifetimes()
    {
        if (!getuid()) QSKIP("Nonroot guardian");
        QTemporaryDir directory;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto socket = directory.filePath(QStringLiteral("guardian.sock"));
        const QByteArray token(32, 'a');
        KRdp::VirtualSessionGuardian guardian;
        QVERIFY(guardian.start(getuid(), id, token, socket, *sleeper(getuid(), {})));
        QTRY_COMPARE(guardian.phase(), QStringLiteral("running"));
        const auto pid = guardian.processId();
        KRdp::VirtualSessionGuardianClient::Identity identity{quint32(getuid()), id, guardian.incarnation(), socket, token};
        Supervisor::Handle old;
        {
            Supervisor broker({});
            const auto adopted = broker.adopt(identity);
            QVERIFY(adopted);
            old = *adopted;
            QVERIFY(!broker.adopt(identity));
            QVERIFY(!broker.attach(getuid(), id, 1));
            QTest::qWait(50); // handshake alone does not make capture ready
            QCOMPARE(broker.list(getuid()).first().phase, Phase::Starting);
            QVERIFY(broker.captureReady(*adopted));
            QVERIFY(broker.attach(getuid(), id, 1));
            QVERIFY(broker.disconnect(*adopted, 1));
        }
        QCOMPARE(guardian.processId(), pid);
        QCOMPARE(guardian.phase(), QStringLiteral("running"));
        Supervisor next({});
        const auto adopted = next.adopt(identity);
        QVERIFY(adopted);
        QVERIFY(adopted->manager != old.manager);
        QVERIFY(!next.captureReady(old));
        // Deliberately deliver capture before handshake: both gates still needed.
        next.captureReady(*adopted);
        QTRY_COMPARE(next.list(getuid()).first().phase, Phase::Retained);
        QVERIFY(!next.attach(getuid() + 1, id, 2));
        QVERIFY(next.attach(getuid(), id, 2));
        QVERIFY(next.stop(getuid(), id));
        QTRY_COMPARE(next.list(getuid()).first().phase, Phase::Absent);
        QCOMPARE(guardian.phase(), QStringLiteral("exited"));
        QVERIFY(next.forget(getuid(), id));
    }
    void failedAdoptionDoesNotReplaceOrForgetLiveApps()
    {
        if (!getuid()) QSKIP("Nonroot guardian");
        QTemporaryDir directory;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto socket = directory.filePath(QStringLiteral("guardian.sock"));
        const QByteArray token(32, 'b');
        KRdp::VirtualSessionGuardian guardian;
        QVERIFY(guardian.start(getuid(), id, token, socket, *sleeper(getuid(), {})));
        QTRY_COMPARE(guardian.phase(), QStringLiteral("running"));
        Supervisor broker(sleeper);
        int revoked = 0;
        broker.setUnavailableCallback([&](const auto &) { ++revoked; });
        const auto adopted = broker.adopt({quint32(getuid()), id, QUuid::createUuid().toString(QUuid::WithoutBraces), socket, token});
        QVERIFY(adopted);
        QTRY_COMPARE(broker.list(getuid()).first().phase, Phase::Failed);
        QCOMPARE(revoked, 1);
        QVERIFY(!broker.captureReady(*adopted));
        QVERIFY(!broker.recreate(getuid(), id));
        QVERIFY(!broker.forget(getuid(), id));
        QCOMPARE(guardian.phase(), QStringLiteral("running"));
    }
    void captureTimeoutDoesNotStopRetainedDesktop()
    {
        if (!getuid()) QSKIP("Nonroot guardian");
        QTemporaryDir directory;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto socket = directory.filePath(QStringLiteral("guardian.sock"));
        const QByteArray token(32, 'c');
        KRdp::VirtualSessionGuardian guardian;
        QVERIFY(guardian.start(getuid(), id, token, socket, *sleeper(getuid(), {})));
        QTRY_COMPARE(guardian.phase(), QStringLiteral("running"));
        Supervisor broker({}, 60);
        QVERIFY(broker.adopt({quint32(getuid()), id, guardian.incarnation(), socket, token}));
        QTRY_COMPARE(broker.list(getuid()).first().phase, Phase::Failed);
        QCOMPARE(guardian.phase(), QStringLiteral("running"));
        QVERIFY(!broker.stop(getuid() + 1, id));
        QVERIFY(broker.stop(getuid(), id));
        QTRY_COMPARE(broker.list(getuid()).first().phase, Phase::Absent);
        QCOMPARE(guardian.phase(), QStringLiteral("exited"));
    }
    void lostStopContactDoesNotClaimExit()
    {
        if (!getuid()) QSKIP("Nonroot guardian");
        QTemporaryDir directory;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto socket = directory.filePath(QStringLiteral("guardian.sock"));
        const QByteArray token(32, 'd');
        KRdp::VirtualSessionGuardian guardian;
        QVERIFY(guardian.start(getuid(), id, token, socket, *sleeper(getuid(), {})));
        QTRY_COMPARE(guardian.phase(), QStringLiteral("running"));
        Supervisor broker({});
        int revoked = 0;
        broker.setUnavailableCallback([&](const auto &) { ++revoked; });
        const auto adopted = broker.adopt({quint32(getuid()), id, guardian.incarnation(), socket, token});
        QVERIFY(adopted);
        broker.captureReady(*adopted);
        QTRY_COMPARE(broker.list(getuid()).first().phase, Phase::Retained);
        QVERIFY(QFile::remove(socket)); // break contact with only this disposable guardian
        QVERIFY(broker.stop(getuid(), id));
        QTRY_COMPARE(broker.list(getuid()).first().phase, Phase::Failed);
        QCOMPARE(revoked, 1);
        QCOMPARE(guardian.phase(), QStringLiteral("running"));
        QVERIFY(!broker.forget(getuid(), id));
        QVERIFY(!broker.recreate(getuid(), id));
    }
    void unavailableCallbackMayDestroyBroker()
    {
        if (!getuid()) QSKIP("Nonroot guardian");
        QTemporaryDir directory;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto socket = directory.filePath(QStringLiteral("guardian.sock"));
        const QByteArray token(32, 'e');
        KRdp::VirtualSessionGuardian guardian;
        QVERIFY(guardian.start(getuid(), id, token, socket, *sleeper(getuid(), {})));
        QTRY_COMPARE(guardian.phase(), QStringLiteral("running"));
        auto broker = std::make_unique<Supervisor>(Supervisor::LaunchFactory{}, 60);
        broker->setUnavailableCallback([&](const auto &handle) {
            broker.reset();
            QCOMPARE(handle.id, id); // callback argument must outlive broker storage
        });
        QVERIFY(broker->adopt({quint32(getuid()), id, guardian.incarnation(), socket, token}));
        QTRY_VERIFY(!broker);
        QCOMPARE(guardian.phase(), QStringLiteral("running"));
    }
    void synchronousStopFailureMayDestroyBroker()
    {
        if (!getuid()) QSKIP("Nonroot guardian");
        QTemporaryDir directory;
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto path = directory.filePath(QStringLiteral("guardian.sock"));
        const QByteArray token(32, 'f');
        KRdp::VirtualSessionGuardian guardian;
        QVERIFY(guardian.start(getuid(), id, token, path, *sleeper(getuid(), {})));
        QTRY_COMPARE(guardian.phase(), QStringLiteral("running"));
        auto broker = std::make_unique<Supervisor>(Supervisor::LaunchFactory{});
        const auto handle = broker->adopt({quint32(getuid()), id, guardian.incarnation(), path, token});
        QVERIFY(handle);
        broker->captureReady(*handle);
        QTRY_COMPARE(broker->list(getuid()).first().phase, Phase::Retained);
        QVERIFY(QFile::remove(path));
        const auto encoded = QFile::encodeName(path);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        QVERIFY(encoded.size() < qsizetype(sizeof(address.sun_path)));
        std::memcpy(address.sun_path, encoded.constData(), size_t(encoded.size()) + 1);
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        QVERIFY(fd >= 0);
        QCOMPARE(bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
        close(fd); // valid private socket inode but kernel refuses connection
        QCOMPARE(chmod(encoded.constData(), 0600), 0);
        broker->setUnavailableCallback([&](const auto &snapshot) {
            broker.reset();
            QCOMPARE(snapshot.id, id);
        });
        QVERIFY(broker->stop(getuid(), id));
        QTRY_VERIFY(!broker);
        QCOMPARE(guardian.phase(), QStringLiteral("running"));
    }
    void disconnectRetainsActualProcess()
    {
        Supervisor supervisor(sleeper);
        const auto handle = supervisor.create(1000);
        QVERIFY(handle);
        QVERIFY(!supervisor.attach(1000, handle->id, 1));
        bool ready = false;
        QTRY_VERIFY(ready || (ready = supervisor.captureReady(*handle)));
        QVERIFY(supervisor.attach(1000, handle->id, 1));
        QVERIFY(supervisor.disconnect(*handle, 1));
        QCOMPARE(supervisor.list(1000).first().phase, Phase::Retained);
        QVERIFY(supervisor.attach(1000, handle->id, 2));
        QVERIFY(!supervisor.stop(1001, handle->id));
        QVERIFY(supervisor.stop(1000, handle->id));
        QVERIFY(!supervisor.attach(1000, handle->id, 3));
        QTRY_COMPARE(supervisor.list(1000).first().phase, Phase::Absent);
        QVERIFY(supervisor.forget(1000, handle->id));
    }
    void startupTimeoutAndLateReadiness()
    {
        Supervisor supervisor(sleeper, 30, 30);
        const auto old = supervisor.create(1000);
        QVERIFY(old);
        QTRY_COMPARE(supervisor.list(1000).first().phase, Phase::Failed);
        QVERIFY(!supervisor.captureReady(*old));
        const auto next = supervisor.recreate(1000, old->id);
        QVERIFY(next);
        QVERIFY(next->generation > old->generation);
        QVERIFY(!supervisor.captureReady(*old));
        QVERIFY(supervisor.stop(1000, next->id));
        QTRY_COMPARE(supervisor.list(1000).first().phase, Phase::Absent);
    }
    void failedSpawnAndUnsafeFactory()
    {
        Supervisor missing([](quint32, const Supervisor::Handle &) {
            return Supervisor::Launch{QStringLiteral("/nonexistent/krdp-test-program"), {}, {}, {}};
        });
        QVERIFY(missing.create(1000));
        QTRY_COMPARE(missing.list(1000).first().phase, Phase::Failed);
        Supervisor inherited([](quint32, const Supervisor::Handle &) {
            auto spec = sleeper(1000, {});
            spec->environment = QProcessEnvironment(QProcessEnvironment::InheritFromParent);
            return spec;
        });
        QVERIFY(inherited.create(1000));
        QCOMPARE(inherited.list(1000).first().phase, Phase::Failed);
        Supervisor rejected([](quint32, const Supervisor::Handle &) -> std::optional<Supervisor::Launch> { return {}; });
        QVERIFY(rejected.create(1000));
        QCOMPARE(rejected.list(1000).first().phase, Phase::Failed);
    }
    void processExitInvalidatesRetention()
    {
        Supervisor supervisor([](quint32, const Supervisor::Handle &) {
            return Supervisor::Launch{QStringLiteral("/usr/bin/true"), {}, {}, {}};
        });
        const auto handle = supervisor.create(1000);
        QVERIFY(handle);
        QTRY_COMPARE(supervisor.list(1000).first().phase, Phase::Failed);
        QVERIFY(!supervisor.attach(1000, handle->id, 1));
        QVERIFY(!supervisor.captureReady(*handle));
    }
    void forcedStopDoesNotTouchAnotherRuntime()
    {
        Supervisor supervisor([](quint32 uid, const Supervisor::Handle &handle) {
            auto spec = sleeper(uid, handle);
            spec->childSetup = [] { ::signal(SIGTERM, SIG_IGN); };
            return spec;
        }, 5000, 200);
        const auto first = supervisor.create(1000);
        const auto other = supervisor.create(1001);
        QVERIFY(first && other);
        bool readyFirst = false;
        bool readyOther = false;
        QTRY_VERIFY(readyFirst || (readyFirst = supervisor.captureReady(*first)));
        QTRY_VERIFY(readyOther || (readyOther = supervisor.captureReady(*other)));
        QVERIFY(supervisor.stop(1000, first->id));
        QTest::qWait(20);
        QCOMPARE(supervisor.list(1000).first().phase, Phase::Stopping);
        QTRY_COMPARE(supervisor.list(1000).first().phase, Phase::Absent);
        QVERIFY(supervisor.attach(1001, other->id, 9));
        QVERIFY(supervisor.disconnect(*other, 9));
        QCOMPARE(supervisor.list(1001).first().phase, Phase::Retained);
    }
};
QTEST_GUILESS_MAIN(VirtualSessionSupervisorTest)
#include "VirtualSessionSupervisorTest.moc"
