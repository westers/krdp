// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "ConsoleHostController.h"
#include "ConsoleWorkerSession.h"
#include "RdpConnection.h"
#include "Server.h"
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>

namespace KRdp
{
class ConsoleHostControllerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void creatorOwnedOutputKeepsConsoleKindAndOwner()
    {
        Server server;
        ConsoleHostController host(&server, {}, {});
        const ConsoleWorkerWire::Outputs captured{{
            {QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true},
            {QStringLiteral("Virtual-owned"), QRect(1280, 0, 1280, 720), 1, false}}};
        Q_EMIT host.m_endpoint.outputsReceived(captured);
        const ConsoleWorkerWire::Topology topology{{
            {QStringLiteral("DP-1"), QSize(1280, 720), QRect(0, 0, 1280, 720), 1, true, 1, true},
            {QStringLiteral("Virtual-owned"), QSize(1280, 720), QRect(1280, 0, 1280, 720), 1, false, 2, false}}};
        Q_EMIT host.m_endpoint.topologyReceived(topology);
        QVERIFY(host.m_topologyAvailable);
        const auto &outputs = host.m_topologyCatalog.snapshot().outputs;
        QCOMPARE(outputs.size(), 2);
        QVERIFY(outputs[0].output.physical);
        QVERIFY(outputs[0].output.owner.isEmpty());
        QVERIFY(!outputs[1].output.physical);
        QCOMPARE(outputs[1].output.owner, QStringLiteral("physical-console"));
    }

