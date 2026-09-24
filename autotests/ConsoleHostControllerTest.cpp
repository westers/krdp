// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "ConsoleHostController.h"
#include "ConsoleWorkerSession.h"
#include "RdpConnection.h"
#include "Server.h"
#include <QTest>

namespace KRdp
{
class ConsoleHostControllerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void physicalTopologyRevisionAndWorkerInvalidation()
    {
        Server server;
        ConsoleHostController host(&server, {}, {});
        const ConsoleWorkerWire::Outputs captured{{
            {QStringLiteral("DP-1"), QRect(0, 0, 2560, 1440), 1.0, true},
            {QStringLiteral("HDMI-A-1"), QRect(2560, 0, 2560, 1440), 1.0, false}}};
        Q_EMIT host.m_endpoint.outputsReceived(captured);
        const ConsoleWorkerWire::Topology confirmed{{
            {QStringLiteral("DP-1"), QSize(2560, 1440), QRect(0, 0, 2560, 1440), 1.0, true, 1},
            {QStringLiteral("HDMI-A-1"), QSize(2560, 1440), QRect(2560, 0, 2560, 1440), 1.0, false, 3}}};
        Q_EMIT host.m_endpoint.topologyReceived(confirmed);
        QVERIFY(host.m_topologyAvailable);
        QCOMPARE(host.m_topologyPriorities.value(QStringLiteral("HDMI-A-1")), 3);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, quint64(1));
        const QString generation = host.m_topologyCatalog.snapshot().generation;
        Q_EMIT host.m_endpoint.topologyReceived(confirmed);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, quint64(1));
        auto changed = confirmed;
        changed.outputs[1].pixels = QSize(1920, 1080);
        changed.outputs[1].logical = QRect(2560, 0, 1920, 1080);
        auto newCapture = captured;
        newCapture.monitors[1].geometry = QRect(2560, 0, 1920, 1080);
        Q_EMIT host.m_endpoint.outputsReceived(newCapture);
        Q_EMIT host.m_endpoint.topologyReceived(changed);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, quint64(2));
        QCOMPARE(host.m_topologyCatalog.snapshot().generation, generation);
        Q_EMIT host.m_endpoint.topologyReceived(ConsoleWorkerWire::Topology{});
        QVERIFY(!host.m_topologyAvailable);
        QVERIFY(host.m_topologyPriorities.isEmpty());
        QVERIFY(host.m_topologyCatalog.snapshot().generation != generation);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, quint64(0));
    }

    void microphoneAcknowledgementAndRevocation()
    {
        // No event-loop pumping: RdpConnection's queued socket initialization
        // must never run. Exercise actual broker/consent methods without RDP.
        Server server;
        RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.addClient(&connection);
        auto &client = *host.m_clients.front();
        QVERIFY(client.externalMicrophone);
        host.m_control.admit(client.id);
        host.syncControlState();
        connection.setAudioPriority(true);
        host.m_inputEnabled = true;
        const auto generation = host.m_controlGeneration;
        const auto pending = [&](quint64 request) {
            client.media = {false, false, true};
            QVERIFY(host.m_control.setMedia(client.id, client.media));
            host.m_microphoneClient = client.id;
            host.m_microphonePolicy = {generation, request, true};
            host.m_nextMicrophoneId = request;
        };
        pending(1);
        QVERIFY(!connection.audioPriorityActive()); // No mic until worker ready.
        host.microphoneResult({generation + 1, 1, {}});
        host.microphoneResult({generation, 2, {}});
        QVERIFY(!connection.audioPriorityActive());
        host.microphoneResult({generation, 1, {}});
        QVERIFY(connection.audioPriorityActive()); // Mic-only is enough.
        QVERIFY(host.m_microphoneReady);
        QVERIFY(host.m_microphonePump.isActive());
        host.setWorkerActive(false);
        QVERIFY(!connection.audioPriorityActive());
        QVERIFY(!host.m_control.media().microphone);
        QVERIFY(!host.m_microphonePump.isActive());
        host.microphoneResult({generation, 1, {}});
        QVERIFY(!connection.audioPriorityActive());

        host.m_inputEnabled = true;
        pending(3);
        host.microphoneResult({generation, 3, QStringLiteral("source failed")});
        QVERIFY(!connection.audioPriorityActive());
        QCOMPARE(host.m_microphoneClient, quint64(0));
        pending(5);
        host.microphoneResult({generation, 5, {}});
        QVERIFY(connection.audioPriorityActive());
        host.m_control.release(client.id);
        host.syncControlState();
        QVERIFY(!connection.audioPriorityActive());
        QVERIFY(!host.m_control.media().microphone);
        host.microphoneResult({generation, 5, {}});
        QVERIFY(!connection.audioPriorityActive());
    }
};
}
QTEST_GUILESS_MAIN(KRdp::ConsoleHostControllerTest)
#include "ConsoleHostControllerTest.moc"
