// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-055 wave 2: the Console worker heartbeat watchdog. The pure verdict class runs on a fake clock; the endpoint
// tests drive the same class over a real local socket with an injected clock and a stopped timer (no daemons).

#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "ConsoleWorkerEndpoint.h"
#include "ConsoleWorkerOutbox.h"
#include "ConsoleWorkerWatchdog.h"

using namespace KRdp;
using Verdict = ConsoleWorkerWatchdog::Verdict;

namespace
{
constexpr int Interval = ConsoleWorkerWire::HeartbeatIntervalMs;
constexpr int HungAfter = ConsoleWorkerWire::HeartbeatHungAfterMs;

struct EndpointRig {
    QTemporaryDir directory;
    ConsoleWorkerEndpoint endpoint;
    ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), quint32(getuid())};
    QByteArray token = QByteArray(24, 'w');
    QLocalSocket worker;
    qint64 now = 100000;
    bool ok = false;

    explicit EndpointRig(bool enabled = true, bool ready = true)
    {
        endpoint.setWatchdogEnabled(enabled);
        endpoint.setWatchdogInterval(3600 * 1000); // the test calls watchdogTick() itself
        endpoint.setWatchdogClock([this] { return now; });
        QString error;
        if (!directory.isValid() || !endpoint.listen(directory.filePath(QStringLiteral("wd.sock")), target, token, &error)) {
            qWarning() << "rig: listen failed" << error;
            return;
        }
        worker.connectToServer(endpoint.socketName());
        if (!worker.waitForConnected(1000)) {
            qWarning() << "rig: connect failed" << worker.errorString();
            return;
        }
        send(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{target.sessionId, target.uid, token}));
        if (!QTest::qWaitFor([&] { return endpoint.authenticated(); })) return;
        if (ready) {
            send(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
            if (!QTest::qWaitFor([&] { return endpoint.ready(); })) return;
        }
        ok = true;
    }
    void send(const QByteArray &bytes)
    {
        worker.write(bytes);
        worker.flush();
    }
    // The worker sends a record and the broker has read it before the next check.
    bool beat(quint64 seq)
    {
        send(ConsoleWorkerWire::frame(ConsoleWorkerWire::Heartbeat{seq}));
        QTest::qWait(30);
        return worker.state() == QLocalSocket::ConnectedState;
    }
    void advanceAndCheck(qint64 ms)
    {
        now += ms;
        endpoint.watchdogTick();
    }
};
}

class ConsoleWorkerWatchdogTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ---- the pure verdict ----
    void aliveWorkerIsNeverHung()
    {
        ConsoleWorkerWatchdog dog;
        qint64 now = 5000;
        dog.arm(now);
        for (int i = 0; i < 2000; ++i) { // ~100 minutes of 3 s beats
            now += Interval;
            dog.alive(now);
            QCOMPARE(dog.tick(now), Verdict::Alive);
        }
        QCOMPARE(dog.silentMs(), 0);
    }

    void silentWorkerIsHungExactlyOnceAfterTheThreshold()
    {
        ConsoleWorkerWatchdog dog;
        qint64 now = 0;
        dog.arm(now);
        const int ticks = HungAfter / Interval;
        for (int i = 1; i < ticks; ++i) {
            now += Interval;
            QCOMPARE(dog.tick(now), Verdict::Alive);
        }
        now += Interval;
        QCOMPARE(dog.tick(now), Verdict::Hung);
        QCOMPARE(dog.silentMs(), qint64(HungAfter));
        now += Interval;
        QCOMPARE(dog.tick(now), Verdict::Idle); // reported once; the owner is already replacing the worker
    }

    void anyRecordResetsTheSilence()
    {
        ConsoleWorkerWatchdog dog;
        qint64 now = 0;
        dog.arm(now);
        for (int i = 0; i < 14; ++i) {
            now += Interval;
            QCOMPARE(dog.tick(now), Verdict::Alive);
        }
        dog.alive(now); // a frame, a result or a heartbeat
        QCOMPARE(dog.silentMs(), 0);
        for (int i = 0; i < 14; ++i) {
            now += Interval;
            QCOMPARE(dog.tick(now), Verdict::Alive);
        }
    }

    void aStalledBrokerIsNotAFalsePositive()
    {
        ConsoleWorkerWatchdog dog;
        qint64 now = 0;
        dog.arm(now);
        // The machine was suspended / the broker starved for ten minutes: the next check is hugely late, yet it
        // adds at most two intervals of silence, so it cannot reach the 45 s threshold on its own.
        now += 10 * 60 * 1000;
        QCOMPARE(dog.tick(now), Verdict::Alive);
        QVERIFY(dog.silentMs() <= 2 * Interval);
        // The worker's beat that was waiting in the socket is read right after: back to zero.
        dog.alive(now);
        QCOMPARE(dog.silentMs(), 0);
        // Repeated load spikes with a beat in between never accumulate.
        for (int i = 0; i < 50; ++i) {
            now += 20 * 1000;
            QCOMPARE(dog.tick(now), Verdict::Alive);
            dog.alive(now);
        }
        // Even if the worker really is silent, a stalled broker needs many checks, not one.
        ConsoleWorkerWatchdog silent;
        silent.arm(0);
        int checks = 0;
        qint64 t = 0;
        Verdict verdict = Verdict::Alive;
        while (verdict == Verdict::Alive && checks < 100) {
            t += 60 * 1000; // every check is a minute late
            verdict = silent.tick(t);
            ++checks;
        }
        QCOMPARE(verdict, Verdict::Hung);
        QVERIFY(checks >= HungAfter / (2 * Interval)); // 8 checks at the 6 s credit cap
    }

    void unreadBytesCountAsLife()
    {
        ConsoleWorkerWatchdog dog;
        qint64 now = 0;
        dog.arm(now);
        for (int i = 0; i < 100; ++i) {
            now += Interval;
            QCOMPARE(dog.tick(now, true), Verdict::Alive);
        }
        QCOMPARE(dog.silentMs(), 0);
    }

    void disarmedAndRearmed()
    {
        ConsoleWorkerWatchdog dog;
        QCOMPARE(dog.tick(1000000), Verdict::Idle); // never armed
        dog.arm(0);
        dog.disarm();
        QCOMPARE(dog.tick(1000000), Verdict::Idle);
        dog.arm(2000000); // a new worker: fresh silence
        QCOMPARE(dog.tick(2000000 + Interval), Verdict::Alive);
        QCOMPARE(dog.silentMs(), qint64(Interval));
    }

    // ---- the worker's side ----
    void outboxSendsHeartbeatsFromHelloOnWithRisingSequence()
    {
        QList<QByteArray> written;
        ConsoleWorkerOutbox outbox([&](const QByteArray &bytes) { written.append(bytes); });
        outbox.heartbeat();
        QVERIFY(written.isEmpty()); // before Hello nobody is listening
        outbox.hello(ConsoleWorkerWire::Hello{QStringLiteral("3"), 1000, QByteArray(8, 'a')});
        written.clear();
        outbox.heartbeat(); // also before Ready
        outbox.heartbeat();
        outbox.ready();
        outbox.heartbeat();
        QList<quint64> sequences;
        ConsoleWorkerWire::Deframer deframer;
        for (const auto &bytes : written) deframer.feed(bytes);
        while (const auto record = deframer.next()) {
            if (const auto beat = ConsoleWorkerWire::heartbeat(*record)) sequences.append(beat->sequence);
        }
        QCOMPARE(sequences, (QList<quint64>{1, 2, 3}));
        QCOMPARE(deframer.takeInvalidCount(), 0);
    }

    // ---- the endpoint, same verdict over a real socket ----
    void endpointStaysQuietWhileHeartbeatsArrive()
    {
        EndpointRig rig;
        QVERIFY(rig.ok);
        QSignalSpy hung(&rig.endpoint, &ConsoleWorkerEndpoint::workerUnresponsive);
        for (quint64 i = 1; i <= 100; ++i) { // five minutes
            QVERIFY(rig.beat(i));
            rig.advanceAndCheck(Interval);
        }
        QCOMPARE(hung.size(), 0);
        QVERIFY(rig.endpoint.ready());
    }

    void endpointReportsAHungWorkerOnceAndDropsIt()
    {
        EndpointRig rig;
        QVERIFY(rig.ok);
        QSignalSpy hung(&rig.endpoint, &ConsoleWorkerEndpoint::workerUnresponsive);
        QSignalSpy stopped(&rig.endpoint, &ConsoleWorkerEndpoint::workerStopped);
        for (int i = 1; i < HungAfter / Interval; ++i) {
            rig.advanceAndCheck(Interval);
            QCOMPARE(hung.size(), 0);
        }
        rig.advanceAndCheck(Interval);
        QCOMPARE(hung.size(), 1);
        QCOMPARE(hung.first().first().toLongLong(), qint64(HungAfter));
        QTRY_COMPARE(rig.worker.state(), QLocalSocket::UnconnectedState);
        QTRY_COMPARE(stopped.size(), 1);
        rig.advanceAndCheck(Interval);
        QCOMPARE(hung.size(), 1); // once
    }

    void endpointCountsAnyRecordAsLife()
    {
        EndpointRig rig;
        QVERIFY(rig.ok);
        QSignalSpy hung(&rig.endpoint, &ConsoleWorkerEndpoint::workerUnresponsive);
        // A worker that is busy encoding sends load reports and frames but (say) a stuck heartbeat timer: still alive.
        for (int i = 0; i < 60; ++i) {
            rig.send(ConsoleWorkerWire::frame(ConsoleWorkerWire::EncoderLoad{qint64(i) * 1000}));
            QTest::qWait(20);
            rig.advanceAndCheck(Interval);
        }
        QCOMPARE(hung.size(), 0);
    }

    void endpointToleratesALoadSpike()
    {
        EndpointRig rig;
        QVERIFY(rig.ok);
        QSignalSpy hung(&rig.endpoint, &ConsoleWorkerEndpoint::workerUnresponsive);
        // The broker's whole process was frozen for an hour (suspend) and the worker's beats queued up unread.
        rig.send(ConsoleWorkerWire::frame(ConsoleWorkerWire::Heartbeat{1}));
        QTest::qWait(30);
        rig.advanceAndCheck(3600 * 1000);
        QCOMPARE(hung.size(), 0);
        QVERIFY(rig.beat(2));
        rig.advanceAndCheck(Interval);
        QCOMPARE(hung.size(), 0);
    }

    void heartbeatBeforeReadyKeepsAWorkerStillStartingAlive()
    {
        EndpointRig rig(true, false);
        QVERIFY(rig.ok);
        QVERIFY(!rig.endpoint.ready());
        QSignalSpy errors(&rig.endpoint, &ConsoleWorkerEndpoint::protocolError);
        QSignalSpy hung(&rig.endpoint, &ConsoleWorkerEndpoint::workerUnresponsive);
        for (quint64 i = 1; i <= 40; ++i) {
            QVERIFY(rig.beat(i));
            rig.advanceAndCheck(Interval);
        }
        QCOMPARE(errors.size(), 0);
        QCOMPARE(hung.size(), 0);
        QVERIFY(!rig.endpoint.ready());
        // And a worker that never gets to Ready and goes silent is hung too.
        for (int i = 0; i < HungAfter / Interval; ++i) rig.advanceAndCheck(Interval);
        QCOMPARE(hung.size(), 1);
    }

    void aMalformedHeartbeatFailsTheWorker()
    {
        EndpointRig rig;
        QVERIFY(rig.ok);
        QSignalSpy errors(&rig.endpoint, &ConsoleWorkerEndpoint::protocolError);
        rig.send(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Heartbeat, QByteArray(3, 'x')));
        QTRY_COMPARE(errors.size(), 1);
    }

    void theWatchdogIsOffUnlessTheBrokerEnablesIt()
    {
        EndpointRig rig(false);
        QVERIFY(rig.ok);
        QSignalSpy hung(&rig.endpoint, &ConsoleWorkerEndpoint::workerUnresponsive);
        for (int i = 0; i < 3 * HungAfter / Interval; ++i) rig.advanceAndCheck(Interval);
        QCOMPARE(hung.size(), 0);
        QVERIFY(rig.endpoint.ready());
        // Heartbeats are still accepted (the Virtual broker's endpoint receives them from the same worker).
        QVERIFY(rig.beat(1));
    }

    void aNewWorkerStartsWithFreshSilence()
    {
        EndpointRig rig;
        QVERIFY(rig.ok);
        QSignalSpy hung(&rig.endpoint, &ConsoleWorkerEndpoint::workerUnresponsive);
        for (int i = 0; i < 10; ++i) rig.advanceAndCheck(Interval);
        rig.worker.disconnectFromServer();
        QTRY_VERIFY(!rig.endpoint.authenticated());
        rig.now += 3600 * 1000;
        rig.endpoint.watchdogTick(); // no worker: nothing to judge
        QCOMPARE(hung.size(), 0);
        rig.worker.connectToServer(rig.endpoint.socketName());
        QVERIFY(rig.worker.waitForConnected(1000));
        rig.send(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{rig.target.sessionId, rig.target.uid, rig.token}));
        QTRY_VERIFY(rig.endpoint.authenticated());
        for (int i = 0; i < 14; ++i) rig.advanceAndCheck(Interval);
        QCOMPARE(hung.size(), 0);
    }
};

QTEST_GUILESS_MAIN(ConsoleWorkerWatchdogTest)

#include "ConsoleWorkerWatchdogTest.moc"
