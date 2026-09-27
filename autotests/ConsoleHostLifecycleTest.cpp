// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-C-1, C-2, C-4, C-5, C-6, C-7: console broker worker lifecycle and
// client admission, driven through a fake launcher (no processes are started)
// and a real worker-side socket where the wire matters.

#include "ConsoleAdmission.h"
#include "ConsoleHostController.h"
#include "ConsoleWorkerSession.h"
#include "RdpConnection.h"
#include "Server.h"

#include <QDataStream>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <unistd.h>

namespace KRdp
{
namespace
{
const quint32 Me = quint32(getuid());

ConsoleSeat::Session user(const QString &id, quint32 uid, bool locked = false)
{
    ConsoleSeat::Session session;
    session.id = id;
    session.uid = uid;
    session.seat = QStringLiteral("seat0");
    session.type = QStringLiteral("wayland");
    session.sessionClass = QStringLiteral("user");
    session.state = QStringLiteral("active");
    session.active = true;
    session.locked = locked;
    return session;
}

ConsoleSeat::Session greeter(const QString &id, quint32 uid)
{
    auto session = user(id, uid);
    session.sessionClass = QStringLiteral("greeter");
    return session;
}

struct FakeLauncher {
    struct Launch {
        ConsoleHandoff::Target target;
        QString socket;
        QByteArray token;
    };
    QList<Launch> launches;
    QList<int> signals_;
    bool fail = false;

    ConsoleHostController::WorkerLauncher functions()
    {
        return {[this](const ConsoleHandoff::Target &target, const QString &socket, const QByteArray &token, QString *error) {
                    if (fail) {
                        if (error) *error = QStringLiteral("fake launch failure");
                        return false;
                    }
                    launches.append({target, socket, token});
                    return true;
                },
                [this](const QString &, int signal) {
                    signals_.append(signal);
                }};
    }
};

QByteArray foreignVersionHello()
{
    QByteArray body;
    QDataStream stream(&body, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint16(ConsoleWorkerWire::ProtocolVersion + 11) << quint8(ConsoleWorkerWire::Kind::Hello) << QByteArray("old");
    QByteArray result;
    QDataStream header(&result, QIODevice::WriteOnly);
    header.setByteOrder(QDataStream::BigEndian);
    header << quint32(body.size());
    return result + body;
}
}

class ConsoleHostLifecycleTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void drainDeadlineTerminatesThenKillsWithoutOverlap()
    {
        QTemporaryDir runtime;
        Server server;
        FakeLauncher launcher;
        ConsoleHostController host(&server, launcher.functions(), runtime.path());
        QCOMPARE(ConsoleHostController::DrainDeadlineMs, 5000);
        QCOMPARE(ConsoleHostController::KillDeadlineMs, 2000);
        host.m_drainDeadline.setInterval(40); // Injected clock.
        host.m_killDeadline.setInterval(40);

        host.setSeatSessions({user(QStringLiteral("3"), Me)});
        QCOMPARE(launcher.launches.size(), 1);
        const auto first = launcher.launches.first();
        // Per-launch private socket directory (AUD-C-9).
        const QFileInfo directory(QFileInfo(first.socket).path());
        QCOMPARE(directory.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther),
                 QFileDevice::Permissions{});

