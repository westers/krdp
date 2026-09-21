// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "RdpConnection.h"
#include "Server.h"
#include "MicrophoneConsent.h"
#include "MicrophonePcmQueue.h"

using namespace KRdp;

class RdpAudioPriorityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void microphoneQueueIsBoundedFreshAndGenerationScoped()
    {
        using Queue = MicrophonePcmQueue;
        const auto now = Queue::Clock::time_point{};
        const QByteArray packet(Queue::PacketBytes, 'a');
        Queue queue;
        QVERIFY(!queue.write(0, packet, now));
        queue.reset(1);
        QVERIFY(!queue.write(1, QByteArray(3, 'x'), now));
        QVERIFY(!queue.write(1, QByteArray(192004, 'x'), now));
        QVERIFY(queue.write(1, QByteArray(Queue::Capacity, 'a'), now));
        QVERIFY(queue.write(1, QByteArray(Queue::PacketBytes, 'b'), now));
        for (int i = 0; i < 9; ++i) QCOMPARE(queue.take(1, now), packet);
        QCOMPARE(queue.take(1, now), QByteArray(Queue::PacketBytes, 'b'));
        QVERIFY(queue.take(1, now).isEmpty());
        QVERIFY(queue.write(1, packet, now));
        const auto later = now + std::chrono::milliseconds(251);
        QVERIFY(queue.write(1, QByteArray(4, 'n'), later));
        QCOMPARE(queue.take(1, later), QByteArray(4, 'n')); // no stale prefix
        QVERIFY(queue.write(1, packet, later));
        queue.reset(2);
        QVERIFY(queue.take(2, later).isEmpty());
        QVERIFY(!queue.write(1, packet, later));
        QVERIFY(queue.write(2, packet, later));
        queue.reset(1); // stale cleanup cannot erase successor samples
        QCOMPARE(queue.take(2, later), packet);
    }

    void externalMicrophoneRouteDoesNotImplyConsent()
    {
        Server server;
        RdpConnection connection(&server, -1);
        QVERIFY(connection.enableExternalMicrophone());
        QVERIFY(connection.takeExternalMicrophone().isEmpty());
        connection.setAudioPriority(true);
        QVERIFY(!connection.audioPriorityActive());
        connection.setMediaPolicy(false, true, false);
        QVERIFY(connection.audioPriorityActive());
        QVERIFY(connection.takeExternalMicrophone().isEmpty());
        connection.setMediaPolicy(false, false, false);
        QVERIFY(connection.takeExternalMicrophone().isEmpty());
    }

    void microphoneConsentRejectsLateContexts()
    {
        MicrophoneConsent consent;
        int samples = 0;
        const auto sink = [&] { ++samples; };
        QVERIFY(!consent.deliver(0, sink));
        consent.setEnabled(true);
        const auto first = consent.snapshot();
        QVERIFY(first.enabled);
        QVERIFY(first.generation != 0);
        QVERIFY(consent.deliver(first.generation, sink));
        consent.setEnabled(true);
        QCOMPARE(consent.snapshot().generation, first.generation);
        consent.setEnabled(false);
        QVERIFY(!consent.deliver(first.generation, sink));
        consent.setEnabled(true);
        const auto second = consent.snapshot();
        QVERIFY(second.generation != first.generation);
        QVERIFY(!consent.deliver(first.generation, sink));
        QVERIFY(consent.deliver(second.generation, sink));
        QCOMPARE(samples, 2);
    }

    void defaultsOverridesAndConsent()
    {
        // Destroy before dispatching events: initialize() is queued, so these
        // policy tests never open a socket, audio device or authentication flow.
        Server server;
        RdpConnection connection(&server, -1);
        QVERIFY(!connection.audioPriorityActive());
        connection.setAudioPriorityDefault(true);
        QVERIFY(!connection.audioPriorityActive());
        connection.setMediaPolicy(false, true, false); // mic only
        QVERIFY(connection.audioPriorityActive());
        connection.setAudioPriority(false);
        QVERIFY(!connection.audioPriorityActive());
        connection.setAudioPriorityDefault(false);
        connection.setAudioPriorityDefault(true);
        QVERIFY(!connection.audioPriorityActive()); // default reload cannot beat override
        connection.clearAudioPriorityOverride();
        QVERIFY(connection.audioPriorityActive());
        connection.setMediaPolicy(true, false, false); // playback only
        QVERIFY(connection.audioPriorityActive());
        connection.setMediaPolicy(true, true, false); // duplex
        QVERIFY(connection.audioPriorityActive());
        connection.setMediaPolicy(false, false, true); // camera is not audio consent
        QVERIFY(!connection.audioPriorityActive());
        connection.setAudioPriorityDefault(false);
        connection.setAudioPriority(true);
        connection.setMediaPolicy(false, true, false);
        QVERIFY(connection.audioPriorityActive());
        connection.setMediaPolicy(false, false, false);
        QVERIFY(!connection.audioPriorityActive());
    }
};
QTEST_GUILESS_MAIN(RdpAudioPriorityTest)
#include "RdpAudioPriorityTest.moc"
