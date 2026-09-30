// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "ConsoleHostController.h"
#include "ConsoleFrameLayout.h"
#include "ConsoleWorkerSession.h"
#include "ConsoleWorkerOutbox.h"
#include <QSignalSpy>
#include "RdpConnection.h"
#include "Server.h"
#include "VideoStream.h"
#include <QJsonArray>
#include <QMouseEvent>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>

namespace KRdp
{
class ConsoleHostControllerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void perUserDefaultsSurviveControlTransferWithoutCrossingAccounts()
    {
        Server server;
        RdpConnection first(&server, -1), second(&server, -1);
        ConsoleHostController host(&server, {}, {});
        int reads = 0;
        std::optional<quint32> firstUid;
        host.setUidResolver([&](RdpConnection *connection) {
            return connection == &first ? firstUid : std::optional<quint32>(1001);
        });
        host.setUserSettingsReader([&](quint32 uid) {
            ++reads;
            return BrokerUserSettings::parse(uid == 1000
                ? "[General]\nQuality=43\nAdaptiveQuality=false\nPreferAudioQuality=true\nAv1Tiles=8\nSoftwareEncoding=prefer\n"
                : "[General]\nQuality=64\nPreferAudioQuality=false\nAv1Tiles=2\nSoftwareEncoding=never\n");
        });
        host.setVideoCodecHost({});
        host.addClient(&first);
        host.addClient(&second);
        auto &a = *host.m_clients[0];
        auto &b = *host.m_clients[1];
        host.loadUserSettings(a);
        QCOMPARE(reads, 0); // No authenticated identity: no user-file read.
        firstUid = 1000;
        host.loadUserSettings(a);
        host.loadUserSettings(b);
        QCOMPARE(reads, 2);
        QCOMPARE(first.videoStream()->av1TilesSetting(), 8);
        QCOMPARE(second.videoStream()->av1TilesSetting(), 2);
        QCOMPARE(first.videoStream()->softwareEncoding(), CodecPolicy::SoftwareEncoding::Prefer);
        QCOMPARE(second.videoStream()->softwareEncoding(), CodecPolicy::SoftwareEncoding::Never);
        host.m_control.admit(a.id);
        host.m_control.admit(b.id);
        host.syncControlState();
        a.videoQuality = 10; // A reduced/live setting from the first ownership period.
        QVERIFY(host.m_control.release(a.id));
        host.syncControlState();
        QVERIFY(host.m_control.acquire(b.id));
        host.syncControlState();
        QCOMPARE(a.videoQuality, quint8(43));
        QCOMPARE(b.videoQuality, quint8(64));
        QCOMPARE(a.preferences.preferAudioQuality, std::optional(true));
        QCOMPARE(b.preferences.preferAudioQuality, std::optional(false));
        host.loadUserSettings(a);
        QCOMPARE(reads, 2); // Snapshot once per connection; no read of another owner.
    }

    void configuredVideoQualityIsUsedForNewConsoleOwner()
    {
        Server server;
        RdpConnection connection(&server, -1);
        QSignalSpy quality(connection.videoStream(), &VideoStream::requestedQualityChanged);
        ConsoleHostController host(&server, {}, {});
        host.setVideoQualityPolicy(37, true);
        host.addClient(&connection);
        QCOMPARE(host.m_clients.front()->videoQuality, quint8(37));
        QVERIFY(!quality.isEmpty());
        QCOMPARE(quality.last().at(0).value<quint8>(), quint8(37));
    }

    void multiOutputFrameRequiresCapturedPixelAtlas()
    {
        // The catalog sorts by backend key; capture/surface indices instead
        // follow QScreen order. Matching array indices would paint the wrong
        // output when a virtual name sorts ahead of a physical connector.
        const ConsoleWorkerWire::Outputs captured{{
            {QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true},
            {QStringLiteral("Virtual-krdp-added-test"), QRect(1280, 0, 960, 540), 1, false}}, QPoint(200, 100)};
        RemoteTopologyCatalog::Snapshot snapshot{QStringLiteral("generation"), 1, {
            {QStringLiteral("o-2"), {QStringLiteral("Virtual-krdp-added-test"), QStringLiteral("Virtual-krdp-added-test"),
                QSize(960, 540), QRect(1480, 100, 960, 540), 1, true, false, false, QStringLiteral("physical-console")}},
            {QStringLiteral("o-1"), {QStringLiteral("DP-1"), QStringLiteral("DP-1"),
                QSize(1280, 720), QRect(200, 100, 1280, 720), 1, true, true, true, {}}}}};
        VideoFrame frame;
        frame.size = QSize(960, 540);
        frame.monitorIndex = 1;
        frame.isKeyFrame = true;
        frame.monitors = {{QRect(0, 0, 1280, 720), true}, {QRect(1280, 0, 960, 540), false}};
        QVERIFY(ConsoleFrameLayout::confirmed(frame, captured, snapshot));
        auto wrong = frame;
        wrong.monitorIndex = 0;
        QVERIFY(!ConsoleFrameLayout::confirmed(wrong, captured, snapshot));
        wrong = frame;
        wrong.monitors[1].geometry.moveLeft(1281);
        QVERIFY(!ConsoleFrameLayout::confirmed(wrong, captured, snapshot));
        wrong = frame;
        wrong.monitors[1].primary = true;
        QVERIFY(!ConsoleFrameLayout::confirmed(wrong, captured, snapshot));
        auto stale = snapshot;
        stale.outputs[0].output.logicalGeometry.moveLeft(1481);
        QVERIFY(!ConsoleFrameLayout::confirmed(frame, captured, stale));
        stale = snapshot;
        stale.outputs[0].output.nativePixels = QSize(958, 540);
        QVERIFY(!ConsoleFrameLayout::confirmed(frame, captured, stale));
    }