        // The worker connects and authenticates but is still starting when
        // the seat changes to the greeter.
        QLocalSocket worker;
        worker.connectToServer(first.socket);
        QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), Me, first.token}));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(host.m_endpoint.authenticated());

        host.setSeatSessions({greeter(QStringLiteral("c1"), Me)});
        QVERIFY(host.m_handoff.draining());
        QTRY_VERIFY(worker.bytesAvailable() > 0); // Stop reached the authenticated worker.
        ConsoleWorkerWire::Deframer fromBroker;
        fromBroker.feed(worker.readAll());
        const auto stop = fromBroker.next();
        QVERIFY(stop);
        QCOMPARE(stop->kind, ConsoleWorkerWire::Kind::Stop);

        // Ready while draining must not grant input or start anything.
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTest::qWait(20);
        QVERIFY(!host.m_inputEnabled);
        QVERIFY(!host.m_endpoint.ready());

        // The worker ignores Stop: SIGTERM after the drain deadline, then SIGKILL.
        QTRY_COMPARE(launcher.signals_, (QList<int>{SIGTERM, SIGKILL}));
        QCOMPARE(launcher.launches.size(), 1); // Never two workers at once (AUD-C-7).

        host.workerExited(QStringLiteral("/not/this/launch"));
        QCOMPARE(launcher.launches.size(), 1); // A stale exit does not end the drain.
        host.workerExited(first.socket);
        QCOMPARE(launcher.launches.size(), 2);
        QCOMPARE(launcher.launches.last().target.adapter, ConsoleSeat::Adapter::Greeter);
        QVERIFY(!QFileInfo::exists(directory.filePath())); // Old launch directory removed.
        QVERIFY(!host.m_retryTimer.isActive()); // An intentional drain is not a failure.
    }

    void stopBeforeAuthenticationStillDrains()
    {
        QTemporaryDir runtime;
        Server server;
        FakeLauncher launcher;
        ConsoleHostController host(&server, launcher.functions(), runtime.path());
        host.setSeatSessions({user(QStringLiteral("3"), Me)});
        QCOMPARE(launcher.launches.size(), 1);
        host.setSeatSessions({greeter(QStringLiteral("c1"), Me)}); // Before the worker even connected.
        QVERIFY(host.m_endpoint.stopPending());
        QLocalSocket worker;
        worker.connectToServer(launcher.launches.first().socket);
        QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), Me, launcher.launches.first().token}));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(worker.bytesAvailable() > 0);
        ConsoleWorkerWire::Deframer fromBroker;
        fromBroker.feed(worker.readAll());
        const auto stop = fromBroker.next();
        QVERIFY(stop);
        QCOMPARE(stop->kind, ConsoleWorkerWire::Kind::Stop);
    }

    void versionMismatchBacksOff()
    {
        QTemporaryDir runtime;
        Server server;
        FakeLauncher launcher;
        ConsoleHostController host(&server, launcher.functions(), runtime.path());
        const auto sessions = QList<ConsoleSeat::Session>{user(QStringLiteral("3"), Me)};
        host.setSeatSessions(sessions);
        QCOMPARE(launcher.launches.size(), 1);

        QList<int> delays;
        for (int attempt = 0; attempt < 7; ++attempt) {
            const auto launch = launcher.launches.last();
            QLocalSocket worker;
            worker.connectToServer(launch.socket);
            QVERIFY(worker.waitForConnected(1000));
            worker.write(foreignVersionHello());
            QVERIFY(worker.waitForBytesWritten(1000));
            QTRY_COMPARE(worker.state(), QLocalSocket::UnconnectedState); // Broker dropped it.
            host.workerExited(launch.socket); // The worker's process is reaped.
            QVERIFY(host.m_retryTimer.isActive());
            delays.append(host.m_retryTimer.interval());
            const auto launched = launcher.launches.size();
            // A logind refresh during the backoff must not relaunch early.
            auto refreshed = sessions;
            refreshed.append(greeter(QStringLiteral("c9"), 107)); // Inactive-seat noise.
            refreshed.last().seat = QStringLiteral("seat1");
            host.setSeatSessions(refreshed);
            host.setSeatSessions(sessions);
            QCOMPARE(launcher.launches.size(), launched);
            host.m_retryTimer.stop();
            host.retryWorker(); // Fire the (injected) retry clock.
            QCOMPARE(launcher.launches.size(), launched + 1);
        }
        QCOMPARE(delays, (QList<int>{1000, 2000, 4000, 8000, 16000, 30000, 30000}));

        // Readiness resets the backoff.
        const auto launch = launcher.launches.last();
        QLocalSocket worker;
        worker.connectToServer(launch.socket);
        QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), Me, launch.token})
                     + ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(host.m_inputEnabled);
        QCOMPARE(host.m_backoff.failures(), 0);
    }

    void backoffLogsOncePerLevel()
    {
        ConsoleWorkerBackoff backoff;
        QList<bool> logged;
        for (int i = 0; i < 8; ++i) logged.append(backoff.next().newLevel);
        QCOMPARE(logged, (QList<bool>{true, true, true, true, true, true, false, false}));
        backoff.reset();
        QCOMPARE(backoff.next().delayMs, 1000);
    }

    void launchFailureRetriesWithBackoff()
    {
        QTemporaryDir runtime;
        Server server;
        FakeLauncher launcher;
        launcher.fail = true;
        ConsoleHostController host(&server, launcher.functions(), runtime.path());
        host.setSeatSessions({user(QStringLiteral("3"), Me)});
        QVERIFY(host.m_retryTimer.isActive()); // No logind poll exists to retry for us.
        QVERIFY(!host.m_workerAlive);
        launcher.fail = false;
        host.m_retryTimer.stop();
        host.retryWorker();
        QCOMPARE(launcher.launches.size(), 1);
    }

    void workerExitClearsLeaseAndGreeterIsServed()
    {
        QTemporaryDir runtime;
        Server server;
        FakeLauncher launcher;
        ConsoleHostController host(&server, launcher.functions(), runtime.path());
        host.setSeatSessions({user(QStringLiteral("3"), Me)});
        host.m_physicalLeaseActive = true; // Worker vanished before a verified release.
        host.m_consoleCreatorsActive = true;
        host.m_physicalLeaseGeneration = 9;
        host.setSeatSessions({greeter(QStringLiteral("c1"), Me)});
        host.workerExited(launcher.launches.first().socket);
        QVERIFY(!host.m_physicalLeaseActive);
        QVERIFY(!host.m_consoleCreatorsActive);
        QCOMPARE(launcher.launches.size(), 2);
        const auto launch = launcher.launches.last();
        QLocalSocket worker;
        worker.connectToServer(launch.socket);
        QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("c1"), Me, launch.token})
                     + ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(host.m_inputEnabled); // The greeter is never refused by a stale lease.
    }

    void admissionMatrix()
    {
        using ConsoleAdmission::allowed;
        const ConsoleHandoff::Target desktop{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000};
        const ConsoleHandoff::Target login{ConsoleSeat::Adapter::Greeter, QStringLiteral("c1"), 107};
        QVERIFY(allowed(desktop, false, 1000));   // same uid
        QVERIFY(!allowed(desktop, false, 1001));  // other uid, unlocked desktop
        QVERIFY(allowed(desktop, true, 1001));    // other uid, lock screen only
        QVERIFY(allowed(login, false, 1001));     // greeter: any PAM user
        QVERIFY(allowed(login, false, 1000));
        QVERIFY(!allowed(desktop, false, std::nullopt)); // no PAM identity
        QVERIFY(!allowed(login, false, std::nullopt));
        QVERIFY(!allowed({ConsoleSeat::Adapter::VirtualUser, QStringLiteral("x"), 1000}, false, 1000));
    }

    void admissionIsEnforcedOnStreamingAndSeatChanges()
    {
        Server server;
        ConsoleHostController host(&server, {}, {});
        QHash<RdpConnection *, quint32> uids;
        host.setUidResolver([&uids](RdpConnection *connection) -> std::optional<quint32> {
            return uids.contains(connection) ? std::optional<quint32>(uids.value(connection)) : std::nullopt;
        });
        host.setSeatSessions({user(QStringLiteral("3"), 1000, false)});
        RdpConnection owner(&server, -1);
        RdpConnection intruder(&server, -1);
        RdpConnection anonymous(&server, -1);
        uids.insert(&owner, 1000);
        uids.insert(&intruder, 1001);
        for (auto *connection : {&owner, &intruder, &anonymous}) {
            host.addClient(connection);
        }
        const auto idOf = [&host](RdpConnection *connection) -> ConsoleControl::Id {
            for (const auto &client : host.m_clients) {
                if (client->connection == connection) return client->id;
            }
            return 0;
        };
        const auto intruderId = idOf(&intruder);
        Q_EMIT intruder.stateChanged(RdpConnection::State::Streaming);
        QCOMPARE(idOf(&intruder), ConsoleControl::Id(0)); // Refused and removed.
        QVERIFY(!host.m_control.admitted(intruderId));
        QVERIFY(host.m_control.owner() != intruderId);
        Q_EMIT anonymous.stateChanged(RdpConnection::State::Streaming);
        QCOMPARE(idOf(&anonymous), ConsoleControl::Id(0));
        const auto ownerId = idOf(&owner);
        Q_EMIT owner.stateChanged(RdpConnection::State::Streaming);
        QVERIFY(host.m_control.admitted(ownerId));
        QVERIFY(host.m_control.ownsControl(ownerId));

        // Locked desktop: another account may use the lock screen ...
        host.setSeatSessions({user(QStringLiteral("3"), 1000, true)});
        RdpConnection visitor(&server, -1);
        uids.insert(&visitor, 1001);
        host.addClient(&visitor);
        const auto visitorId = idOf(&visitor);
        Q_EMIT visitor.stateChanged(RdpConnection::State::Streaming);
        QVERIFY(host.m_control.admitted(visitorId));
        // ... until it is unlocked, when only the owner stays.
        host.setSeatSessions({user(QStringLiteral("3"), 1000, false)});
        QVERIFY(!host.m_control.admitted(visitorId));
        QCOMPARE(idOf(&visitor), ConsoleControl::Id(0));
        QVERIFY(host.m_control.admitted(ownerId));

        // Greeter: anyone authenticated; a later desktop of uid 1001 evicts 1000.
        host.setSeatSessions({greeter(QStringLiteral("c1"), 107)});
        RdpConnection other(&server, -1);
        uids.insert(&other, 1001);
        host.addClient(&other);
        const auto otherId = idOf(&other);
        Q_EMIT other.stateChanged(RdpConnection::State::Streaming);
        QVERIFY(host.m_control.admitted(otherId));
        QVERIFY(host.m_control.admitted(ownerId));
        host.setSeatSessions({user(QStringLiteral("5"), 1001, false)});
        QVERIFY(host.m_control.admitted(otherId));
        QVERIFY(!host.m_control.admitted(ownerId));
    }

    void disconnectWhileTopologyChangeIsPending()
    {
        Server server;
        ConsoleHostController host(&server, {}, {});
        auto *connection = new RdpConnection(&server, -1);
        // Mimic Server::incomingConnection(): its Closed slot, connected
        // first, deletes the connection inside the emission (src/Server.cpp).
        QObject::connect(connection, &RdpConnection::stateChanged, &server, [connection](RdpConnection::State state) {
            if (state == RdpConnection::State::Closed) delete connection;
        });
        host.addClient(connection);
        const auto id = host.m_clients.front()->id;
        host.m_control.admit(id);
        host.syncControlState();
        host.m_inputEnabled = true;
        host.m_pendingPhysical = ConsoleHostController::PendingPhysical{id, QStringLiteral("req"), 7, host.m_controlGeneration, {}, true};
        host.m_pendingVirtual.reset();
        host.m_pendingTopology.insert(id, QStringLiteral("query"));
        // A control record still queued when the connection disappears.
        Q_EMIT connection->controlRecordReceived(QJsonObject{{QStringLiteral("type"), QStringLiteral("topology-query")},
                                                             {QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("late")}});
        Q_EMIT connection->stateChanged(RdpConnection::State::Closed);
        QVERIFY(host.m_clients.empty()); // Removed via destroyed; nothing dangles.
        QVERIFY(!host.m_pendingPhysical);
        QVERIFY(!host.m_pendingTopology.contains(id));
        QVERIFY(!host.m_control.admitted(id));
        QTest::qWait(10); // Deliver the queued record: must be ignored, not dereferenced.
        Q_EMIT host.m_endpoint.outputsReceived({{{QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true}}});
        host.sendLayouts();
        host.finishTopologyQueries();
    }
};
}

QTEST_GUILESS_MAIN(KRdp::ConsoleHostLifecycleTest)

#include "ConsoleHostLifecycleTest.moc"