    void physicalPreviewCommitWaitsForExactCapturedReadback()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Server server;
        RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, directory.path());
        host.addClient(&connection);
        const auto id = host.m_clients.front()->id;
        host.m_control.admit(id);
        host.syncControlState();
        host.m_inputEnabled = true;
        const ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000};
        const QByteArray token(24, 't');
        QVERIFY(host.m_endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), target, token));
        QLocalSocket worker;
        worker.connectToServer(host.m_endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), 1000, token})
            + ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(host.m_endpoint.ready());
        const ConsoleWorkerWire::Outputs captured{{
            {QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true},
            {QStringLiteral("HDMI-A-1"), QRect(1280, 0, 1280, 720), 1, false}}};
        const ConsoleWorkerWire::Topology before{{
            {QStringLiteral("DP-1"), QSize(1280, 720), QRect(0, 0, 1280, 720), 1, true, 1},
            {QStringLiteral("HDMI-A-1"), QSize(1280, 720), QRect(1280, 0, 1280, 720), 1, false, 3}}};
        Q_EMIT host.m_endpoint.outputsReceived(captured);
        Q_EMIT host.m_endpoint.topologyReceived(before);
        QVERIFY(host.m_topologyAvailable);
        const auto snapshot = host.m_topologyCatalog.snapshot();
        const auto preview = [&](const QString &requestId) {
            return QJsonObject{{QStringLiteral("type"), QStringLiteral("topology-preview")},
                {QStringLiteral("v"), 1}, {QStringLiteral("id"), requestId},
                {QStringLiteral("generation"), snapshot.generation},
                {QStringLiteral("expectedRevision"), double(snapshot.revision)},
                {QStringLiteral("allowRemoval"), false}, {QStringLiteral("allowPhysicalChange"), true},
                {QStringLiteral("operations"), QJsonArray{QJsonObject{
                    {QStringLiteral("op"), QStringLiteral("move")},
                    {QStringLiteral("output"), snapshot.outputs[1].id},
                    {QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), 1280}, {QStringLiteral("y"), 100}}}}}}};
        };
        const auto commit = [&](const QString &requestId, const QString &previewToken) {
            return QJsonObject{{QStringLiteral("type"), QStringLiteral("topology-commit")},
                {QStringLiteral("v"), 1}, {QStringLiteral("id"), requestId},
                {QStringLiteral("token"), previewToken}, {QStringLiteral("generation"), snapshot.generation},
                {QStringLiteral("expectedRevision"), double(snapshot.revision)}};
        };
        host.onControlRecord(&connection, id, preview(QStringLiteral("disabled")));
        QVERIFY(!host.m_physicalPreview); // Source route is deliberately off in installed defaults.
        host.m_experimentalPhysicalTopology = true;
        host.onControlRecord(&connection, id, preview(QStringLiteral("first")));
        QVERIFY(host.m_physicalPreview);
        QCOMPARE(host.m_physicalPreview->plan.beforePriorities.value(QStringLiteral("HDMI-A-1")), 3);
        auto reordered = before;
        reordered.outputs[1].priority = 2;
        Q_EMIT host.m_endpoint.topologyReceived(reordered);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, snapshot.revision + 1);
        host.onControlRecord(&connection, id, commit(QStringLiteral("first"), host.m_physicalPreview->token));
        QVERIFY(!host.m_pendingPhysical); // A KDE priority-only edit invalidates the preview.

        const auto current = host.m_topologyCatalog.snapshot();
        auto currentPreview = preview(QStringLiteral("second"));
        currentPreview.insert(QStringLiteral("expectedRevision"), double(current.revision));
        host.onControlRecord(&connection, id, currentPreview);
        QVERIFY(host.m_physicalPreview);
        auto currentCommit = commit(QStringLiteral("second"), host.m_physicalPreview->token);
        currentCommit.insert(QStringLiteral("expectedRevision"), double(current.revision));
        host.onControlRecord(&connection, id, currentCommit);
        QVERIFY(host.m_pendingPhysical);
        QVERIFY(!host.m_pendingPhysical->waitingReadback);
        const auto serial = host.m_pendingPhysical->serial;
        const auto generation = host.m_pendingPhysical->controlGeneration;
        ConsoleWorkerWire::Deframer fromBroker;
        std::optional<ConsoleWorkerWire::PhysicalLayout> dispatched;
        QElapsedTimer readDeadline;
        readDeadline.start();
        while (!dispatched && readDeadline.elapsed() < 1000) {
            QCoreApplication::processEvents();
            if (!worker.bytesAvailable()) worker.waitForReadyRead(20);
            fromBroker.feed(worker.readAll());
            while (const auto record = fromBroker.next()) {
                if (const auto physical = ConsoleWorkerWire::physicalLayout(*record)) dispatched = *physical;
            }
        }
        QVERIFY(dispatched);
        QCOMPARE(dispatched->requestId, serial);
        QCOMPARE(dispatched->controlGeneration, generation);
        QCOMPARE(dispatched->before[1].priority, quint8(2));
        QCOMPARE(dispatched->operations.size(), 1);
        QCOMPARE(dispatched->operations[0].output, QStringLiteral("HDMI-A-1"));
        QCOMPARE(dispatched->operations[0].globalLogical, QPoint(1280, 100));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PhysicalLayoutResult{serial, generation, {}}));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(host.m_pendingPhysical && host.m_pendingPhysical->waitingReadback);
        QVERIFY(host.m_physicalLeaseActive);
        QCOMPARE(host.m_physicalLeaseGeneration, generation);
        QVERIFY(host.m_pendingPhysical); // Worker success alone is not client success.
        auto movedCapture = captured;
        movedCapture.monitors[1].geometry.moveTop(100);
        Q_EMIT host.m_endpoint.outputsReceived(movedCapture);
        auto moved = reordered;
        moved.outputs[1].logical.moveTop(100);
        Q_EMIT host.m_endpoint.topologyReceived(moved);
        QVERIFY(!host.m_pendingPhysical);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, current.revision + 1);
        QCOMPARE(host.m_topologyCatalog.snapshot().outputs[1].output.logicalGeometry.top(), 100);
        host.onControlRecord(&connection, id, QJsonObject{{QStringLiteral("type"), QStringLiteral("console-resize")},
            {QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("fit-1")},
            {QStringLiteral("output"), QStringLiteral("DP-1")},
            {QStringLiteral("width"), 1600}, {QStringLiteral("height"), 900}, {QStringLiteral("scale"), 1.25}});
        QVERIFY(host.m_pendingPhysical);
        QVERIFY(host.m_pendingPhysical->resizeReply);
        QVERIFY(!host.m_pendingResize); // Experimental legacy Fit uses the same physical lease.
        const auto fitSerial = host.m_pendingPhysical->serial;
        std::optional<ConsoleWorkerWire::PhysicalLayout> fitCommand;
        readDeadline.restart();
        while (!fitCommand && readDeadline.elapsed() < 1000) {
            QCoreApplication::processEvents();
            if (!worker.bytesAvailable()) worker.waitForReadyRead(20);
            fromBroker.feed(worker.readAll());
            while (const auto record = fromBroker.next()) {
                if (const auto physical = ConsoleWorkerWire::physicalLayout(*record)) fitCommand = *physical;
            }
        }
        QVERIFY(fitCommand);
        QCOMPARE(fitCommand->requestId, fitSerial);
        QCOMPARE(fitCommand->operations.size(), 1);
        QCOMPARE(fitCommand->operations[0].kind, ConsoleWorkerWire::MixedOperation::Kind::Resize);
        QCOMPARE(fitCommand->operations[0].output, QStringLiteral("DP-1"));
        QCOMPARE(fitCommand->operations[0].pixels, QSize(1600, 900));
        QCOMPARE(fitCommand->operations[0].scale, 1.25);
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PhysicalLayoutResult{fitSerial, generation, {}}));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(host.m_pendingPhysical && host.m_pendingPhysical->waitingReadback);
        movedCapture.monitors[0].scale = 1.25;
        Q_EMIT host.m_endpoint.outputsReceived(movedCapture);
        moved.outputs[0].pixels = QSize(1600, 900);
        moved.outputs[0].scale = 1.25;
        Q_EMIT host.m_endpoint.topologyReceived(moved);
        QVERIFY(!host.m_pendingPhysical);
        QVERIFY(host.m_physicalLeaseActive);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, current.revision + 2);
        RemoteTopologyDraft::Request nextDraft;
        nextDraft.generation = host.m_topologyCatalog.snapshot().generation;
        nextDraft.expectedRevision = host.m_topologyCatalog.snapshot().revision;
        nextDraft.owner = QString::number(id);
        nextDraft.allowPhysicalChange = true;
        nextDraft.operations.append({RemoteTopologyDraft::Operation::Kind::Move,
            host.m_topologyCatalog.snapshot().outputs[1].id, QPoint(1280, 200), {}, 1});
        const auto nextPlan = ConsoleTopologyPlan::make(host.m_topologyCatalog.snapshot(), host.m_topologyPriorities,
            nextDraft, {.changePrimary = true, .maxOutputs = 16, .maxOutputDimension = 4096, .maxAtlasDimension = 8192});
        QVERIFY(nextPlan);
        host.m_pendingPhysical = ConsoleHostController::PendingPhysical{id, QStringLiteral("failed"), serial + 1,
            generation, *nextPlan, true};
        movedCapture.monitors[1].geometry.moveTop(150);
        Q_EMIT host.m_endpoint.outputsReceived(movedCapture);
        moved.outputs[1].logical.moveTop(150);
        Q_EMIT host.m_endpoint.topologyReceived(moved);
        QVERIFY(!host.m_pendingPhysical);
        QVERIFY(!host.m_inputEnabled); // A plausible but wrong whole-layout readback fails closed.
    }

    void physicalLeaseTransferWaitsForVerifiedRelease()
    {
        Server server;
        RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.addClient(&connection);
        const auto id = host.m_clients.front()->id;
        host.m_control.admit(id);
        host.syncControlState();
        host.m_inputEnabled = true;
        host.m_physicalLeaseActive = true;
        host.m_physicalLeaseGeneration = host.m_controlGeneration;
        const auto oldGeneration = host.m_controlGeneration;
        host.m_control.release(id);
        host.syncControlState();
        QVERIFY(!host.m_inputEnabled);
        QVERIFY(host.m_physicalLeaseActive);
        host.setWorkerActive(true);
        QVERIFY(!host.m_inputEnabled); // A replacement worker cannot bypass the pending release.
        Q_EMIT host.m_endpoint.physicalLeaseReleased({oldGeneration + 1, true});
        QVERIFY(host.m_physicalLeaseActive);
        Q_EMIT host.m_endpoint.physicalLeaseReleased({oldGeneration, false});
        QVERIFY(host.m_physicalLeaseActive);
        Q_EMIT host.m_endpoint.workerStopped();
        QVERIFY(host.m_physicalLeaseActive); // A vanished worker cannot claim release.
        QVERIFY(!host.m_inputEnabled);
        Q_EMIT host.m_endpoint.physicalLeaseReleased({oldGeneration, true});
        QVERIFY(!host.m_physicalLeaseActive);
        QVERIFY(!host.m_inputEnabled); // The next worker must still establish fresh capture.
    }

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
        auto reordered = confirmed;
        reordered.outputs[1].priority = 2;
        Q_EMIT host.m_endpoint.topologyReceived(reordered);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, quint64(2));
        QCOMPARE(host.m_topologyPriorities.value(QStringLiteral("HDMI-A-1")), 2);
        Q_EMIT host.m_endpoint.topologyReceived(reordered);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, quint64(2));
        auto changed = confirmed;
        changed.outputs[1].pixels = QSize(1920, 1080);
        changed.outputs[1].logical = QRect(2560, 0, 1920, 1080);
        auto newCapture = captured;
        newCapture.monitors[1].geometry = QRect(2560, 0, 1920, 1080);
        Q_EMIT host.m_endpoint.outputsReceived(newCapture);
        Q_EMIT host.m_endpoint.topologyReceived(changed);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, quint64(3));
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