    void independentOutputChangeWaitsForReadbackAndKeyframe()
    {
        Server server;
        RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.addClient(&connection);
        host.m_inputEnabled = true;
        const auto clientId = host.m_clients.front()->id;
        host.m_control.admit(clientId);
        QVERIFY(host.m_control.acquire(clientId));
        host.m_clients.front()->session->setWorkerActive(true);
        const auto press = std::make_shared<QMouseEvent>(QEvent::MouseButtonPress, QPointF(20, 20), QPointF{},
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        VideoFrame single;
        single.size = QSize(1280, 720);
        single.isKeyFrame = true;
        single.monitors = {{QRect(0, 0, 1280, 720), true}};
        Q_EMIT host.m_endpoint.outputsReceived({{{QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true}}});
        Q_EMIT host.m_endpoint.topologyReceived({{{QStringLiteral("DP-1"), QSize(1280, 720),
            QRect(0, 0, 1280, 720), 1, true, 1, true}}});
        Q_EMIT host.m_endpoint.frameReceived(single);
        QVERIFY(host.m_clients.front()->wireLayout.isEmpty());
        host.m_clients.front()->session->sendEvent(press);

        Q_EMIT host.m_endpoint.outputsReceived({{
            {QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true},
            {QStringLiteral("Virtual-extra"), QRect(1280, 0, 960, 540), 1, false}}});
        QVERIFY(host.m_layoutAwaitingReadback);
        QVERIFY(host.m_inputState.releaseAll().isEmpty()); // Independent transition released the held button.
        host.m_clients.front()->session->sendEvent(press);
        QVERIFY(host.m_inputState.releaseAll().isEmpty()); // No stale-coordinate input before readback.
        Q_EMIT host.m_endpoint.frameReceived(single); // Old single-output capture must not pass the new inventory.
        QVERIFY(host.m_clients.front()->wireLayout.isEmpty());
        VideoFrame multi = single;
        multi.monitors = {{QRect(0, 0, 1280, 720), true}, {QRect(1280, 0, 960, 540), false}};
        Q_EMIT host.m_endpoint.frameReceived(multi);
        QVERIFY(host.m_clients.front()->wireLayout.isEmpty());
        Q_EMIT host.m_endpoint.topologyReceived({{
            {QStringLiteral("DP-1"), QSize(1280, 720), QRect(0, 0, 1280, 720), 1, true, 1, true},
            {QStringLiteral("Virtual-extra"), QSize(960, 540), QRect(1280, 0, 960, 540), 1, false, 2, false}}});
        QVERIFY(!host.m_layoutAwaitingReadback);
        multi.isKeyFrame = false;
        Q_EMIT host.m_endpoint.frameReceived(multi);
        QVERIFY(host.m_clients.front()->wireLayout.isEmpty());
        multi.isKeyFrame = true;
        Q_EMIT host.m_endpoint.frameReceived(multi);
        QCOMPARE(host.m_clients.front()->wireLayout, multi.monitors);
        host.m_clients.front()->session->sendEvent(press);
        QCOMPARE(host.m_inputState.releaseAll().size(), 1); // Input resumed only after authoritative readback.

        Q_EMIT host.m_endpoint.outputsReceived({{{QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true}}});
        QVERIFY(host.m_layoutAwaitingReadback);
        Q_EMIT host.m_endpoint.frameReceived(single);
        QCOMPARE(host.m_clients.front()->wireLayout, multi.monitors);
        Q_EMIT host.m_endpoint.topologyReceived({{{QStringLiteral("DP-1"), QSize(1280, 720),
            QRect(0, 0, 1280, 720), 1, true, 1, true}}});
        QVERIFY(!host.m_layoutAwaitingReadback);
        Q_EMIT host.m_endpoint.frameReceived(single);
        QVERIFY(host.m_clients.front()->wireLayout.isEmpty());
    }

    void consoleOwnedAddRemoveWaitForCapturedReadback()
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
        host.m_experimentalPhysicalTopology = true;
        const ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000};
        const QByteArray token(24, 'v');
        QVERIFY(host.m_endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), target, token));
        QLocalSocket worker;
        worker.connectToServer(host.m_endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), 1000, token})
            + ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(host.m_endpoint.ready());
        Q_EMIT host.m_endpoint.outputsReceived({{{QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true}}});
        Q_EMIT host.m_endpoint.topologyReceived({{{QStringLiteral("DP-1"), QSize(1280, 720),
            QRect(0, 0, 1280, 720), 1, true, 1, true}}});
        QVERIFY(host.m_topologyAvailable);
        auto before = host.m_topologyCatalog.snapshot();
        QVERIFY(!host.consoleTopology(QStringLiteral("query")).value(QStringLiteral("capabilities")).toObject()
            .value(QStringLiteral("add")).toBool()); // Physical-only opt-in does not expose new Console virtual writes.
        host.m_experimentalConsoleVirtual = true;
        QVERIFY(host.consoleTopology(QStringLiteral("query")).value(QStringLiteral("capabilities")).toObject()
            .value(QStringLiteral("add")).toBool());
        const auto preview = [&](const QString &request, const QString &operation, const QString &output,
                                 bool allowRemoval, quint64 revision) {
            QJsonObject change{{QStringLiteral("op"), operation}, {QStringLiteral("output"), output}};
            if (operation == QStringLiteral("add")) {
                change.insert(QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), 1280}, {QStringLiteral("y"), 0}});
                change.insert(QStringLiteral("pixels"), QJsonObject{{QStringLiteral("width"), 960}, {QStringLiteral("height"), 540}});
                change.insert(QStringLiteral("scale"), 1.0);
            }
            return QJsonObject{{QStringLiteral("type"), QStringLiteral("topology-preview")},
                {QStringLiteral("v"), 1}, {QStringLiteral("id"), request},
                {QStringLiteral("generation"), before.generation},
                {QStringLiteral("expectedRevision"), double(revision)},
                {QStringLiteral("allowRemoval"), allowRemoval}, {QStringLiteral("allowPhysicalChange"), false},
                {QStringLiteral("operations"), QJsonArray{change}}};
        };
        const auto commit = [&](const QString &request, const QString &previewToken, quint64 revision) {
            return QJsonObject{{QStringLiteral("type"), QStringLiteral("topology-commit")},
                {QStringLiteral("v"), 1}, {QStringLiteral("id"), request},
                {QStringLiteral("token"), previewToken}, {QStringLiteral("generation"), before.generation},
                {QStringLiteral("expectedRevision"), double(revision)}};
        };
        host.onControlRecord(&connection, id, preview(QStringLiteral("add"), QStringLiteral("add"), QStringLiteral("new:extra"), false, before.revision));
        QVERIFY(host.m_virtualPreview);
        QVERIFY(!host.m_physicalLeaseActive); // Preview does not create anything.
        const auto addToken = host.m_virtualPreview->token;
        host.onControlRecord(&connection, id, commit(QStringLiteral("add"), addToken, before.revision));
        QVERIFY(host.m_pendingVirtual);
        QVERIFY(host.m_physicalLeaseActive);
        const auto add = *host.m_pendingVirtual;
        QVERIFY(add.backendKey.startsWith(QStringLiteral("Virtual-krdp-added-")));
        ConsoleWorkerWire::Deframer fromBroker;
        std::optional<ConsoleWorkerWire::AddVirtual> dispatchedAdd;
        QElapsedTimer wireDeadline;
        wireDeadline.start();
        while (!dispatchedAdd && wireDeadline.elapsed() < 1000) {
            QCoreApplication::processEvents();
            if (!worker.bytesAvailable()) worker.waitForReadyRead(20);
            fromBroker.feed(worker.readAll());
            while (const auto record = fromBroker.next()) {
                if (const auto command = ConsoleWorkerWire::addVirtual(*record)) dispatchedAdd = *command;
            }
        }
        QVERIFY(dispatchedAdd);
        QCOMPARE(dispatchedAdd->requestId, add.serial);
        QCOMPARE(dispatchedAdd->generation, add.controlGeneration);
        QCOMPARE(dispatchedAdd->output, add.backendKey);
        QCOMPARE(dispatchedAdd->pixels, QSize(960, 540));
        QCOMPARE(dispatchedAdd->globalLogical, QPoint(1280, 0));
        Q_EMIT host.m_endpoint.addVirtualFinished({add.serial, add.controlGeneration, {}});
        QVERIFY(host.m_pendingVirtual->waitingReadback);
        Q_EMIT host.m_endpoint.outputsReceived({{
            {QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true},
            {add.backendKey, QRect(1280, 0, 960, 540), 1, false}}});
        Q_EMIT host.m_endpoint.topologyReceived({{
            {QStringLiteral("DP-1"), QSize(1280, 720), QRect(0, 0, 1280, 720), 1, true, 1, true},
            {add.backendKey, QSize(960, 540), QRect(1280, 0, 960, 540), 1, false, 2, false}}});
        QVERIFY(!host.m_pendingVirtual);
        QVERIFY(host.m_consoleCreatorsActive);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, before.revision + 1);
        VideoFrame multiFrame;
        multiFrame.size = QSize(1280, 720);
        multiFrame.monitorIndex = 0;
        multiFrame.isKeyFrame = true;
        multiFrame.monitors = {{QRect(0, 0, 1280, 720), true}, {QRect(1280, 0, 960, 540), false}};
        Q_EMIT host.m_endpoint.frameReceived(multiFrame);
        QCOMPARE(host.m_clients.front()->wireLayout, multiFrame.monitors); // Installs two RDPGFX surfaces before indexed frames.
        before = host.m_topologyCatalog.snapshot();
        const auto addedId = std::find_if(before.outputs.cbegin(), before.outputs.cend(), [&add](const auto &entry) {
            return entry.output.backendKey == add.backendKey;
        })->id;
        QVERIFY(host.consoleTopology(QStringLiteral("query")).value(QStringLiteral("capabilities")).toObject()
            .value(QStringLiteral("remove")).toBool());
        host.onControlRecord(&connection, id, preview(QStringLiteral("remove"), QStringLiteral("remove"), addedId, true, before.revision));
        QVERIFY(host.m_virtualPreview);
        host.onControlRecord(&connection, id, commit(QStringLiteral("remove"), host.m_virtualPreview->token, before.revision));
        QVERIFY(host.m_pendingVirtual);
        const auto remove = *host.m_pendingVirtual;
        QCOMPARE(remove.backendKey, add.backendKey);
        std::optional<ConsoleWorkerWire::RemoveVirtual> dispatchedRemove;
        wireDeadline.restart();
        while (!dispatchedRemove && wireDeadline.elapsed() < 1000) {
            QCoreApplication::processEvents();
            if (!worker.bytesAvailable()) worker.waitForReadyRead(20);
            fromBroker.feed(worker.readAll());
            while (const auto record = fromBroker.next()) {
                if (const auto command = ConsoleWorkerWire::removeVirtual(*record)) dispatchedRemove = *command;
            }
        }
        QVERIFY(dispatchedRemove);
        QCOMPARE(dispatchedRemove->requestId, remove.serial);
        QCOMPARE(dispatchedRemove->generation, remove.controlGeneration);
        QCOMPARE(dispatchedRemove->output, add.backendKey);
        Q_EMIT host.m_endpoint.removeVirtualFinished({remove.serial, remove.controlGeneration, {}});
        QVERIFY(host.m_pendingVirtual->waitingReadback);
        Q_EMIT host.m_endpoint.outputsReceived({{{QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true}}});
        Q_EMIT host.m_endpoint.topologyReceived({{{QStringLiteral("DP-1"), QSize(1280, 720),
            QRect(0, 0, 1280, 720), 1, true, 1, true}}});
        QVERIFY(!host.m_pendingVirtual);
        QVERIFY(!host.m_consoleCreatorsActive);
        QVERIFY(!host.m_physicalLeaseActive);
        QCOMPARE(host.m_topologyCatalog.snapshot().revision, before.revision + 1);
        VideoFrame singleFrame;
        singleFrame.size = QSize(1280, 720);
        singleFrame.monitorIndex = 0;
        singleFrame.isKeyFrame = true;
        singleFrame.monitors = {{QRect(0, 0, 1280, 720), true}};
        Q_EMIT host.m_endpoint.frameReceived(singleFrame);
        QVERIFY(host.m_clients.front()->wireLayout.isEmpty()); // Return from 2→1 clears indexed surfaces.
    }

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

    void microphoneReplyEchoesItsRequestIdOnce()
    {
        // KRDPCTL v2: the `device` answering a request echoes its requestId; a later
        // unsolicited `device` (a microphone lost after it started) must not repeat it.
        Server server;
        RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.addClient(&connection);
        auto &client = *host.m_clients.front();
        host.m_control.admit(client.id);
        host.syncControlState();
        host.m_inputEnabled = true;
        const auto generation = host.m_controlGeneration;
        client.media = {false, false, true};
        QVERIFY(host.m_control.setMedia(client.id, client.media));
        client.microphoneRequestId = QStringLiteral("media-1");
        host.m_microphoneClient = client.id;
        host.m_microphonePolicy = {generation, 1, true};
        host.m_nextMicrophoneId = 1;
        host.microphoneResult({generation, 1, {}});
        QVERIFY(host.m_microphoneReady);
        QVERIFY(client.microphoneRequestId.isEmpty());
    }

    // --- AUD-D3: the KRDPCTL `device` record on the console ---
    static QJsonObject deviceRecord(const QString &requestId, const QString &name, const QString &action)
    {
        return {{QStringLiteral("type"), QStringLiteral("device")}, {QStringLiteral("v"), 1}, {QStringLiteral("requestId"), requestId},
                {QStringLiteral("device"), name}, {QStringLiteral("action"), action}};
    }

    // AUD-FIX7: the console's one worker encodes for every client, so the controlling client's
    // private codec runs only while it is the only one admitted: a viewer turns the console back
    // to AVC (a `codec` push), and its leaving brings the private codec back.
    void codecPolicyRunsWhileTheControllerIsAlone()
    {
        Server server;
        RdpConnection owner(&server, -1);
        RdpConnection viewer(&server, -1);
        ConsoleHostController host(&server, {}, {});
        VideoCodecHost video;
        video.probe.encoders.avc = {true, true, true};
        video.probe.encoders.hevc = {true, false, false};
        host.setVideoCodecHost(video);
        QList<std::pair<RdpConnection *, QJsonObject>> sent;
        host.m_recordSent = [&sent](RdpConnection *connection, const QJsonObject &record) {
            sent.append({connection, record});
        };
        const auto last = [&sent](RdpConnection *connection) {
            for (auto it = sent.crbegin(); it != sent.crend(); ++it) {
                if (it->first == connection) return it->second;
            }
            return QJsonObject{};
        };
        const auto codec = [](const QString &requestId) {
            return QJsonObject{{QStringLiteral("type"), QStringLiteral("codec")}, {QStringLiteral("v"), 1}, {QStringLiteral("requestId"), requestId},
                               {QStringLiteral("codecs"), QJsonArray{QStringLiteral("hevc")}}};
        };
        host.addClient(&owner);
        const auto ownerId = host.m_clients.at(0)->id;
        // Asked before admission (right after `capabilities`): AVC for now.
        host.onControlRecord(&owner, ownerId, codec(QStringLiteral("k0")));
        QCOMPARE(last(&owner).value(QStringLiteral("selected")).toString(), QStringLiteral("avc"));
        QCOMPARE(last(&owner).value(QStringLiteral("requestId")).toString(), QStringLiteral("k0"));
        // Admitted and in control, alone: the policy starts (a push, no requestId).
        host.m_control.admit(ownerId);
        host.syncControlState();
        host.syncCodecPolicy();
        QVERIFY(host.m_control.ownsControl(ownerId));
        QCOMPARE(last(&owner).value(QStringLiteral("selected")).toString(), QStringLiteral("hevc"));
        QCOMPARE(last(&owner).value(QStringLiteral("backend")).toString(), QStringLiteral("hardware"));
        QVERIFY(!last(&owner).contains(QStringLiteral("requestId")));
        QCOMPARE(owner.videoStream()->codecForSessions(), VideoCodec::Hevc);
        auto &bridge = *host.m_clients.at(0)->codec;
        QVERIFY(bridge.bound());
        QCOMPARE(bridge.config()->codec, VideoCodec::Hevc);
        QCOMPARE(bridge.config()->generation, host.m_controlGeneration);
        // Asked again while alone: answered directly.
        host.onControlRecord(&owner, ownerId, codec(QStringLiteral("k1")));
        QCOMPARE(last(&owner).value(QStringLiteral("requestId")).toString(), QStringLiteral("k1"));
        QCOMPARE(last(&owner).value(QStringLiteral("selected")).toString(), QStringLiteral("hevc"));

        // A viewer joins: AVC for everyone.
        host.addClient(&viewer);
        const auto viewerId = host.m_clients.at(1)->id;
        host.m_control.admit(viewerId);
        host.syncControlState();
        host.syncCodecPolicy();
        QVERIFY(host.m_control.ownsControl(ownerId));
        QCOMPARE(last(&owner).value(QStringLiteral("selected")).toString(), QStringLiteral("avc"));
        QVERIFY(last(&owner).value(QStringLiteral("reason")).toString().contains(QStringLiteral("another client")));
        QCOMPARE(owner.videoStream()->codecForSessions(), VideoCodec::Avc420);
        QVERIFY(!host.m_clients.at(1)->codec->bound()); // only the controller steers the worker
        // The viewer cannot choose the console's codec.
        host.onControlRecord(&viewer, viewerId, codec(QStringLiteral("v1")));
        QCOMPARE(last(&viewer).value(QStringLiteral("selected")).toString(), QStringLiteral("avc"));
        QCOMPARE(last(&viewer).value(QStringLiteral("requestId")).toString(), QStringLiteral("v1"));
        QCOMPARE(viewer.videoStream()->codecForSessions(), VideoCodec::Avc420);

        // The viewer leaves: the controller's private codec is back.
        host.removeClient(&viewer, viewerId);
        QCOMPARE(last(&owner).value(QStringLiteral("selected")).toString(), QStringLiteral("hevc"));
        QCOMPARE(owner.videoStream()->codecForSessions(), VideoCodec::Hevc);
    }

    void deviceRecordsOwnerViewerBusyAndUnsupported()
    {
        Server server;
        RdpConnection owner(&server, -1);
        RdpConnection viewer(&server, -1);
        ConsoleHostController host(&server, {}, {});
        QList<std::pair<RdpConnection *, QJsonObject>> sent;
        host.m_recordSent = [&sent](RdpConnection *connection, const QJsonObject &record) {
            sent.append({connection, record});
        };
        host.addClient(&owner);
        host.addClient(&viewer);
        const auto ownerId = host.m_clients.at(0)->id;
        const auto viewerId = host.m_clients.at(1)->id;
        host.m_control.admit(ownerId);
        host.m_control.admit(viewerId);
        host.syncControlState();
        QVERIFY(host.m_control.ownsControl(ownerId));
        const auto last = [&sent](RdpConnection *connection) {
            for (auto it = sent.crbegin(); it != sent.crend(); ++it) {
                if (it->first == connection) return it->second;
            }
            return QJsonObject{};
        };

        // A logged-in, ready worker is required before a camera can start.
        host.onControlRecord(&owner, ownerId, deviceRecord(QStringLiteral("c1"), QStringLiteral("camera"), QStringLiteral("on")));
        QCOMPARE(last(&owner).value(QStringLiteral("type")).toString(), QStringLiteral("device"));
        QCOMPARE(last(&owner).value(QStringLiteral("code")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(last(&owner).value(QStringLiteral("requestId")).toString(), QStringLiteral("c1"));
        host.onControlRecord(&owner, ownerId, deviceRecord(QStringLiteral("c2"), QStringLiteral("camera"), QStringLiteral("query")));
        QCOMPARE(last(&owner).value(QStringLiteral("state")).toString(), QStringLiteral("off"));
        QCOMPARE(last(&owner).value(QStringLiteral("requestId")).toString(), QStringLiteral("c2"));

        // Playback is per client; silencing the host is the controller's.
        host.onControlRecord(&viewer, viewerId, deviceRecord(QStringLiteral("p1"), QStringLiteral("playback"), QStringLiteral("on")));
        QCOMPARE(last(&viewer).value(QStringLiteral("type")).toString(), QStringLiteral("device"));
        QCOMPARE(last(&viewer).value(QStringLiteral("state")).toString(), QStringLiteral("on"));
        QCOMPARE(last(&viewer).value(QStringLiteral("requestId")).toString(), QStringLiteral("p1"));
        auto silent = deviceRecord(QStringLiteral("p2"), QStringLiteral("playback"), QStringLiteral("on"));
        silent.insert(QStringLiteral("silenceHost"), true);
        host.onControlRecord(&viewer, viewerId, silent);
        QCOMPARE(last(&viewer).value(QStringLiteral("code")).toString(), QStringLiteral("not-owner"));
        QCOMPARE(last(&viewer).value(QStringLiteral("requestId")).toString(), QStringLiteral("p2"));
        QVERIFY(host.m_control.media().playback);
        QVERIFY(!host.m_control.media().silenceHost);

        // A viewer cannot share a microphone.
        host.onControlRecord(&viewer, viewerId, deviceRecord(QStringLiteral("m1"), QStringLiteral("microphone"), QStringLiteral("on")));
        QCOMPARE(last(&viewer).value(QStringLiteral("type")).toString(), QStringLiteral("error"));
        QCOMPARE(last(&viewer).value(QStringLiteral("code")).toString(), QStringLiteral("not-owner"));
        QCOMPARE(last(&viewer).value(QStringLiteral("requestId")).toString(), QStringLiteral("m1"));
        // The controller without a ready desktop: a state error, not a refusal.
        host.onControlRecord(&owner, ownerId, deviceRecord(QStringLiteral("m2"), QStringLiteral("microphone"), QStringLiteral("on")));
        QCOMPARE(last(&owner).value(QStringLiteral("state")).toString(), QStringLiteral("error"));
        QCOMPARE(last(&owner).value(QStringLiteral("code")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(last(&owner).value(QStringLiteral("requestId")).toString(), QStringLiteral("m2"));
        QCOMPARE(host.m_microphoneClient, quint64(0));

        // While the controller shares its microphone, anyone else is `busy`.
        auto &ownerClient = *host.m_clients.at(0);
        ownerClient.media.microphone = true;
        QVERIFY(host.m_control.setMedia(ownerId, ownerClient.media));
        host.m_microphoneClient = ownerId;
        host.m_microphonePolicy = {host.m_controlGeneration, ++host.m_nextMicrophoneId, true};
        host.onControlRecord(&viewer, viewerId, deviceRecord(QStringLiteral("m3"), QStringLiteral("microphone"), QStringLiteral("on")));
        QCOMPARE(last(&viewer).value(QStringLiteral("type")).toString(), QStringLiteral("device"));
        QCOMPARE(last(&viewer).value(QStringLiteral("state")).toString(), QStringLiteral("error"));
        QCOMPARE(last(&viewer).value(QStringLiteral("code")).toString(), QStringLiteral("busy"));
        QCOMPARE(last(&viewer).value(QStringLiteral("requestId")).toString(), QStringLiteral("m3"));
        QCOMPARE(host.m_microphoneClient, ownerId);
        // The controller's `off` ends it and is answered with its own requestId.
        host.onControlRecord(&owner, ownerId, deviceRecord(QStringLiteral("m4"), QStringLiteral("microphone"), QStringLiteral("off")));
        QCOMPARE(last(&owner).value(QStringLiteral("state")).toString(), QStringLiteral("off"));
        QCOMPARE(last(&owner).value(QStringLiteral("requestId")).toString(), QStringLiteral("m4"));
        QCOMPARE(host.m_microphoneClient, quint64(0));
        QVERIFY(!host.m_control.media().microphone);

        // Malformed: `invalid`, echoing the requestId.
        auto invalid = deviceRecord(QStringLiteral("x1"), QStringLiteral("microphone"), QStringLiteral("on"));
        invalid.insert(QStringLiteral("camera"), true);
        host.onControlRecord(&owner, ownerId, invalid);
        QCOMPARE(last(&owner).value(QStringLiteral("code")).toString(), QStringLiteral("invalid"));
        QCOMPARE(last(&owner).value(QStringLiteral("requestId")).toString(), QStringLiteral("x1"));
    }

    void microphoneAsyncReplyAndRevocation()
    {
        Server server;
        RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        QList<QJsonObject> sent;
        host.m_recordSent = [&sent](RdpConnection *, const QJsonObject &record) {
            sent.append(record);
        };
        host.addClient(&connection);
        auto &client = *host.m_clients.front();
        host.m_control.admit(client.id);
        host.syncControlState();
        host.m_inputEnabled = true;
        const auto start = [&](const QString &requestId) -> ConsoleWorkerWire::MicrophonePolicy {
            client.media = {false, false, true};
            if (!host.m_control.setMedia(client.id, client.media)) return {};
            client.microphoneRequestId = requestId;
            host.m_microphoneClient = client.id;
            host.m_microphonePolicy = {host.m_controlGeneration, ++host.m_nextMicrophoneId, true};
            return host.m_microphonePolicy;
        };
        // The worker's acknowledgement is the (asynchronous) answer, sent once.
        auto policy = start(QStringLiteral("a1"));
        QVERIFY(sent.isEmpty());
        host.microphoneResult({policy.generation, policy.requestId, {}});
        QCOMPARE(sent.size(), 1);
        QCOMPARE(sent.last().value(QStringLiteral("device")).toString(), QStringLiteral("microphone"));
        QCOMPARE(sent.last().value(QStringLiteral("state")).toString(), QStringLiteral("on"));
        QCOMPARE(sent.last().value(QStringLiteral("requestId")).toString(), QStringLiteral("a1"));
        // A console user change later revokes it: pushed, no requestId.
        host.setWorkerActive(false);
        QCOMPARE(sent.size(), 2);
        QCOMPARE(sent.last().value(QStringLiteral("state")).toString(), QStringLiteral("off"));
        QCOMPARE(sent.last().value(QStringLiteral("code")).toString(), QStringLiteral("revoked"));
        QVERIFY(!sent.last().contains(QStringLiteral("requestId")));
        QVERIFY(!host.m_control.media().microphone);

        // Revoked while the start was pending: that request is answered by it.
        host.m_inputEnabled = true;
        policy = start(QStringLiteral("a2"));
        host.setWorkerActive(false);
        QCOMPARE(sent.last().value(QStringLiteral("code")).toString(), QStringLiteral("revoked"));
        QCOMPARE(sent.last().value(QStringLiteral("requestId")).toString(), QStringLiteral("a2"));
        const auto count = sent.size();
        host.microphoneResult({policy.generation, policy.requestId, {}}); // late: ignored
        QCOMPARE(sent.size(), count);

        // A worker failure and the startup deadline are errors, answered once.
        host.m_inputEnabled = true;
        policy = start(QStringLiteral("a3"));
        host.microphoneResult({policy.generation, policy.requestId, QStringLiteral("source failed")});
        QCOMPARE(sent.last().value(QStringLiteral("state")).toString(), QStringLiteral("error"));
        QCOMPARE(sent.last().value(QStringLiteral("code")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(sent.last().value(QStringLiteral("requestId")).toString(), QStringLiteral("a3"));
        policy = start(QStringLiteral("a4"));
        host.m_microphoneDeadline.setInterval(1);
        host.m_microphoneDeadline.start();
        QTRY_COMPARE(sent.last().value(QStringLiteral("code")).toString(), QStringLiteral("timeout"));
        QCOMPARE(sent.last().value(QStringLiteral("requestId")).toString(), QStringLiteral("a4"));
        QVERIFY(client.microphoneRequestId.isEmpty());
    }

    void cameraAsyncReplyAndRevocation()
    {
        Server server;
        RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        QList<QJsonObject> sent;
        host.m_recordSent = [&sent](RdpConnection *, const QJsonObject &record) { sent.append(record); };
        host.addClient(&connection);
        const auto id = host.m_clients.front()->id;
        host.m_control.admit(id);
        host.syncControlState();
        host.m_inputEnabled = true;
        const auto start = [&](const QString &requestId) {
            host.m_cameraClient = id;
            host.m_cameraReady = false;
            host.m_clients.front()->cameraRequestId = requestId;
            host.m_cameraPolicy = {host.m_controlGeneration, ++host.m_nextCameraId, true, {}};
            return host.m_cameraPolicy;
        };
        const auto old = start(QStringLiteral("camera-1"));
        const auto next = start(QStringLiteral("camera-2"));
        host.cameraResult({old.generation, old.requestId, {}});
        QVERIFY(sent.isEmpty());
        host.cameraResult({next.generation, next.requestId, {}});
        QCOMPARE(sent.size(), 1);
        QCOMPARE(sent.last().value(QStringLiteral("device")).toString(), QStringLiteral("camera"));
        QCOMPARE(sent.last().value(QStringLiteral("state")).toString(), QStringLiteral("on"));
        QCOMPARE(sent.last().value(QStringLiteral("requestId")).toString(), QStringLiteral("camera-2"));
        host.setWorkerActive(false);
        QCOMPARE(sent.size(), 2);
        QCOMPARE(sent.last().value(QStringLiteral("state")).toString(), QStringLiteral("off"));
        QCOMPARE(sent.last().value(QStringLiteral("code")).toString(), QStringLiteral("revoked"));
    }

    // --- AUD-D3: StandardClientMedia on the console broker ---
    // A stock client (no KRDPCTL) that controls the console gets playback and
    // the microphone from standard negotiation; a viewer never gets a microphone.
    void standardClientMediaControllerOnly_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::newRow("StandardClientMedia on") << true;
        QTest::newRow("StandardClientMedia off") << false;
    }
    void standardClientMediaControllerOnly()
    {
        QFETCH(bool, enabled);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Server server;
        server.setStandardClientMedia(enabled);
        RdpConnection owner(&server, -1);
        RdpConnection viewer(&server, -1);
        ConsoleHostController host(&server, {}, directory.path());
        // A detached test connection joined nothing: as if RDPSND and DRDYNVC were joined.
        host.m_standardChannels = [](RdpConnection *connection) {
            auto channels = connection->standardMediaChannels();
            if (channels) channels->playback = channels->dynamic = true;
            return channels;
        };
        host.addClient(&owner);
        host.addClient(&viewer);
        auto &ownerClient = *host.m_clients.at(0);
        auto &viewerClient = *host.m_clients.at(1);
        QVERIFY(!owner.hasControlChannel());
        // Admission (Streaming) applies it at once to a client without KRDPCTL;
        // the first admitted client controls the console. The desktop is not
        // ready yet (no seat): playback now, the microphone waits.
        host.setUidResolver([](RdpConnection *) { return std::optional<quint32>(1000); });
        Q_EMIT owner.stateChanged(RdpConnection::State::Streaming);
        Q_EMIT viewer.stateChanged(RdpConnection::State::Streaming);
        QVERIFY(host.m_control.admitted(viewerClient.id));
        QVERIFY(host.m_control.ownsControl(ownerClient.id));
        QVERIFY(!viewerClient.media.playback);
        QCOMPARE(ownerClient.media.playback, enabled);
        QCOMPARE(host.m_media.playback, enabled);
        QVERIFY(!ownerClient.media.silenceHost);
        QCOMPARE(host.m_microphoneClient, quint64(0));

        // The logged-in desktop becomes ready: the controller's microphone starts.
        const ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000};
        const QByteArray token(24, 'm');
        QVERIFY(host.m_endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), target, token));
        QLocalSocket worker;
        worker.connectToServer(host.m_endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), 1000, token})
            + ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(host.m_endpoint.ready());
        host.setWorkerActive(true);
        if (!enabled) {
            QCOMPARE(host.m_microphoneClient, quint64(0));
            host.applyStandardMedia(viewerClient.id);
            QVERIFY(!viewerClient.media.playback);
            QVERIFY(!host.m_control.media().playback);
            QVERIFY(!host.m_control.media().microphone);
            return;
        }
        QCOMPARE(host.m_microphoneClient, ownerClient.id);
        QVERIFY(host.m_microphonePolicy.enabled);
        QVERIFY(ownerClient.microphoneRequestId.isEmpty()); // nothing to answer
        host.microphoneResult({host.m_microphonePolicy.generation, host.m_microphonePolicy.requestId, {}});
        QVERIFY(host.m_microphoneReady);
        QVERIFY(host.m_control.media().microphone);

        // A (stock) viewer gets nothing: no microphone, and no playback either.
        host.applyStandardMedia(viewerClient.id);
        QVERIFY(!viewerClient.media.playback);
        QVERIFY(!viewerClient.media.microphone);
        QCOMPARE(host.m_microphoneClient, ownerClient.id);
        // Not even while the controller's microphone is off.
        host.stopMicrophone();
        ownerClient.standardMicrophone = false;
        host.applyStandardMedia(viewerClient.id);
        QCOMPARE(host.m_microphoneClient, quint64(0));
        viewerClient.standardMicrophone = true; // even if it had been set: a viewer is never started
        host.startStandardMicrophone(viewerClient);
        QCOMPARE(host.m_microphoneClient, quint64(0));
        QVERIFY(!viewerClient.media.microphone);
    }

    // A client that speaks KRDPCTL asks for each device itself: a `device`
    // record, or any other known record, keeps StandardClientMedia away.
    void standardClientMediaNotForKrdpctlClients()
    {
        Server server;
        RdpConnection first(&server, -1);
        RdpConnection second(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.m_standardChannels = [](RdpConnection *connection) {
            auto channels = connection->standardMediaChannels();
            if (channels) channels->playback = channels->dynamic = true;
            return channels;
        };
        host.m_recordSent = [](RdpConnection *, const QJsonObject &) {};
        host.addClient(&first);
        host.addClient(&second);
        auto &a = *host.m_clients.at(0);
        auto &b = *host.m_clients.at(1);
        // Before admission: a `device` record is held for replay, and still counts.
        host.onControlRecord(&first, a.id, deviceRecord(QStringLiteral("d1"), QStringLiteral("playback"), QStringLiteral("off")));
        QVERIFY(a.deviceRecordSeen);
        host.m_control.admit(a.id);
        host.syncControlState();
        host.applyStandardMedia(a.id);
        QVERIFY(!a.media.playback);
        QVERIFY(!a.standardMicrophone);
        // A known record (a layout `query`) too; audio-priority alone does not.
        host.m_control.remove(a.id);
        host.syncControlState();
        host.m_control.admit(b.id);
        host.syncControlState();
        QVERIFY(host.m_control.ownsControl(b.id));
        host.onControlRecord(&second, b.id, QJsonObject{{QStringLiteral("type"), QStringLiteral("audio-priority")}, {QStringLiteral("v"), 1},
                                                         {QStringLiteral("id"), QStringLiteral("p")}, {QStringLiteral("enabled"), false}});
        QVERIFY(!b.spokeKrdpctl);
        host.onControlRecord(&second, b.id, QJsonObject{{QStringLiteral("type"), QStringLiteral("query")}, {QStringLiteral("v"), 1}});
        QVERIFY(b.spokeKrdpctl);
        host.applyStandardMedia(b.id);
        QVERIFY(!b.media.playback);
        QVERIFY(!b.standardMicrophone);
    }

    // AUD-FIX8 B1: the console worker's real order - Hello, EncoderCaps, the encoder's backend
    // report (it opens before capture is confirmed), Ready - through ConsoleWorkerOutbox, the class
    // the worker sends through. 23b328a's broker rejected that worker; the controlling client's
    // codec bridge must now get the report, after Ready.
    void consoleWorkerRealOrderReachesTheController()
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
        QVERIFY(host.m_control.ownsControl(id));
        QSignalSpy backend(host.m_clients.front()->session.get(), &AbstractSession::encoderBackendReported);
        QStringList order;
        connect(&host.m_endpoint, &ConsoleWorkerEndpoint::workerReady, this, [&order] { order << QStringLiteral("ready"); });
        connect(&host.m_endpoint, &ConsoleWorkerEndpoint::encoderReported, this, [&order] { order << QStringLiteral("report"); });
        QStringList errors;
        connect(&host.m_endpoint, &ConsoleWorkerEndpoint::protocolError, this, [&errors](const QString &e) { errors << e; });
        const ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), quint32(getuid())};
        const QByteArray token(24, 'r');
        QVERIFY(host.m_endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), target, token));
        QLocalSocket worker;
        worker.connectToServer(host.m_endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        ConsoleWorkerOutbox outbox([&worker](const QByteArray &record) { worker.write(record); });
        outbox.hello({target.sessionId, target.uid, token});
        ConsoleWorkerWire::EncoderCaps caps;
        caps.encoders.avc = {true, true};
        outbox.caps(caps);
        outbox.report({ConsoleWorkerWire::EncoderReport::Event::Backend, VideoCodec::Avc420, true});
        QVERIFY(worker.waitForBytesWritten(1000));
        QTest::qWait(30);
        QVERIFY(!host.m_endpoint.ready());
        QVERIFY(errors.isEmpty());
        outbox.ready();
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(host.m_endpoint.ready());
        QTRY_COMPARE(backend.count(), 1);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(order, (QStringList{QStringLiteral("ready"), QStringLiteral("report")}));
        QCOMPARE(backend.first().at(0).value<VideoCodec>(), VideoCodec::Avc420);
        QVERIFY(backend.first().at(1).toBool());
        QCOMPARE(host.m_endpoint.encoderCaps(), std::optional(caps));
    }
};
}
QTEST_GUILESS_MAIN(KRdp::ConsoleHostControllerTest)
#include "ConsoleHostControllerTest.moc"
