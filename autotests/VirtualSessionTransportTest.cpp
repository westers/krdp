// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
#include <QSignalSpy>
#include <Server.h>
#include <VideoStream.h>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <freerdp/error.h>
#include <QEvent>
#include <limits>
#include "VirtualSessionTransport.h"
#include "RemoteMonitorGeometry.h"
#include "VirtualResizeProtocol.h"
#include "ExternalAudioQueue.h"
using namespace KRdp;
using namespace Qt::StringLiterals;

namespace KRdp {
class VirtualSessionTransportTest : public QObject
{
    Q_OBJECT
    ConsoleWorkerWire::Deframer m_workerDeframer;
    static QJsonObject device(const QString &name, const QString &action, std::optional<bool> silenceHost = std::nullopt) {
        QJsonObject record{{u"type"_s, u"device"_s}, {u"v"_s, 1}, {u"device"_s, name}, {u"action"_s, action}};
        if (silenceHost) record.insert(u"silenceHost"_s, *silenceHost);
        return record;
    }
    // What the old combined `media` request is now: the `device` records a client
    // sends (playback with silenceHost, then the microphone; a camera is refused).
    // Returns the last reply: the microphone's (empty while its start is pending).
    static QJsonObject media(VirtualSessionTransport &t, std::optional<quint32> uid, bool mic = true, bool playback = true, bool camera = false) {
        if (camera) return t.request(device(u"camera"_s, u"on"_s), uid);
        const auto reply = t.request(playback ? device(u"playback"_s, u"on"_s, true) : device(u"playback"_s, u"off"_s), uid);
        if (reply.isEmpty() || reply.value(u"type"_s) != u"device"_s) return reply; // refused, or the transport is gone
        return t.request(device(u"microphone"_s, mic ? u"on"_s : u"off"_s), uid);
    }
    // Through the production identity (RdpConnection's PAM uid), not the test seam.
    static QJsonObject mediaAsConnection(VirtualSessionTransport &t) {
        return media(t, t.m_connection ? t.m_connection->authenticatedPamUid() : std::nullopt);
    }
    // A settled `device` state without a problem (the old `ok`).
    static bool ok(const QJsonObject &reply) {
        const auto state = reply.value(u"state"_s).toString();
        return reply.value(u"type"_s) == u"device"_s && (state == u"on"_s || state == u"off"_s) && !reply.contains(u"code"_s);
    }
    static QString state(const QJsonObject &reply) { return reply.value(u"state"_s).toString(); }
    static QJsonObject priority(bool enabled = true) {
        return {{u"type"_s, u"audio-priority"_s}, {u"v"_s, 1}, {u"id"_s, u"priority-1"_s}, {u"enabled"_s, enabled}};
    }
    static QJsonObject resizeRequest() {
        return {{u"type"_s, u"virtual-resize"_s}, {u"v"_s, 1}, {u"id"_s, u"fit-1"_s},
            {u"width"_s, 1920}, {u"height"_s, 1080}, {u"scale"_s, 1.25}};
    }
    void microphoneFixture(const std::function<void(VirtualSessionTransport &, VirtualSessionControl &,
                                                    ConsoleWorkerEndpoint &, QLocalSocket &)> &test,
                           bool standardClientMedia = true) {
        m_workerDeframer = {};
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            return VirtualSessionSupervisor::Launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        });
        QPointer<VirtualSessionTransport> transport;
        VirtualSessionControl control(supervisor, [&](quint64, const auto &) { if (transport) transport->revoke(); });
        const auto handle = supervisor.create(1000); QVERIFY(handle);
        bool ready = false; QTRY_VERIFY(ready || (ready = supervisor.captureReady(*handle)));
        QVERIFY(control.request(1000, 1, {{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1},
            {u"id"_s, u"attach"_s}, {u"action"_s, u"attach"_s}, {u"session"_s, handle->id}}).value(u"ok"_s).toBool());
        QTemporaryDir dir; ConsoleWorkerEndpoint endpoint; QLocalSocket worker;
        const QByteArray token(32, 't');
        QVERIFY(endpoint.listen(dir.filePath(u"worker.sock"_s), {ConsoleSeat::Adapter::VirtualUser, handle->id, 1000}, token));
        worker.connectToServer(endpoint.socketName()); QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{handle->id, 1000, token}));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000)); QTRY_VERIFY(endpoint.ready());
        Server server; server.setStandardClientMedia(standardClientMedia);
        RdpConnection connection(&server, -1); quint64 sequence = 0;
        // No socket/PAM/capture: remove only this fixture's queued initialization.
        QCoreApplication::removePostedEvents(&connection, QEvent::MetaCall);
        transport = new VirtualSessionTransport(1, &connection, control, {}, sequence, &connection);
        QVERIFY(transport->m_externalMicrophone);
        QVERIFY(!transport->activateBinding(*handle, &endpoint)); // No fabricated production PAM success.
        QVERIFY(!transport->authorized()); QVERIFY(transport->authorized(1000));
        test(*transport, control, endpoint, worker);
        delete transport.data();
    }
    QList<ConsoleWorkerWire::Record> workerRecords(QLocalSocket &worker) {
        QCoreApplication::processEvents();
        worker.waitForReadyRead(20);
        m_workerDeframer.feed(worker.readAll());
        QList<ConsoleWorkerWire::Record> records;
        while (const auto record = m_workerDeframer.next()) records.append(*record);
        return records;
    }
private Q_SLOTS:
    void configuredVideoQualityFlowsToVirtualWorker()
    {
        microphoneFixture([&](auto &transport, auto &, auto &, auto &worker) {
            transport.setVideoQualityPolicy(37, true);
            QCOMPARE(transport.m_qualityCap, quint8(37));
            workerRecords(worker);
            QVERIFY(transport.forwardVideoQuality(transport.m_controlGeneration, 37, 1000));
            bool restored = false;
            for (const auto &record : workerRecords(worker)) {
                if (const auto quality = ConsoleWorkerWire::videoQuality(record)) {
                    if (quality->quality == 37) restored = true;
                }
            }
            QVERIFY(restored);
        });
    }

    void mixedCreateBindsNewOutputAndWholeLayoutToOneCapturedCommit() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            RemoteTopologyCatalog catalog;
            const auto initial = catalog.observe({
                {.backendKey = u"Virtual-0"_s, .name = u"Virtual-0"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = true, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-1"_s, .name = u"Virtual-1"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(1280, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id},
            });
            QVERIFY(initial);
            auto current = *initial;
            t.setTopologyResolver([&current](const auto &) { return std::optional(current); });
            const QString temporary = u"new:client-screen"_s;
            const QJsonArray operations{
                QJsonObject{{u"op"_s, u"resize"_s}, {u"output"_s, current.outputs[0].id},
                    {u"pixels"_s, QJsonObject{{u"width"_s, 1600}, {u"height"_s, 900}}}, {u"scale"_s, 1.0}},
                QJsonObject{{u"op"_s, u"move"_s}, {u"output"_s, current.outputs[1].id},
                    {u"position"_s, QJsonObject{{u"x"_s, 1600}, {u"y"_s, 0}}}},
                QJsonObject{{u"op"_s, u"add"_s}, {u"output"_s, temporary},
                    {u"position"_s, QJsonObject{{u"x"_s, 2880}, {u"y"_s, 0}}},
                    {u"pixels"_s, QJsonObject{{u"width"_s, 960}, {u"height"_s, 540}}}, {u"scale"_s, 1.0}},
                QJsonObject{{u"op"_s, u"primary"_s}, {u"output"_s, temporary}},
            };
            QJsonObject preview{{u"type"_s, u"topology-preview"_s}, {u"v"_s, 1},
                {u"id"_s, u"mixed-create-1"_s}, {u"generation"_s, current.generation},
                {u"expectedRevision"_s, double(current.revision)}, {u"allowRemoval"_s, false},
                {u"allowPhysicalChange"_s, false}, {u"operations"_s, operations}};
            QCOMPARE(t.request(preview, 1000).value(u"code"_s).toString(), u"unsupported"_s);
            t.m_experimentalMixed = true;
            auto simple = preview;
            simple[u"id"_s] = u"mixed-create-simple"_s;
            simple[u"operations"_s] = QJsonArray{operations[1], operations[2]}; // Move, then Add.
            QCOMPARE(t.request(simple, 1000).value(u"type"_s).toString(), u"topology-preview"_s);
            QVERIFY(RemoteTopologyProtocol::retainedReadOnly(u"cap"_s, current, false, false, true)
                .value(u"capabilities"_s).toObject().value(u"mixed"_s).toBool());
            t.m_experimentalMultiResize = true;
            t.m_experimentalPrimary = true;
            const auto proposed = t.request(preview, 1000);
            QCOMPARE(proposed.value(u"type"_s).toString(), u"topology-preview"_s);
            QCOMPARE(proposed.value(u"after"_s).toArray().size(), 3);
            QCOMPARE(proposed.value(u"after"_s).toArray().last().toObject().value(u"id"_s).toString(), temporary);
            QCOMPARE(t.request(preview, 1000).value(u"token"_s).toString(), proposed.value(u"token"_s).toString());
            QJsonObject commit{{u"type"_s, u"topology-commit"_s}, {u"v"_s, 1},
                {u"id"_s, u"mixed-create-1"_s}, {u"token"_s, proposed.value(u"token"_s)},
                {u"generation"_s, current.generation}, {u"expectedRevision"_s, double(current.revision)}};
            QVERIFY(t.request(commit, 1000).isEmpty());
            QVERIFY(t.request(commit, 1000).isEmpty());
            std::optional<ConsoleWorkerWire::MixedCreate> command;
            for (const auto &record : workerRecords(worker))
                if (const auto parsed = ConsoleWorkerWire::mixedCreate(record)) command = parsed;
            QVERIFY(command);
            QCOMPARE(command->newOutput, t.m_topologyResizeBackendKey);
            QCOMPARE(command->pixels, QSize(960, 540));
            QCOMPARE(command->globalLogical, QPoint(2880, 0));
            QCOMPARE(command->changes.size(), 3);
            QCOMPARE(command->changes[2].output, command->newOutput);
            QCOMPARE(command->changes[2].kind, ConsoleWorkerWire::MixedOperation::Kind::Primary);
            QCOMPARE(t.topologyResizeResult({command->requestId, command->generation, {}}, 1000)
                .value(u"code"_s).toString(), u"partial"_s); // Worker metadata alone is insufficient.
            preview[u"id"_s] = u"mixed-create-2"_s;
            const auto second = t.request(preview, 1000);
            QCOMPARE(second.value(u"type"_s).toString(), u"topology-preview"_s);
            commit[u"id"_s] = u"mixed-create-2"_s;
            commit[u"token"_s] = second.value(u"token"_s);
            QVERIFY(t.request(commit, 1000).isEmpty());
            const auto workerId = t.m_topologyResizeWorkerId;
            const auto newKey = t.m_topologyResizeBackendKey;
            auto first = initial->outputs[0].output;
            first.nativePixels = QSize(1600, 900);
            first.logicalGeometry.setSize(QSize(1600, 900));
            first.primary = false;
            auto secondOutput = initial->outputs[1].output;
            secondOutput.logicalGeometry.moveLeft(1600);
            RemoteTopologyCatalog::Output created{
                .backendKey = newKey, .name = newKey, .nativePixels = QSize(960, 540),
                .logicalGeometry = QRect(2880, 0, 960, 540), .scale = 1, .enabled = true,
                .primary = true, .physical = false, .owner = t.m_handle->id,
            };
            const auto updated = catalog.observe({first, secondOutput, created});
            QVERIFY(updated);
            current = *updated;
            const auto success = t.topologyResizeResult({workerId, t.m_controlGeneration, {}}, 1000);
            QCOMPARE(success.value(u"type"_s).toString(), u"topology-result"_s);
            QVERIFY(success.value(u"ok"_s).toBool());
            QCOMPARE(success.value(u"topology"_s).toObject().value(u"revision"_s).toInt(), 2);
            QCOMPARE(t.request(commit, 1000).value(u"code"_s).toString(), u"invalid"_s);
        });
    }

    void mixedPreviewBindsWholeLayoutToOneWorkerCommit() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            RemoteTopologyCatalog catalog;
            const auto initial = catalog.observe({
                {.backendKey = u"Virtual-0"_s, .name = u"Virtual-0"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = true, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-1"_s, .name = u"Virtual-1"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(1280, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id},
            });
            QVERIFY(initial);
            auto current = *initial;
            t.setTopologyResolver([&current](const auto &) { return std::optional(current); });
            const QJsonArray operations{
                QJsonObject{{u"op"_s, u"resize"_s}, {u"output"_s, current.outputs[0].id},
                    {u"pixels"_s, QJsonObject{{u"width"_s, 1600}, {u"height"_s, 900}}}, {u"scale"_s, 1.0}},
                QJsonObject{{u"op"_s, u"move"_s}, {u"output"_s, current.outputs[1].id},
                    {u"position"_s, QJsonObject{{u"x"_s, 1600}, {u"y"_s, 0}}}},
                QJsonObject{{u"op"_s, u"primary"_s}, {u"output"_s, current.outputs[1].id}},
            };
            QJsonObject preview{{u"type"_s, u"topology-preview"_s}, {u"v"_s, 1},
                {u"id"_s, u"mixed-1"_s}, {u"generation"_s, current.generation},
                {u"expectedRevision"_s, double(current.revision)}, {u"allowRemoval"_s, false},
                {u"allowPhysicalChange"_s, false}, {u"operations"_s, operations}};
            QCOMPARE(t.request(preview, 1000).value(u"code"_s).toString(), u"unsupported"_s);
            t.m_experimentalMixed = true;
            t.m_experimentalMultiResize = true;
            t.m_experimentalPrimary = true;
            QCOMPARE(t.request(preview, 1001).value(u"code"_s).toString(), u"not-owner"_s);
            auto duplicate = preview;
            duplicate[u"id"_s] = u"mixed-duplicate"_s;
            auto repeated = operations;
            repeated.append(operations[1]);
            duplicate[u"operations"_s] = repeated;
            QCOMPARE(t.request(duplicate, 1000).value(u"code"_s).toString(), u"unsupported"_s);
            const auto proposed = t.request(preview, 1000);
            QCOMPARE(proposed.value(u"type"_s).toString(), u"topology-preview"_s);
            QCOMPARE(proposed.value(u"after"_s).toArray().size(), 2);
            QCOMPARE(t.request(preview, 1000).value(u"token"_s).toString(), proposed.value(u"token"_s).toString());
            QJsonObject commit{{u"type"_s, u"topology-commit"_s}, {u"v"_s, 1},
                {u"id"_s, u"mixed-1"_s}, {u"token"_s, proposed.value(u"token"_s)},
                {u"generation"_s, current.generation}, {u"expectedRevision"_s, double(current.revision)}};
            QVERIFY(t.request(commit, 1000).isEmpty());
            QVERIFY(t.request(commit, 1000).isEmpty());
            std::optional<ConsoleWorkerWire::Mixed> command;
            for (const auto &record : workerRecords(worker))
                if (auto parsed = ConsoleWorkerWire::mixed(record)) command = parsed;
            QVERIFY(command);
            QCOMPARE(command->operations.size(), 3);
            QCOMPARE(command->operations[0].output, u"Virtual-0"_s);
            QCOMPARE(command->operations[1].globalLogical, QPoint(1600, 0));
            QCOMPARE(command->operations[2].kind, ConsoleWorkerWire::MixedOperation::Kind::Primary);
            QCOMPARE(t.topologyResizeResult({command->requestId, command->generation, {}}, 1000)
                .value(u"code"_s).toString(), u"partial"_s); // No metadata-only success.
            preview[u"id"_s] = u"mixed-2"_s;
            const auto second = t.request(preview, 1000);
            QCOMPARE(second.value(u"type"_s).toString(), u"topology-preview"_s);
            commit[u"id"_s] = u"mixed-2"_s;
            commit[u"token"_s] = second.value(u"token"_s);
            QVERIFY(t.request(commit, 1000).isEmpty());
            const auto workerId = t.m_topologyResizeWorkerId;
            auto first = initial->outputs[0].output;
            first.nativePixels = QSize(1600, 900);
            first.logicalGeometry.setSize(QSize(1600, 900));
            first.primary = false;
            auto secondOutput = initial->outputs[1].output;
            secondOutput.logicalGeometry.moveLeft(1600);
            secondOutput.primary = true;
            const auto updated = catalog.observe({first, secondOutput});
            QVERIFY(updated);
            current = *updated;
            const auto success = t.topologyResizeResult({workerId, t.m_controlGeneration, {}}, 1000);
            QCOMPARE(success.value(u"type"_s).toString(), u"topology-result"_s);
            QVERIFY(success.value(u"ok"_s).toBool());
            QCOMPARE(success.value(u"topology"_s).toObject().value(u"revision"_s).toInt(), 2);
            QCOMPARE(t.request(commit, 1000).value(u"code"_s).toString(), u"invalid"_s);
        });
    }

    void topologyQueryWaitsForCurrentCapturedKeyframe() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            RemoteTopologyCatalog catalog;
            const auto snapshot = catalog.observe({{
                .backendKey = u"Virtual-1"_s, .name = u"Virtual-1"_s,
                .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
                .scale = 1.0, .enabled = true, .primary = true, .physical = false,
                .owner = t.m_handle->id,
            }});
            QVERIFY(snapshot);
            std::optional<RemoteTopologyCatalog::Snapshot> published;
            t.setTopologyResolver([&published](const auto &) { return published; });
            const QJsonObject query{{u"type"_s, u"topology-query"_s}, {u"v"_s, 1}, {u"id"_s, u"top-1"_s}};
            QVERIFY(t.request(query, 1000).isEmpty());
            QCOMPARE(t.m_topologyId, u"top-1"_s);
            QVERIFY(t.m_topologyDeadline.isActive());
            QVERIFY(t.request(query, 1000).isEmpty()); // Retry cannot get early success.
            auto another = query; another[u"id"_s] = u"top-2"_s;
            QCOMPARE(t.request(another, 1000).value(u"code"_s).toString(), u"busy"_s);
            VideoFrame frame;
            frame.size = QSize(1280, 720); frame.data = "fixture"; frame.isKeyFrame = true;
            frame.monitors = {{QRect(0, 0, 1280, 720), true}};
            frame.isKeyFrame = false;
            QVERIFY(t.topologyFrame(frame, 1000).isEmpty());
            frame.isKeyFrame = true;
            QVERIFY(t.topologyFrame(frame, 1001).isEmpty());
            frame.monitors = {{QRect(0, 0, 640, 480), true}};
            QVERIFY(t.topologyFrame(frame, 1000).isEmpty());
            frame.monitors = {{QRect(0, 0, 1280, 720), true}};
            published = snapshot;
            const auto answer = t.topologyFrame(frame, 1000);
            QCOMPARE(answer.value(u"type"_s).toString(), u"topology"_s);
            QCOMPARE(answer.value(u"id"_s).toString(), u"top-1"_s);
            QCOMPARE(answer.value(u"generation"_s).toString(), snapshot->generation);
            QVERIFY(!answer.value(u"capabilities"_s).toObject().value(u"add"_s).toBool());
            QVERIFY(t.m_topologyId.isEmpty());
            QVERIFY(!t.m_topologyDeadline.isActive());
            const auto idleAnswer = t.request(query, 1000);
            QCOMPARE(idleAnswer.value(u"type"_s).toString(), u"topology"_s);
            QCOMPARE(idleAnswer.value(u"generation"_s).toString(), snapshot->generation);
            QVERIFY(t.m_topologyId.isEmpty());
            t.revoke();
            QVERIFY(t.m_topologyId.isEmpty());
            QVERIFY(t.topologyFrame(frame, 1000).isEmpty());
        });
    }
    void topologyQueryRequiresMatchingIndependentSurfaces() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            RemoteTopologyCatalog catalog;
            const auto snapshot = catalog.observe({
                {.backendKey = u"Virtual-left"_s, .name = u"Virtual-left"_s,
                    .nativePixels = QSize(1920, 1080), .logicalGeometry = QRect(-1280, -100, 1280, 720),
                    .scale = 1.5, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-right"_s, .name = u"Virtual-right"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
                    .scale = 1.0, .enabled = true, .primary = true, .physical = false, .owner = t.m_handle->id},
            });
            QVERIFY(snapshot);
            std::optional<RemoteTopologyCatalog::Snapshot> published;
            t.setTopologyResolver([&published](const auto &) { return published; });
            const QJsonObject query{{u"type"_s, u"topology-query"_s}, {u"v"_s, 1}, {u"id"_s, u"two"_s}};
            QVERIFY(t.request(query, 1000).isEmpty());
            VideoFrame frame;
            frame.size = QSize(1920, 1080); frame.data = "fixture"; frame.isKeyFrame = true;
            frame.monitors = RemoteMonitorGeometry::projectToWire({
                {QPoint(-1280, -100), QSize(1920, 1080), 1.5, false},
                {QPoint(0, 0), QSize(1280, 720), 1.0, true},
            });
            frame.monitorIndex = 0;
            auto invalid = frame;
            invalid.monitors[1].geometry.moveLeft(1280); // Logical pixels cannot be reused as wire pixels.
            QVERIFY(t.topologyFrame(invalid, 1000).isEmpty());
            published = snapshot;
            const auto reply = t.topologyFrame(frame, 1000);
            QCOMPARE(reply.value(u"type"_s).toString(), u"topology"_s);
            QCOMPARE(reply.value(u"outputs"_s).toArray().size(), 2);
            const auto idleReply = t.request(query, 1000);
            QCOMPARE(idleReply.value(u"outputs"_s).toArray().size(), 2);
            QVERIFY(t.m_topologyId.isEmpty());
            QCOMPARE(reply.value(u"outputs"_s).toArray().first().toObject().value(u"logical"_s).toObject().value(u"x"_s).toInt(), -1280);
            const auto capabilities = reply.value(u"capabilities"_s).toObject();
            QVERIFY(capabilities.value(u"multiOutputCapture"_s).toBool());
            QVERIFY(capabilities.value(u"add"_s).toBool());
            QCOMPARE(capabilities.value(u"maxOutputs"_s).toInt(), 16);
            QVERIFY(capabilities.value(u"position"_s).toBool());
            QCOMPARE(capabilities.value(u"positionMin"_s).toInt(), 0);
        });
    }
    void topologyPreviewNeedsPublishedBackend() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &) {
            const QJsonObject preview{{u"type"_s, u"topology-preview"_s}, {u"v"_s, 1},
                {u"id"_s, u"preview-1"_s}, {u"generation"_s, u"generation-1"_s},
                {u"expectedRevision"_s, 1}, {u"allowRemoval"_s, false},
                {u"allowPhysicalChange"_s, false}, {u"operations"_s, QJsonArray{
                    QJsonObject{{u"op"_s, u"move"_s}, {u"output"_s, u"o-1"_s},
                        {u"position"_s, QJsonObject{{u"x"_s, 1280}, {u"y"_s, 0}}}}}}};
            QCOMPARE(t.request(preview, std::nullopt).value(u"code"_s).toString(), u"not-owner"_s);
            QCOMPARE(t.request(preview, 1001).value(u"code"_s).toString(), u"not-owner"_s);
            const auto reply = t.request(preview, 1000);
            QCOMPARE(reply.value(u"type"_s).toString(), u"topology-error"_s);
            QCOMPARE(reply.value(u"id"_s).toString(), u"preview-1"_s);
            QCOMPARE(reply.value(u"code"_s).toString(), u"unsupported"_s);
            auto malformed = preview;
            malformed.insert(u"owner"_s, u"another-user"_s);
            QCOMPARE(t.request(malformed, 1000).value(u"code"_s).toString(), u"invalid"_s);
        });
    }
    void retainedPositionPreviewCommitRequiresRevisionAndFullRecapture() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            RemoteTopologyCatalog catalog;
            const auto initial = catalog.observe({
                {.backendKey = u"Virtual-0"_s, .name = u"Virtual-0"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
                    .scale = 1.0, .enabled = true, .primary = true, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-1"_s, .name = u"Virtual-1"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(1280, 0, 1280, 720),
                    .scale = 1.0, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id},
            });
            QVERIFY(initial);
            auto current = *initial;
            t.setTopologyResolver([&current](const auto &) { return std::optional(current); });
            const auto secondId = current.outputs[1].id;
            QJsonObject preview{{u"type"_s, u"topology-preview"_s}, {u"v"_s, 1},
                {u"id"_s, u"move-1"_s}, {u"generation"_s, current.generation},
                {u"expectedRevision"_s, double(current.revision)}, {u"allowRemoval"_s, false},
                {u"allowPhysicalChange"_s, false}, {u"operations"_s, QJsonArray{
                    QJsonObject{{u"op"_s, u"move"_s}, {u"output"_s, secondId},
                        {u"position"_s, QJsonObject{{u"x"_s, 1280}, {u"y"_s, 100}}}}}}};
            QCOMPARE(t.request(preview, 1001).value(u"code"_s).toString(), u"not-owner"_s);
            const auto answer = t.request(preview, 1000);
            QCOMPARE(answer.value(u"type"_s).toString(), u"topology-preview"_s);
            QCOMPARE(answer.value(u"after"_s).toArray()[1].toObject().value(u"logical"_s).toObject().value(u"y"_s).toInt(), 100);
            const QString token = answer.value(u"token"_s).toString();
            QVERIFY(!token.isEmpty());
            QCOMPARE(t.request(preview, 1000).value(u"token"_s).toString(), token);
            QJsonObject commit{{u"type"_s, u"topology-commit"_s}, {u"v"_s, 1},
                {u"id"_s, u"move-1"_s}, {u"token"_s, token}, {u"generation"_s, current.generation},
                {u"expectedRevision"_s, double(current.revision)}};
            auto bad = commit;
            bad.insert(u"token"_s, u"wrong"_s);
            QCOMPARE(t.request(bad, 1000).value(u"code"_s).toString(), u"invalid"_s);
            QVERIFY(t.request(commit, 1000).isEmpty());
            QVERIFY(t.request(commit, 1000).isEmpty()); // In-flight retry is not an early error.
            const auto records = workerRecords(worker);
            std::optional<ConsoleWorkerWire::Position> command;
            for (const auto &record : records) if (auto parsed = ConsoleWorkerWire::position(record)) command = parsed;
            QVERIFY(command);
            QCOMPARE(command->output, u"Virtual-1"_s);
            QCOMPARE(command->globalLogical, QPoint(1280, 100));
            QCOMPARE(command->generation, t.m_controlGeneration);
            const auto stale = t.positionResult({command->requestId, command->generation, {}}, 1000);
            QCOMPARE(stale.value(u"code"_s).toString(), u"partial"_s); // No metadata-only success.
            QVERIFY(t.m_positionId.isEmpty());

            preview.insert(u"id"_s, u"move-2"_s);
            const auto secondPreview = t.request(preview, 1000);
            const QString secondToken = secondPreview.value(u"token"_s).toString();
            QVERIFY(!secondToken.isEmpty());
            commit.insert(u"id"_s, u"move-2"_s);
            commit.insert(u"token"_s, secondToken);
            QVERIFY(t.request(commit, 1000).isEmpty());
            const auto secondCommand = t.m_positionWorkerId;
            current.outputs[1].output.logicalGeometry.moveTop(100);
            current.revision = initial->revision + 1;
            const auto success = t.positionResult({secondCommand, t.m_controlGeneration, {}}, 1000);
            QCOMPARE(success.value(u"type"_s).toString(), u"topology-result"_s);
            QVERIFY(success.value(u"ok"_s).toBool());
            QCOMPARE(success.value(u"topology"_s).toObject().value(u"revision"_s).toInt(), 2);
            QCOMPARE(t.request(commit, 1000).value(u"code"_s).toString(), u"invalid"_s); // Token consumed.
        });
    }
    void retainedPrimaryRequiresOneUsePreviewAndFullRecapture() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            RemoteTopologyCatalog catalog;
            const auto initial = catalog.observe({
                {.backendKey = u"Virtual-0"_s, .name = u"Virtual-0"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = true, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-1"_s, .name = u"Virtual-1"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(1280, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id},
            });
            QVERIFY(initial);
            auto current = *initial;
            t.setTopologyResolver([&current](const auto &) { return std::optional(current); });
            QJsonObject preview{{u"type"_s, u"topology-preview"_s}, {u"v"_s, 1},
                {u"id"_s, u"primary-1"_s}, {u"generation"_s, current.generation},
                {u"expectedRevision"_s, double(current.revision)}, {u"allowRemoval"_s, false},
                {u"allowPhysicalChange"_s, false}, {u"operations"_s, QJsonArray{
                    QJsonObject{{u"op"_s, u"primary"_s}, {u"output"_s, current.outputs[1].id}}}}};
            QCOMPARE(t.request(preview, 1000).value(u"code"_s).toString(), u"unsupported"_s);
            t.m_experimentalPrimary = true;
            QCOMPARE(t.request(preview, 1001).value(u"code"_s).toString(), u"not-owner"_s);
            const auto answer = t.request(preview, 1000);
            QCOMPARE(answer.value(u"type"_s).toString(), u"topology-preview"_s);
            QCOMPARE(answer.value(u"after"_s).toArray()[1].toObject().value(u"primary"_s).toBool(), true);
            QJsonObject commit{{u"type"_s, u"topology-commit"_s}, {u"v"_s, 1},
                {u"id"_s, u"primary-1"_s}, {u"token"_s, answer.value(u"token"_s)},
                {u"generation"_s, current.generation}, {u"expectedRevision"_s, double(current.revision)}};
            QVERIFY(t.request(commit, 1000).isEmpty());
            QVERIFY(t.request(commit, 1000).isEmpty());
            std::optional<ConsoleWorkerWire::Primary> command;
            for (const auto &record : workerRecords(worker))
                if (auto parsed = ConsoleWorkerWire::primary(record)) command = parsed;
            QVERIFY(command);
            QCOMPARE(command->output, u"Virtual-1"_s);
            QCOMPARE(t.topologyResizeResult({command->requestId, command->generation, {}}, 1000)
                .value(u"code"_s).toString(), u"partial"_s); // Metadata-only cannot succeed.

            preview[u"id"_s] = u"primary-2"_s;
            const auto second = t.request(preview, 1000);
            QCOMPARE(second.value(u"type"_s).toString(), u"topology-preview"_s);
            commit[u"id"_s] = u"primary-2"_s;
            commit[u"token"_s] = second.value(u"token"_s);
            QVERIFY(t.request(commit, 1000).isEmpty());
            const auto workerId = t.m_topologyResizeWorkerId;
            auto before = initial->outputs[0].output;
            auto selected = initial->outputs[1].output;
            before.primary = false;
            selected.primary = true;
            const auto observed = catalog.observe({before, selected});
            QVERIFY(observed);
            current = *observed;
            const auto result = t.topologyResizeResult({workerId, t.m_controlGeneration, {}}, 1000);
            QCOMPARE(result.value(u"type"_s).toString(), u"topology-result"_s);
            QVERIFY(result.value(u"ok"_s).toBool());
            QCOMPARE(result.value(u"topology"_s).toObject().value(u"revision"_s).toInt(), 2);
            QCOMPARE(result.value(u"topology"_s).toObject().value(u"capabilities"_s).toObject()
                .value(u"primary"_s).toBool(), true);
            QCOMPARE(t.request(commit, 1000).value(u"code"_s).toString(), u"invalid"_s);
        });
    }
    void retainedPositionBatchRequiresOneUsePreviewAndExactCatalog() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            RemoteTopologyCatalog catalog;
            const auto initial = catalog.observe({
                {.backendKey = u"Virtual-0"_s, .name = u"Virtual-0"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = true, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-1"_s, .name = u"Virtual-1"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(1280, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id},
            });
            QVERIFY(initial);
            auto current = *initial;
            t.setTopologyResolver([&current](const auto &) { return std::optional(current); });
            const QJsonArray moves{
                QJsonObject{{u"op"_s, u"move"_s}, {u"output"_s, current.outputs[0].id},
                    {u"position"_s, QJsonObject{{u"x"_s, 1280}, {u"y"_s, 0}}}},
                QJsonObject{{u"op"_s, u"move"_s}, {u"output"_s, current.outputs[1].id},
                    {u"position"_s, QJsonObject{{u"x"_s, 0}, {u"y"_s, 0}}}},
            };
            QJsonObject preview{{u"type"_s, u"topology-preview"_s}, {u"v"_s, 1},
                {u"id"_s, u"batch-1"_s}, {u"generation"_s, current.generation},
                {u"expectedRevision"_s, double(current.revision)}, {u"allowRemoval"_s, false},
                {u"allowPhysicalChange"_s, false}, {u"operations"_s, moves}};
            QCOMPARE(t.request(preview, 1001).value(u"code"_s).toString(), u"not-owner"_s);
            auto duplicate = preview;
            auto duplicated = moves;
            duplicated[1] = duplicated[0];
            duplicate[u"operations"_s] = duplicated;
            QCOMPARE(t.request(duplicate, 1000).value(u"code"_s).toString(), u"unsupported"_s);
            auto mixed = preview;
            auto mixedOps = moves;
            mixedOps[1] = QJsonObject{{u"op"_s, u"remove"_s}, {u"output"_s, current.outputs[1].id}};
            mixed[u"operations"_s] = mixedOps;
            QCOMPARE(t.request(mixed, 1000).value(u"code"_s).toString(), u"unsupported"_s);
            const auto answer = t.request(preview, 1000);
            QCOMPARE(answer.value(u"type"_s).toString(), u"topology-preview"_s);
            const auto after = answer.value(u"after"_s).toArray();
            QCOMPARE(after[0].toObject().value(u"logical"_s).toObject().value(u"x"_s).toInt(), 1280);
            QCOMPARE(after[1].toObject().value(u"logical"_s).toObject().value(u"x"_s).toInt(), 0);
            QString token = answer.value(u"token"_s).toString();
            QVERIFY(!token.isEmpty());
            QCOMPARE(t.request(preview, 1000).value(u"token"_s).toString(), token);
            auto altered = preview;
            auto alteredMoves = moves;
            auto alteredSecond = alteredMoves[1].toObject();
            alteredSecond[u"position"_s] = QJsonObject{{u"x"_s, 0}, {u"y"_s, 100}};
            alteredMoves[1] = alteredSecond;
            altered[u"operations"_s] = alteredMoves;
            const auto alteredReply = t.request(altered, 1000);
            QCOMPARE(alteredReply.value(u"type"_s).toString(), u"topology-preview"_s);
            QVERIFY(alteredReply.value(u"token"_s).toString() != token);
            token = t.request(preview, 1000).value(u"token"_s).toString();
            QVERIFY(!token.isEmpty());
            QVERIFY(token != alteredReply.value(u"token"_s).toString());
            QJsonObject commit{{u"type"_s, u"topology-commit"_s}, {u"v"_s, 1},
                {u"id"_s, u"batch-1"_s}, {u"token"_s, token},
                {u"generation"_s, current.generation}, {u"expectedRevision"_s, double(current.revision)}};
            QVERIFY(t.request(commit, 1000).isEmpty());
            QVERIFY(t.request(commit, 1000).isEmpty());
            std::optional<ConsoleWorkerWire::PositionBatch> command;
            for (const auto &record : workerRecords(worker))
                if (auto parsed = ConsoleWorkerWire::positionBatch(record)) command = parsed;
            QVERIFY(command);
            QCOMPARE(command->targets.size(), 2);
            QCOMPARE(command->targets[0].output, u"Virtual-0"_s);
            QCOMPARE(command->targets[0].globalLogical, QPoint(1280, 0));
            QCOMPARE(command->targets[1].output, u"Virtual-1"_s);
            QCOMPARE(command->targets[1].globalLogical, QPoint(0, 0));
            QVERIFY(t.positionResult({command->requestId, command->generation, {}}, 1000).isEmpty());
            QCOMPARE(t.positionResult({command->requestId, command->generation, {}}, 1000, true)
                .value(u"code"_s).toString(), u"partial"_s); // Worker metadata alone is insufficient.
            preview[u"id"_s] = u"batch-2"_s;
            const auto second = t.request(preview, 1000);
            QCOMPARE(second.value(u"type"_s).toString(), u"topology-preview"_s);
            commit[u"id"_s] = u"batch-2"_s;
            commit[u"token"_s] = second.value(u"token"_s);
            QVERIFY(t.request(commit, 1000).isEmpty());
            const auto workerId = t.m_positionWorkerId;
            auto left = initial->outputs[0].output;
            auto right = initial->outputs[1].output;
            left.logicalGeometry.moveLeft(1280);
            right.logicalGeometry.moveLeft(0);
            const auto updated = catalog.observe({left, right});
            QVERIFY(updated);
            current = *updated;
            QCOMPARE(current.revision, initial->revision + 1);
            const auto success = t.positionResult({workerId, t.m_controlGeneration, {}}, 1000, true);
            QCOMPARE(success.value(u"type"_s).toString(), u"topology-result"_s);
            QVERIFY(success.value(u"ok"_s).toBool());
            QCOMPARE(success.value(u"topology"_s).toObject().value(u"revision"_s).toInt(), 2);
            QCOMPARE(t.request(commit, 1000).value(u"code"_s).toString(), u"invalid"_s);
        });
    }
    void experimentalRetainedResizeRequiresOneUseCommitAndExactCatalog() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            RemoteTopologyCatalog catalog;
            const auto initial = catalog.observe({
                {.backendKey = u"Virtual-0"_s, .name = u"Virtual-0"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = true, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-1"_s, .name = u"Virtual-1"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(1280, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id},
            });
            QVERIFY(initial);
            auto current = *initial;
            t.setTopologyResolver([&current](const auto &) { return std::optional(current); });
            const QString targetId = current.outputs[1].id;
            QJsonObject preview{{u"type"_s, u"topology-preview"_s}, {u"v"_s, 1},
                {u"id"_s, u"resize-1"_s}, {u"generation"_s, current.generation},
                {u"expectedRevision"_s, double(current.revision)}, {u"allowRemoval"_s, false},
                {u"allowPhysicalChange"_s, false}, {u"operations"_s, QJsonArray{
                    QJsonObject{{u"op"_s, u"resize"_s}, {u"output"_s, targetId},
                        {u"pixels"_s, QJsonObject{{u"width"_s, 1920}, {u"height"_s, 1080}}},
                        {u"scale"_s, 1.5}}}}};
            t.m_experimentalMultiResize = false;
            QCOMPARE(t.request(preview, 1000).value(u"code"_s).toString(), u"unsupported"_s);
            t.m_experimentalMultiResize = true; // Only the private source probe opts in.
            QCOMPARE(t.request(preview, 1001).value(u"code"_s).toString(), u"not-owner"_s);
            auto collision = preview;
            collision[u"id"_s] = u"resize-collision"_s;
            auto collisionOp = collision.value(u"operations"_s).toArray().first().toObject();
            collisionOp[u"output"_s] = current.outputs[0].id;
            collisionOp[u"scale"_s] = 1.0;
            collision[u"operations"_s] = QJsonArray{collisionOp};
            QCOMPARE(t.request(collision, 1000).value(u"code"_s).toString(), u"overlap"_s);
            auto odd = preview;
            odd[u"id"_s] = u"resize-odd"_s;
            auto oddOp = odd.value(u"operations"_s).toArray().first().toObject();
            oddOp[u"pixels"_s] = QJsonObject{{u"width"_s, 1601}, {u"height"_s, 900}};
            odd[u"operations"_s] = QJsonArray{oddOp};
            QCOMPARE(t.request(odd, 1000).value(u"code"_s).toString(), u"unsupported"_s);
            const auto first = t.request(preview, 1000);
            QCOMPARE(first.value(u"type"_s).toString(), u"topology-preview"_s);
            QCOMPARE(first.value(u"after"_s).toArray()[1].toObject().value(u"pixels"_s).toObject().value(u"width"_s).toInt(), 1920);
            QCOMPARE(first.value(u"after"_s).toArray()[1].toObject().value(u"logical"_s).toObject().value(u"width"_s).toInt(), 1280);
            const QString token = first.value(u"token"_s).toString();
            QVERIFY(!token.isEmpty());
            QJsonObject commit{{u"type"_s, u"topology-commit"_s}, {u"v"_s, 1},
                {u"id"_s, u"resize-1"_s}, {u"token"_s, token},
                {u"generation"_s, current.generation}, {u"expectedRevision"_s, double(current.revision)}};
            QVERIFY(t.request(commit, 1000).isEmpty());
            QVERIFY(t.request(commit, 1000).isEmpty());
            std::optional<ConsoleWorkerWire::Resize> command;
            for (const auto &record : workerRecords(worker))
                if (auto parsed = ConsoleWorkerWire::resize(record)) command = parsed;
            QVERIFY(command);
            QCOMPARE(command->output, u"Virtual-1"_s);
            QCOMPARE(command->pixels, QSize(1920, 1080));
            QCOMPARE(command->scale, 1.5);
            QCOMPARE(command->generation, t.m_controlGeneration);
            QCOMPARE(t.topologyResizeResult({command->requestId, command->generation, {}}, 1000)
                .value(u"code"_s).toString(), u"partial"_s); // No metadata-only success.
            QVERIFY(t.m_topologyResizeId.isEmpty());

            preview[u"id"_s] = u"resize-2"_s;
            const auto second = t.request(preview, 1000);
            QCOMPARE(second.value(u"type"_s).toString(), u"topology-preview"_s);
            commit[u"id"_s] = u"resize-2"_s;
            commit[u"token"_s] = second.value(u"token"_s);
            QVERIFY(t.request(commit, 1000).isEmpty());
            const auto workerId = t.m_topologyResizeWorkerId;
            auto after = initial->outputs[1].output;
            after.nativePixels = QSize(1920, 1080);
            after.scale = 1.5;
            const auto updated = catalog.observe({initial->outputs[0].output, after});
            QVERIFY(updated);
            current = *updated;
            QCOMPARE(current.revision, initial->revision + 1);
            const auto success = t.topologyResizeResult({workerId, t.m_controlGeneration, {}}, 1000);
            QCOMPARE(success.value(u"type"_s).toString(), u"topology-result"_s);
            QVERIFY(success.value(u"ok"_s).toBool());
            const auto topology = success.value(u"topology"_s).toObject();
            QCOMPARE(topology.value(u"revision"_s).toInt(), 2);
            QVERIFY(topology.value(u"capabilities"_s).toObject().value(u"resize"_s).toBool());
            QVERIFY(topology.value(u"capabilities"_s).toObject().value(u"scale"_s).toBool());
            QCOMPARE(t.request(commit, 1000).value(u"code"_s).toString(), u"invalid"_s);
            QCOMPARE(t.request(preview, 1000).value(u"code"_s).toString(), u"stale-revision"_s);
        });
    }
    void managedFitPreviewBindsDependentReflowAndFullReadback() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            RemoteTopologyCatalog catalog;
            const auto initial = catalog.observe({
                {.backendKey = u"Virtual-0"_s, .name = u"Virtual-0"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = true, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-1"_s, .name = u"Virtual-1"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(1280, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id},
            });
            QVERIFY(initial);
            auto current = *initial;
            t.setTopologyResolver([&current](const auto &) { return std::optional(current); });
            QJsonObject preview{{u"type"_s, u"topology-fit-preview"_s}, {u"v"_s, 1},
                {u"id"_s, u"fit-1"_s}, {u"generation"_s, current.generation},
                {u"expectedRevision"_s, double(current.revision)}, {u"output"_s, current.outputs[0].id},
                {u"pixels"_s, QJsonObject{{u"width"_s, 1600}, {u"height"_s, 900}}},
                {u"scale"_s, 1.0}, {u"relations"_s, QJsonArray{
                    QJsonObject{{u"parent"_s, current.outputs[0].id}, {u"child"_s, current.outputs[1].id},
                        {u"edge"_s, u"right"_s}, {u"offset"_s, 0}}}}};
            t.m_experimentalMultiResize = false;
            QCOMPARE(t.request(preview, 1000).value(u"code"_s).toString(), u"unsupported"_s);
            t.m_experimentalMultiResize = true;
            QCOMPARE(t.request(preview, 1001).value(u"code"_s).toString(), u"not-owner"_s);
            auto malformed = preview;
            malformed[u"owner"_s] = u"forged"_s;
            QCOMPARE(t.request(malformed, 1000).value(u"code"_s).toString(), u"invalid"_s);
            const auto answer = t.request(preview, 1000);
            QCOMPARE(answer.value(u"type"_s).toString(), u"topology-preview"_s);
            QCOMPARE(answer.value(u"after"_s).toArray()[1].toObject().value(u"logical"_s).toObject().value(u"x"_s).toInt(), 1600);
            const auto token = answer.value(u"token"_s).toString();
            QVERIFY(!token.isEmpty());
            QJsonObject commit{{u"type"_s, u"topology-commit"_s}, {u"v"_s, 1},
                {u"id"_s, u"fit-1"_s}, {u"token"_s, token},
                {u"generation"_s, current.generation}, {u"expectedRevision"_s, double(current.revision)}};
            QVERIFY(t.request(commit, 1000).isEmpty());
            std::optional<ConsoleWorkerWire::ManagedFit> command;
            for (const auto &record : workerRecords(worker))
                if (auto parsed = ConsoleWorkerWire::managedFit(record)) command = parsed;
            QVERIFY(command);
            QCOMPARE(command->output, u"Virtual-0"_s);
            QCOMPARE(command->pixels, QSize(1600, 900));
            QCOMPARE(command->relations.size(), 1);
            QCOMPARE(command->relations[0].child, u"Virtual-1"_s);
            QCOMPARE(t.topologyResizeResult({command->requestId, command->generation, {}}, 1000)
                .value(u"code"_s).toString(), u"partial"_s); // No metadata-only success.
            preview[u"id"_s] = u"fit-2"_s;
            const auto second = t.request(preview, 1000);
            commit[u"id"_s] = u"fit-2"_s;
            commit[u"token"_s] = second.value(u"token"_s);
            QVERIFY(t.request(commit, 1000).isEmpty());
            const auto workerId = t.m_topologyResizeWorkerId;
            auto firstOutput = initial->outputs[0].output;
            firstOutput.nativePixels = QSize(1600, 900);
            firstOutput.logicalGeometry.setSize(QSize(1600, 900));
            auto secondOutput = initial->outputs[1].output;
            secondOutput.logicalGeometry.moveLeft(1600);
            const auto updated = catalog.observe({firstOutput, secondOutput});
            QVERIFY(updated);
            current = *updated;
            const auto success = t.topologyResizeResult({workerId, t.m_controlGeneration, {}}, 1000);
            QCOMPARE(success.value(u"type"_s).toString(), u"topology-result"_s);
            QVERIFY(success.value(u"ok"_s).toBool());
            QCOMPARE(t.request(commit, 1000).value(u"code"_s).toString(), u"invalid"_s);
        });
    }
    void retainedAddRequiresAuthenticatedCommitAndCapturedReadback() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            RemoteTopologyCatalog catalog;
            const auto initial = catalog.observe({
                {.backendKey = u"Virtual-0"_s, .name = u"Virtual-0"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = true, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-1"_s, .name = u"Virtual-1"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(1280, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id},
            });
            QVERIFY(initial);
            auto current = *initial;
            t.setTopologyResolver([&current](const auto &) { return std::optional(current); });
            QJsonObject preview{{u"type"_s, u"topology-preview"_s}, {u"v"_s, 1},
                {u"id"_s, u"add-1"_s}, {u"generation"_s, current.generation},
                {u"expectedRevision"_s, double(current.revision)}, {u"allowRemoval"_s, false},
                {u"allowPhysicalChange"_s, false}, {u"operations"_s, QJsonArray{
                    QJsonObject{{u"op"_s, u"add"_s}, {u"output"_s, u"new:extra"_s},
                        {u"position"_s, QJsonObject{{u"x"_s, 2560}, {u"y"_s, 0}}},
                        {u"pixels"_s, QJsonObject{{u"width"_s, 960}, {u"height"_s, 540}}},
                        {u"scale"_s, 1.0}}}}};
            QCOMPARE(t.request(preview, 1001).value(u"code"_s).toString(), u"not-owner"_s);
            const auto answer = t.request(preview, 1000);
            QCOMPARE(answer.value(u"type"_s).toString(), u"topology-preview"_s);
            const auto after = answer.value(u"after"_s).toArray();
            QCOMPARE(after.size(), 3);
            const QString backend = after.last().toObject().value(u"name"_s).toString();
            QVERIFY(backend.startsWith(u"Virtual-krdp-added-"_s));
            QCOMPARE(t.request(preview, 1000).value(u"token"_s).toString(), answer.value(u"token"_s).toString());
            QJsonObject commit{{u"type"_s, u"topology-commit"_s}, {u"v"_s, 1},
                {u"id"_s, u"add-1"_s}, {u"token"_s, answer.value(u"token"_s)},
                {u"generation"_s, current.generation}, {u"expectedRevision"_s, double(current.revision)}};
            QCOMPARE(t.request(commit, 1001).value(u"code"_s).toString(), u"not-owner"_s);
            QVERIFY(t.request(commit, 1000).isEmpty());
            QVERIFY(t.request(commit, 1000).isEmpty());
            std::optional<ConsoleWorkerWire::AddVirtual> command;
            for (const auto &record : workerRecords(worker))
                if (auto parsed = ConsoleWorkerWire::addVirtual(record)) command = parsed;
            QVERIFY(command);
            QCOMPARE(command->output, backend);
            QCOMPARE(command->pixels, QSize(960, 540));
            QCOMPARE(command->globalLogical, QPoint(2560, 0));
            QCOMPARE(t.addVirtualResult({command->requestId, command->generation, {}}, 1000)
                .value(u"code"_s).toString(), u"partial"_s); // Worker metadata alone cannot succeed.

            preview.insert(u"id"_s, u"add-2"_s);
            const auto second = t.request(preview, 1000);
            const QString secondBackend = second.value(u"after"_s).toArray().last().toObject().value(u"name"_s).toString();
            commit.insert(u"id"_s, u"add-2"_s);
            commit.insert(u"token"_s, second.value(u"token"_s));
            QVERIFY(t.request(commit, 1000).isEmpty());
            const auto workerId = t.m_addWorkerId;
            const auto updated = catalog.observe({initial->outputs[0].output, initial->outputs[1].output,
                {.backendKey = secondBackend, .name = secondBackend,
                    .nativePixels = QSize(960, 540), .logicalGeometry = QRect(2560, 0, 960, 540),
                    .scale = 1, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id}});
            QVERIFY(updated);
            current = *updated;
            const auto result = t.addVirtualResult({workerId, t.m_controlGeneration, {}}, 1000);
            QCOMPARE(result.value(u"type"_s).toString(), u"topology-result"_s);
            QVERIFY(result.value(u"ok"_s).toBool());
            QCOMPARE(result.value(u"topology"_s).toObject().value(u"revision"_s).toInt(), 2);
            QCOMPARE(result.value(u"topology"_s).toObject().value(u"outputs"_s).toArray().size(), 3);
            QCOMPARE(t.request(commit, 1000).value(u"code"_s).toString(), u"invalid"_s);
        });
    }
    void retainedRemoveRequiresOwnedAddOutputAndCapturedReadback() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            RemoteTopologyCatalog catalog;
            const auto initial = catalog.observe({
                {.backendKey = u"Virtual-0"_s, .name = u"Virtual-0"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = true, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-1"_s, .name = u"Virtual-1"_s,
                    .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(1280, 0, 1280, 720),
                    .scale = 1, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-krdp-added-test"_s, .name = u"Virtual-krdp-added-test"_s,
                    .nativePixels = QSize(960, 540), .logicalGeometry = QRect(2560, 0, 960, 540),
                    .scale = 1, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id},
            });
            QVERIFY(initial);
            auto current = *initial;
            t.setTopologyResolver([&current](const auto &) { return std::optional(current); });
            const auto target = current.outputs.last().id;
            QJsonObject preview{{u"type"_s, u"topology-preview"_s}, {u"v"_s, 1},
                {u"id"_s, u"remove-1"_s}, {u"generation"_s, current.generation},
                {u"expectedRevision"_s, double(current.revision)}, {u"allowRemoval"_s, true},
                {u"allowPhysicalChange"_s, false}, {u"operations"_s, QJsonArray{
                    QJsonObject{{u"op"_s, u"remove"_s}, {u"output"_s, target}}}}};
            QCOMPARE(t.request(preview, 1001).value(u"code"_s).toString(), u"not-owner"_s);
            auto unsafe = preview;
            unsafe[u"allowRemoval"_s] = false;
            QCOMPARE(t.request(unsafe, 1000).value(u"code"_s).toString(), u"unsupported"_s);
            unsafe = preview;
            unsafe[u"operations"_s] = QJsonArray{QJsonObject{{u"op"_s, u"remove"_s},
                {u"output"_s, current.outputs.first().id}}};
            QCOMPARE(t.request(unsafe, 1000).value(u"code"_s).toString(), u"unsupported"_s);
            const auto answer = t.request(preview, 1000);
            QCOMPARE(answer.value(u"type"_s).toString(), u"topology-preview"_s);
            QCOMPARE(answer.value(u"after"_s).toArray().size(), 2);
            QJsonObject commit{{u"type"_s, u"topology-commit"_s}, {u"v"_s, 1},
                {u"id"_s, u"remove-1"_s}, {u"token"_s, answer.value(u"token"_s)},
                {u"generation"_s, current.generation}, {u"expectedRevision"_s, double(current.revision)}};
            QCOMPARE(t.request(commit, 1001).value(u"code"_s).toString(), u"not-owner"_s);
            QVERIFY(t.request(commit, 1000).isEmpty());
            QVERIFY(t.request(commit, 1000).isEmpty());
            std::optional<ConsoleWorkerWire::RemoveVirtual> command;
            for (const auto &record : workerRecords(worker))
                if (auto parsed = ConsoleWorkerWire::removeVirtual(record)) command = parsed;
            QVERIFY(command);
            QCOMPARE(command->output, u"Virtual-krdp-added-test"_s);
            QCOMPARE(t.removeVirtualResult({command->requestId, command->generation, {}}, 1000)
                .value(u"code"_s).toString(), u"partial"_s);
            preview[u"id"_s] = u"remove-2"_s;
            const auto second = t.request(preview, 1000);
            commit[u"id"_s] = u"remove-2"_s;
            commit[u"token"_s] = second.value(u"token"_s);
            QVERIFY(t.request(commit, 1000).isEmpty());
            const auto workerId = t.m_removeWorkerId;
            const auto updated = catalog.observe({initial->outputs[0].output, initial->outputs[1].output});
            QVERIFY(updated);
            current = *updated;
            const auto result = t.removeVirtualResult({workerId, t.m_controlGeneration, {}}, 1000);
            QCOMPARE(result.value(u"type"_s).toString(), u"topology-result"_s);
            QVERIFY(result.value(u"ok"_s).toBool());
            QCOMPARE(result.value(u"topology"_s).toObject().value(u"revision"_s).toInt(), 2);
            QCOMPARE(result.value(u"topology"_s).toObject().value(u"outputs"_s).toArray().size(), 2);
            QCOMPARE(t.request(commit, 1000).value(u"code"_s).toString(), u"invalid"_s);
        });
    }
    void virtualResizeStrictSchema() {
        auto request = resizeRequest();
        QVERIFY(VirtualResizeProtocol::parse(request));
        for (const QString &field : {u"v"_s, u"width"_s, u"height"_s, u"scale"_s}) {
            auto invalid = request; invalid[field] = true;
            QVERIFY(!VirtualResizeProtocol::parse(invalid));
        }
        for (const auto value : {319.0, 321.0, 4098.0, 1920.5}) {
            auto invalid = request; invalid[u"width"_s] = value;
            QVERIFY(!VirtualResizeProtocol::parse(invalid));
        }
        request[u"output"_s] = u"DP-1"_s;
        QVERIFY(!VirtualResizeProtocol::parse(request));
        request.remove(u"output"_s); request[u"scale"_s] = 1.333;
        QCOMPARE(VirtualResizeProtocol::parse(request)->scale, 160.0 / 120);
        request[u"id"_s] = QString(65, u'x');
        QVERIFY(!VirtualResizeProtocol::parse(request));
    }
    void virtualResizeDispatchCorrelationAndRevoke() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            QVERIFY(!t.request(resizeRequest(), std::nullopt).value(u"ok"_s).toBool());
            QVERIFY(!t.request(resizeRequest(), 1001).value(u"ok"_s).toBool());
            QVERIFY(t.request(resizeRequest(), 1000).isEmpty()); // Not an early success.
            QVERIFY(t.m_resizeDeadline.isActive());
            std::optional<ConsoleWorkerWire::Resize> sent;
            for (const auto &record : workerRecords(worker)) if (auto resize = ConsoleWorkerWire::resize(record)) sent = resize;
            QVERIFY(sent); QCOMPARE(sent->output, u"virtual-desktop"_s);
            QCOMPARE(sent->pixels, QSize(1920, 1080)); QCOMPARE(sent->scale, 1.25);
            QCOMPARE(sent->generation, t.m_controlGeneration);
            QVERIFY(t.request(resizeRequest(), 1000).isEmpty()); // Pending duplicate is coalesced.
            QCOMPARE(t.m_resizeWorkerId, sent->requestId);
            auto another = resizeRequest(); another[u"id"_s] = u"another-fit"_s;
            QVERIFY(!t.request(another, 1000).value(u"ok"_s).toBool());
            QVERIFY(t.resizeResult({sent->requestId + 1, sent->generation, {}}, 1000).isEmpty());
            QVERIFY(t.resizeResult({sent->requestId, sent->generation + 1, {}}, 1000).isEmpty());
            QCOMPARE(t.m_resizeId, u"fit-1"_s);
            const auto result = t.resizeResult({sent->requestId, sent->generation, {}}, 1000);
            QVERIFY(result.value(u"ok"_s).toBool()); QCOMPARE(result.value(u"id"_s).toString(), u"fit-1"_s);
            QVERIFY(t.m_resizeId.isEmpty()); QVERIFY(!t.m_resizeDeadline.isActive());
            QVERIFY(t.request(resizeRequest(), 1000).isEmpty());
            const auto second = t.m_resizeWorkerId; QVERIFY(second > sent->requestId);
            QVERIFY(t.resizeResult({sent->requestId, sent->generation, u"old failure"_s}, 1000).isEmpty());
            QCOMPARE(t.m_resizeWorkerId, second);
            t.revoke(); QVERIFY(t.m_resizeId.isEmpty()); QVERIFY(!t.m_resizeDeadline.isActive());
            QVERIFY(t.resizeResult({second, sent->generation, {}}, 1000).isEmpty());
        });
    }
    void virtualResizeFailureLostAuthorityAndExhaustion() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            QVERIFY(t.request(resizeRequest(), 1000).isEmpty());
            const auto refused = t.resizeResult({t.m_resizeWorkerId, t.m_resizeGeneration, u"readback mismatch"_s}, 1000);
            QVERIFY(!refused.value(u"ok"_s).toBool());
            QCOMPARE(refused.value(u"message"_s).toString(), u"readback mismatch"_s);
            QVERIFY(t.request(resizeRequest(), 1000).isEmpty());
            QVERIFY(t.resizeResult({t.m_resizeWorkerId, t.m_resizeGeneration, {}}, std::nullopt).isEmpty());
            QVERIFY(t.m_resizeId.isEmpty());
            t.m_nextResizeId = std::numeric_limits<quint64>::max();
            QVERIFY(!t.request(resizeRequest(), 1000).value(u"ok"_s).toBool());
            QVERIFY(t.m_resizeId.isEmpty());
        });
    }
    void virtualResizeTimeoutRetryCannotCompleteFromOldReply() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            QVERIFY(t.request(resizeRequest(), 1000).isEmpty());
            const auto previous = t.m_resizeWorkerId;
            const auto generation = t.m_resizeGeneration;
            t.m_resizeDeadline.start(1);
            QTRY_VERIFY(t.m_resizeId.isEmpty()); // Real timer; no fabricated PAM identity in callback.
            QVERIFY(t.request(resizeRequest(), 1000).isEmpty());
            QVERIFY(t.m_resizeWorkerId > previous);
            QVERIFY(t.resizeResult({previous, generation, {}}, 1000).isEmpty());
            QCOMPARE(t.m_resizeId, u"fit-1"_s);
            const auto busy = t.resizeResult({t.m_resizeWorkerId, generation, u"worker still settling"_s}, 1000);
            QVERIFY(!busy.value(u"ok"_s).toBool());
        });
    }
    void finalAudioOffRestoresWithoutFrameOrPriorityRequest() {
        for (int mode = 0; mode < 3; ++mode) microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            QVERIFY(t.request(priority(), 1000).value(u"ok"_s).toBool());
            ConsoleWorkerWire::MicrophonePolicy policy;
            if (mode == 0) QVERIFY(ok(media(t, 1000, false, true)));
            else {
                QVERIFY(media(t, 1000, true, false).isEmpty()); policy = t.m_microphonePolicy;
                QVERIFY(ok(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000)));
                t.m_microphonePump.stop();
            }
            QVERIFY(t.m_connection->audioPriorityActive());
            t.m_connection->videoStream()->setQualityCap(45); // Deterministic reduced local state, not a congestion simulation.
            QVERIFY(t.forwardVideoQuality(t.m_controlGeneration, 45, 1000));
            workerRecords(worker);
            QSignalSpy local(t.m_connection->videoStream(), &VideoStream::requestedQualityChanged);
            const auto response = mode == 2
                ? t.microphoneResult({policy.generation, policy.requestId, u"source lost"_s}, 1000)
                : media(t, 1000, false, false);
            QCOMPARE(ok(response), mode != 2);
            QVERIFY(!t.m_connection->audioPriorityActive());
            QVERIFY(!local.isEmpty()); QCOMPARE(local.last().at(0).value<quint8>(), quint8(80));
            bool restored = false;
            for (const auto &record : workerRecords(worker)) if (auto q = ConsoleWorkerWire::videoQuality(record)) {
                QCOMPARE(q->generation, t.m_controlGeneration); QCOMPARE(q->quality, quint8(80)); restored = true;
            }
            QVERIFY(restored); // No priority() query, extra frame or manual forwarding after audio-off.
        });
    }
    void microphoneStartAndFailureKeepPlaybackPriorityQuality() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            QVERIFY(ok(media(t, 1000, false)));
            QVERIFY(t.request(priority(), 1000).value(u"effective"_s).toBool());
            t.m_connection->videoStream()->setQualityCap(45);
            QVERIFY(t.forwardVideoQuality(t.m_controlGeneration, 45, 1000)); workerRecords(worker);
            QSignalSpy local(t.m_connection->videoStream(), &VideoStream::requestedQualityChanged);
            QVERIFY(media(t, 1000, true, true).isEmpty()); const auto policy = t.m_microphonePolicy;
            QVERIFY(!ok(t.microphoneResult({policy.generation, policy.requestId, u"startup failed"_s}, 1000)));
            QVERIFY(t.m_connection->audioPriorityActive()); QVERIFY(t.m_playback); QVERIFY(t.m_silenceHost);
            QVERIFY(local.isEmpty());
            for (const auto &record : workerRecords(worker)) QVERIFY(!ConsoleWorkerWire::videoQuality(record));
        });
    }
    void finalAudioOffRestoreMayDestroyTransport() {
        microphoneFixture([&](auto &t, auto &control, auto &, auto &) {
            QVERIFY(ok(media(t, 1000, false)));
            QVERIFY(t.request(priority(), 1000).value(u"effective"_s).toBool());
            QPointer<VirtualSessionTransport> alive(&t);
            const auto connection = t.m_connection;
            QObject::connect(connection->videoStream(), &VideoStream::requestedQualityChanged, connection, [alive](quint8 quality) {
                if (quality == 80 && alive) delete alive.data();
            });
            QVERIFY(media(t, 1000, false, false).isEmpty());
            QVERIFY(!alive); QVERIFY(!control.attachment(1)); QVERIFY(!connection->audioPriorityActive());
        });
    }
    void unboundAttachDoesNotAdvertiseAudioPriority() {
        microphoneFixture([&](auto &t, auto &control, auto &, auto &) {
            const auto session = t.m_handle->id;
            const QJsonObject attach{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1},
                {u"id"_s, u"capability-check"_s}, {u"action"_s, u"attach"_s}, {u"session"_s, session}};
            control.disconnected(1);
            const auto controlOnly = control.request(1000, 1, attach);
            QVERIFY(controlOnly.value(u"ok"_s).toBool()); QVERIFY(!controlOnly.contains(u"audioPriority"_s));
            // A Control success is insufficient: the real bind still refuses
            // this fixture's absent PAM identity and must not advertise support.
            const auto response = t.request(attach, 1000);
            QVERIFY(!response.value(u"ok"_s).toBool()); QVERIFY(!response.contains(u"audioPriority"_s));
        });
    }
    void audioPriorityProtocolAndEffectiveDirections() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &) {
            QVERIFY(!t.request(priority()).value(u"ok"_s).toBool()); // Actual unauthenticated PAM path.
            QVERIFY(!t.request(priority(), 1001).value(u"ok"_s).toBool());
            for (const auto &key : {u"v"_s, u"id"_s, u"enabled"_s}) {
                auto invalid = priority(); invalid.remove(key);
                QVERIFY(!t.request(invalid, 1000).value(u"ok"_s).toBool());
            }
            auto invalid = priority(); invalid[u"enabled"_s] = 1;
            QVERIFY(!t.request(invalid, 1000).value(u"ok"_s).toBool());
            invalid = priority(); invalid[u"id"_s] = QString(65, QLatin1Char('x'));
            QVERIFY(!t.request(invalid, 1000).value(u"ok"_s).toBool());
            auto reply = t.request(priority(), 1000);
            QVERIFY(reply.value(u"ok"_s).toBool()); QVERIFY(!reply.value(u"effective"_s).toBool());
            QCOMPARE(reply.value(u"id"_s).toString(), u"priority-1"_s);
            QVERIFY(ok(media(t, 1000, false, true)));
            QVERIFY(t.m_connection->audioPriorityActive());
            QVERIFY(t.request(priority(), 1000).value(u"effective"_s).toBool());
            QVERIFY(!t.request(priority(false), 1000).value(u"effective"_s).toBool());
            QVERIFY(!t.m_connection->audioPriorityActive());
            QVERIFY(media(t, 1000, true, false).isEmpty());
            const auto policy = t.m_microphonePolicy;
            QVERIFY(!t.request(priority(), 1000).value(u"effective"_s).toBool()); // Mic still pending.
            QVERIFY(ok(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000)));
            t.m_microphonePump.stop(); // No actual PAM/AUDIN in the fixture.
            QVERIFY(t.request(priority(), 1000).value(u"effective"_s).toBool());
            QVERIFY(ok(media(t, 1000, false, false)));
            QVERIFY(!t.m_connection->audioPriorityActive());
            QVERIFY(!t.request(priority(), 1000).value(u"effective"_s).toBool());
            t.revoke();
            QVERIFY(!t.request(priority(), 1000).value(u"ok"_s).toBool());
            t.m_connection->setDeviceEnabled(MediaDevice::Playback, true);
            QVERIFY(!t.m_connection->audioPriorityActive()); // No latent override after ownership loss.
        });
    }
    void audioPriorityWorkerQualityAndStaleBinding() {
        microphoneFixture([&](auto &t, auto &control, auto &endpoint, auto &worker) {
            const auto handle = *t.m_handle;
            const auto generation = t.m_controlGeneration;
            QVERIFY(ok(media(t, 1000, false)));
            QVERIFY(t.request(priority(), 1000).value(u"effective"_s).toBool());
            workerRecords(worker);
            // Production signal delivery is queued and must still refuse this
            // socket-free fixture's absent PAM identity.
            t.m_connection->videoStream()->requestedQualityChanged(43);
            for (const auto &r : workerRecords(worker)) QVERIFY(!ConsoleWorkerWire::videoQuality(r));
            QVERIFY(t.forwardVideoQuality(generation, 45, 1000));
            bool seen = false;
            for (const auto &r : workerRecords(worker)) if (auto q = ConsoleWorkerWire::videoQuality(r)) {
                QCOMPARE(q->generation, generation); QCOMPARE(q->quality, quint8(45)); seen = true;
            }
            QVERIFY(seen);
            QVERIFY(t.forwardVideoQuality(generation, 9, 1000));
            workerRecords(worker);
            QVERIFY(!t.forwardVideoQuality(generation, 101, 1000));
            QVERIFY(!t.forwardVideoQuality(generation, 40, 1001));
            QVERIFY(t.request(priority(false), 1000).value(u"ok"_s).toBool());
            QVERIFY(t.forwardVideoQuality(generation, 30, 1000)); // Queued reduction after off is clamped.
            for (const auto &r : workerRecords(worker)) if (auto q = ConsoleWorkerWire::videoQuality(r)) QCOMPARE(q->quality, quint8(80));
            // Exercise the same receiver with the private identity seam, queued
            // under its OLD generation. Never fabricate PAM in production.
            bool staleDelivered = true;
            QMetaObject::invokeMethod(&t, [&t, generation, &staleDelivered] {
                staleDelivered = t.forwardVideoQuality(generation, 25, 1000);
            }, Qt::QueuedConnection);
            t.m_connection->videoStream()->requestedQualityChanged(25); // Also leave a real old queued signal.
            control.disconnected(1);
            QVERIFY(!t.m_connection->audioPriorityActive());
            QVERIFY(control.request(1000, 1, {{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1},
                {u"id"_s, u"audio-reattach"_s}, {u"action"_s, u"attach"_s}, {u"session"_s, handle.id}}).value(u"ok"_s).toBool());
            QVERIFY(!t.activateBinding(handle, &endpoint));
            const auto next = t.m_controlGeneration; QVERIFY(next != generation);
            QVERIFY(ok(media(t, 1000, false)));
            QVERIFY(t.request(priority(), 1000).value(u"effective"_s).toBool());
            bool restoredOld = false, initializedNew = false;
            for (const auto &r : workerRecords(worker)) if (auto q = ConsoleWorkerWire::videoQuality(r)) {
                QCOMPARE(q->quality, quint8(80));
                restoredOld |= q->generation == generation; initializedNew |= q->generation == next;
            }
            QVERIFY(!staleDelivered); QVERIFY(restoredOld); QVERIFY(initializedNew);
            QVERIFY(!t.forwardVideoQuality(generation, 40, 1000));
            QVERIFY(t.forwardVideoQuality(next, 50, 1000));
            bool newQuality = false;
            for (const auto &r : workerRecords(worker)) if (auto q = ConsoleWorkerWire::videoQuality(r)) {
                QCOMPARE(q->generation, next); QCOMPARE(q->quality, quint8(50)); newQuality = true;
            }
            QVERIFY(newQuality);
            worker.abort(); QTRY_VERIFY(!endpoint.ready());
            QVERIFY(!t.m_connection->audioPriorityActive());
            QVERIFY(!t.forwardVideoQuality(next, 40, 1000));
            QVERIFY(!t.request(priority(), 1000).value(u"ok"_s).toBool());
        });
    }
    void microphoneAcknowledgementEchoesTheDeviceRequestId() {
        // KRDPCTL v2: the worker's acknowledgement is the reply to the `device` request, so it
        // carries that request's requestId; a later failure is unsolicited and carries none.
        microphoneFixture([&](auto &t, auto &, auto &, auto &) {
            auto record = device(u"microphone"_s, u"on"_s); record.insert(u"requestId"_s, u"mic-1"_s);
            t.deliverControlRecord(record, 1000);
            QVERIFY(t.m_replyRequestId.isEmpty());
            const auto policy = t.m_microphonePolicy; QVERIFY(policy.enabled);
            const auto ack = t.microphoneResult({policy.generation, policy.requestId, {}}, 1000);
            QVERIFY(ok(ack)); QCOMPARE(state(ack), u"on"_s);
            QCOMPARE(ack.value(u"requestId"_s).toString(), u"mic-1"_s);
            const auto lost = t.microphoneResult({policy.generation, policy.requestId, u"source lost"_s}, 1000);
            QVERIFY(!ok(lost)); QVERIFY(!lost.contains(u"requestId"_s));

            record.insert(u"requestId"_s, u"mic-2"_s);
            t.deliverControlRecord(record, 1000);
            QVERIFY(t.m_microphonePolicy.enabled);
            const auto timeout = t.microphoneTimeout();
            QCOMPARE(timeout.value(u"code"_s).toString(), u"timeout"_s);
            QCOMPARE(timeout.value(u"requestId"_s).toString(), u"mic-2"_s);
            QVERIFY(t.microphoneTimeout().isEmpty()); // answered once
        });
    }
    void deviceCameraStartReselectAndRevocation() {
        // A new consent period invalidates the old worker result. Detach answers
        // a pending camera request exactly once, with the other devices.
        microphoneFixture([&](auto &t, auto &, auto &, auto &) {
            t.setCameraLoopbackDevice(u"/dev/video42"_s);
            QList<QJsonObject> pushed;
            t.m_recordPushed = [&pushed](const QJsonObject &record) { pushed.append(record); };
            auto camera = device(u"camera"_s, u"on"_s); camera.insert(u"requestId"_s, u"c1"_s);
            t.deliverControlRecord(camera, 1000);
            QVERIFY(t.m_cameraPolicy.enabled);
            QCOMPARE(t.m_cameraPolicy.loopbackDevice, u"/dev/video42"_s);
            const auto oldCamera = t.m_cameraPolicy;
            QCOMPARE(state(t.request(device(u"camera"_s, u"query"_s), 1000)), u"starting"_s);
            camera.insert(u"action"_s, u"reselect"_s); camera.insert(u"requestId"_s, u"c2"_s);
            t.deliverControlRecord(camera, 1000);
            QVERIFY(t.m_cameraPolicy.enabled);
            QVERIFY(t.m_cameraPolicy.requestId != oldCamera.requestId);
            QVERIFY(t.cameraResult({oldCamera.generation, oldCamera.requestId, {}}, 1000).isEmpty());
            QCOMPARE(state(t.request(device(u"microphone"_s, u"query"_s))), u"off"_s); // a query needs no ownership
            auto invalid = device(u"microphone"_s, u"on"_s); invalid.insert(u"silenceHost"_s, true);
            QCOMPARE(t.request(invalid, 1000).value(u"code"_s).toString(), u"invalid"_s);
            QVERIFY(ok(t.request(device(u"playback"_s, u"on"_s), 1000)));
            auto start = device(u"microphone"_s, u"on"_s); start.insert(u"requestId"_s, u"v1"_s);
            t.deliverControlRecord(start, 1000);
            QVERIFY(t.m_microphonePolicy.enabled);
            QCOMPARE(state(t.request(device(u"microphone"_s, u"query"_s), 1000)), u"starting"_s);
            QVERIFY(pushed.isEmpty());
            t.revoke();
            QCOMPARE(pushed.size(), 3);
            QCOMPARE(pushed.at(0).value(u"device"_s).toString(), u"microphone"_s);
            QCOMPARE(state(pushed.at(0)), u"off"_s);
            QCOMPARE(pushed.at(0).value(u"code"_s).toString(), u"detached"_s);
            QCOMPARE(pushed.at(0).value(u"requestId"_s).toString(), u"v1"_s);
            QCOMPARE(pushed.at(1).value(u"device"_s).toString(), u"camera"_s);
            QCOMPARE(pushed.at(1).value(u"code"_s).toString(), u"detached"_s);
            QCOMPARE(pushed.at(1).value(u"requestId"_s).toString(), u"c2"_s);
            QCOMPARE(pushed.at(2).value(u"device"_s).toString(), u"playback"_s);
            QCOMPARE(pushed.at(2).value(u"code"_s).toString(), u"detached"_s);
            QVERIFY(!pushed.at(2).contains(u"requestId"_s));
            t.revoke(); // nothing on any more: nothing pushed again
            QCOMPARE(pushed.size(), 3);
            // After the detach the connection owns nothing: `not-owner`.
            QCOMPARE(t.request(device(u"microphone"_s, u"on"_s), 1000).value(u"code"_s).toString(), u"not-owner"_s);
        });
    }
    // AUD-D3: StandardClientMedia for the session's client. A client without
    // KRDPCTL gets playback and the microphone from standard negotiation once
    // it is bound (never silenceHost); with the setting off,
    // or once it spoke KRDPCTL, nothing.
    static std::optional<RdpConnection::StandardMediaChannels> joinedEverything(RdpConnection *connection) {
        auto channels = connection->standardMediaChannels();
        if (channels) channels->playback = channels->dynamic = true;
        return channels;
    }
    void standardClientMediaForTheSessionClient_data() {
        QTest::addColumn<bool>("enabled");
        QTest::newRow("StandardClientMedia on") << true;
        QTest::newRow("StandardClientMedia off") << false;
    }
    void standardClientMediaForTheSessionClient() {
        QFETCH(bool, enabled);
        QList<QJsonObject> pushed; // outlives the transport (its teardown pushes `detached`)
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            QVERIFY(!t.m_connection->hasControlChannel());
            t.m_standardChannels = &VirtualSessionTransportTest::joinedEverything;
            t.m_recordPushed = [&pushed](const QJsonObject &record) { pushed.append(record); };
            t.applyStandardMedia(std::nullopt); // no owner identity: nothing
            QVERIFY(!t.m_playback); QVERIFY(!t.m_microphonePolicy.enabled);
            t.applyStandardMedia(1001); // not the desktop's owner: nothing
            QVERIFY(!t.m_playback); QVERIFY(!t.m_microphonePolicy.enabled);
            workerRecords(worker);
            t.applyStandardMedia(1000);
            QCOMPARE(t.m_playback, enabled);
            QVERIFY(!t.m_silenceHost);
            QCOMPARE(t.m_microphonePolicy.enabled, enabled);
            bool mediaOn = false, microphoneOn = false, cameraOn = false;
            for (const auto &record : workerRecords(worker)) {
                if (auto m = ConsoleWorkerWire::media(record); m && m->playback && !m->silenceHost) mediaOn = true;
                if (auto p = ConsoleWorkerWire::microphonePolicy(record); p && p->enabled) microphoneOn = true;
                if (auto p = ConsoleWorkerWire::cameraPolicy(record); p && p->enabled) cameraOn = true;
            }
            QCOMPARE(mediaOn, enabled);
            QCOMPARE(microphoneOn, enabled);
            QCOMPARE(cameraOn, enabled);
            QCOMPARE(state(t.request(device(u"camera"_s, u"query"_s), 1000)), enabled ? u"starting"_s : u"off"_s);
            if (!enabled) return;
            QVERIFY(t.m_microphoneRequestId.isEmpty()); // nothing to answer
            const auto policy = t.m_microphonePolicy;
            const auto ack = t.microphoneResult({policy.generation, policy.requestId, {}}, 1000);
            QCOMPARE(state(ack), u"on"_s); QVERIFY(!ack.contains(u"requestId"_s));
            QVERIFY(t.m_microphoneReady);
            t.m_microphonePump.stop(); // Fixture has no PAM.
            t.applyStandardMedia(1000); // the running source is kept
            QCOMPARE(t.m_microphonePolicy, policy);
            QVERIFY(pushed.isEmpty());
        }, enabled);
    }
    void standardClientMediaNotAfterKrdpctl() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &) {
            t.m_standardChannels = &VirtualSessionTransportTest::joinedEverything;
            QVERIFY(ok(t.request(device(u"playback"_s, u"off"_s), 1000))); // asks for itself
            t.applyStandardMedia(1000);
            QVERIFY(!t.m_playback); QVERIFY(!t.m_microphonePolicy.enabled);
        });
        microphoneFixture([&](auto &t, auto &, auto &, auto &) {
            t.m_standardChannels = &VirtualSessionTransportTest::joinedEverything;
            t.request(priority(false), 1000); // audio-priority alone does not count
            QVERIFY(!t.m_spokeKrdpctl);
            t.request({{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"l"_s}, {u"action"_s, u"list"_s}}, 1000);
            QVERIFY(t.m_spokeKrdpctl);
            t.applyStandardMedia(1000);
            QVERIFY(!t.m_playback); QVERIFY(!t.m_microphonePolicy.enabled);
        });
    }
    void microphonePendingCorrelatedReadinessAndPcm() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            QVERIFY(media(t, 1000).isEmpty()); // No early success.
            QVERIFY(!t.m_microphoneReady); QVERIFY(t.m_microphoneDeadline.isActive());
            const auto policy = t.m_microphonePolicy;
            bool enabled = false;
            for (const auto &record : workerRecords(worker))
                if (auto p = ConsoleWorkerWire::microphonePolicy(record)) { QCOMPARE(*p, policy); enabled = true; }
            QVERIFY(enabled);
            QVERIFY(t.microphoneResult({policy.generation + 1, policy.requestId, {}}, 1000).isEmpty());
            QVERIFY(t.microphoneResult({policy.generation, policy.requestId + 1, {}}, 1000).isEmpty());
            QVERIFY(!t.forwardMicrophone(QByteArray(3840, 'a'), 1000));
            const auto ack = t.microphoneResult({policy.generation, policy.requestId, {}}, 1000);
            QVERIFY(ok(ack)); QCOMPARE(state(ack), u"on"_s); QCOMPARE(ack.value(u"device"_s).toString(), u"microphone"_s);
            QVERIFY(t.m_playback); QVERIFY(t.m_silenceHost);
            QVERIFY(!t.m_microphoneDeadline.isActive()); QCOMPARE(t.m_microphonePump.interval(), 20);
            QVERIFY(t.m_microphonePump.isActive());
            t.m_microphonePump.stop(); // Fixture has no PAM; timer correctly refuses that identity.
            QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).isEmpty());
            QVERIFY(!t.forwardMicrophone(QByteArray(3844, 'a'), 1000));
            QVERIFY(!t.forwardMicrophone(QByteArray(3, 'a'), 1000));
            QVERIFY(!t.forwardMicrophone(QByteArray(3840, 'a'), 1001));
            const QByteArray pcm(3840, 'a'); QVERIFY(t.forwardMicrophone(pcm, 1000));
            bool audio = false;
            for (const auto &record : workerRecords(worker)) if (auto a = ConsoleWorkerWire::microphoneAudio(record)) {
                QCOMPARE(a->generation, policy.generation); QCOMPARE(a->requestId, policy.requestId); QCOMPARE(a->pcm, pcm); audio = true;
            }
            QVERIFY(audio);
            const auto off = media(t, 1000, false); QVERIFY(ok(off));
            QCOMPARE(state(off), u"off"_s); QVERIFY(!t.m_microphoneReady);
            QVERIFY(!t.m_microphonePump.isActive());
            // No AUDIN producer is injected here. Queue generation/expiry/clearing
            // are exercised separately by RdpAudioPriorityTest, not this empty queue.
            QVERIFY(t.m_connection->takeExternalMicrophone().isEmpty());
            QVERIFY(!t.forwardMicrophone(pcm, 1000));
            QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).isEmpty());
            bool disabled = false;
            for (const auto &record : workerRecords(worker)) if (auto p = ConsoleWorkerWire::microphonePolicy(record)) {
                QVERIFY(!p->enabled); QCOMPARE(p->generation, policy.generation); QVERIFY(p->requestId > policy.requestId); disabled = true;
            }
            QVERIFY(disabled);
            QVERIFY(media(t, 1000).isEmpty()); const auto next = t.m_microphonePolicy;
            QCOMPARE(next.generation, policy.generation); QVERIFY(next.requestId > policy.requestId);
            QVERIFY(t.microphoneResult({policy.generation, policy.requestId, u"late old failure"_s}, 1000).isEmpty());
            QCOMPARE(t.m_microphonePolicy, next); QVERIFY(t.m_microphoneDeadline.isActive());
        });
    }
    void microphoneFailuresPreservePlaybackAndSilence() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &) {
            for (bool timeout : {false, true}) {
                QVERIFY(media(t, 1000).isEmpty()); const auto policy = t.m_microphonePolicy;
                const auto reply = timeout ? t.microphoneTimeout()
                    : t.microphoneResult({policy.generation, policy.requestId, u"source failed"_s}, 1000);
                QVERIFY(!ok(reply)); QCOMPARE(state(reply), u"error"_s);
                QCOMPARE(reply.value(u"code"_s).toString(), timeout ? u"timeout"_s : u"unavailable"_s);
                QVERIFY(t.m_playback); QVERIFY(t.m_silenceHost);
                QVERIFY(!t.m_microphonePolicy.enabled); QVERIFY(!t.m_microphoneDeadline.isActive());
                QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).isEmpty());
            }
            t.m_externalMicrophone = false;
            QVERIFY(!ok(media(t, 1000))); QVERIFY(t.m_playback); QVERIFY(t.m_silenceHost);
            t.m_externalMicrophone = true;
            QVERIFY(!ok(media(t, 1000, true, true, true)));
        });
    }
    void microphoneDetachRebindAndOverflow() {
        microphoneFixture([&](auto &t, auto &control, auto &endpoint, auto &worker) {
            const auto handle = *t.m_handle;
            QVERIFY(media(t, 1000).isEmpty()); const auto old = t.m_microphonePolicy;
            control.disconnected(1);
            QVERIFY(!t.m_endpoint); QVERIFY(!t.m_handle); QVERIFY(!t.m_microphonePolicy.enabled);
            QVERIFY(!t.m_microphoneDeadline.isActive()); QVERIFY(!t.m_microphonePump.isActive());
            bool disabled = false;
            for (const auto &record : workerRecords(worker))
                if (auto p = ConsoleWorkerWire::microphonePolicy(record); p && !p->enabled) disabled = true;
            QVERIFY(disabled);
            QVERIFY(control.request(1000, 1, {{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1},
                {u"id"_s, u"reattach"_s}, {u"action"_s, u"attach"_s}, {u"session"_s, handle.id}}).value(u"ok"_s).toBool());
            QVERIFY(!t.activateBinding(handle, &endpoint)); QVERIFY(t.authorized(1000));
            QVERIFY(media(t, 1000).isEmpty());
            QVERIFY(t.m_microphonePolicy.generation != old.generation);
            QVERIFY(t.microphoneResult({old.generation, old.requestId, {}}, 1000).isEmpty()); QVERIFY(!t.m_microphoneReady);
            t.stopMicrophone();
            t.m_nextMicrophoneId = std::numeric_limits<quint64>::max() - 2;
            QVERIFY(media(t, 1000).isEmpty());
            QCOMPARE(t.m_microphonePolicy.requestId, std::numeric_limits<quint64>::max() - 1);
            QVERIFY(ok(media(t, 1000, false)));
            QCOMPARE(t.m_nextMicrophoneId, std::numeric_limits<quint64>::max());
            QVERIFY(!ok(media(t, 1000)));
            QCOMPARE(t.m_nextMicrophoneId, std::numeric_limits<quint64>::max());
        });
    }
    void microphoneRechecksTargetAndProductionIdentity() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &) {
            QVERIFY(!ok(mediaAsConnection(t)));
            QVERIFY(!ok(media(t, 1001)));
            const auto handle = *t.m_handle;
            t.m_handle->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QVERIFY(!t.authorized(1000)); t.m_handle = handle;
            QVERIFY(media(t, 1000).isEmpty()); const auto p = t.m_microphonePolicy;
            const auto refused = t.microphoneResult({p.generation, p.requestId, {}}, std::nullopt);
            QVERIFY(!ok(refused)); QCOMPARE(refused.value(u"code"_s).toString(), u"detached"_s); QVERIFY(!t.m_microphoneReady);
            QVERIFY(media(t, 1000).isEmpty()); const auto active = t.m_microphonePolicy;
            QVERIFY(ok(t.microphoneResult({active.generation, active.requestId, {}}, 1000)));
            t.pumpMicrophone(); // Production PAM-only identity refuses fixture identity.
            QVERIFY(!t.m_microphoneReady); QVERIFY(!t.m_microphonePump.isActive());
        });
    }
    void microphoneRejectsPhysicalAndWrongSessionEndpoint() {
        microphoneFixture([&](auto &t, auto &, auto &original, auto &) {
            for (bool physical : {true, false}) {
                QTemporaryDir dir; ConsoleWorkerEndpoint other; QLocalSocket worker;
                const auto session = physical ? t.m_handle->id : QUuid::createUuid().toString(QUuid::WithoutBraces);
                const QByteArray token(32, 'x');
                QVERIFY(other.listen(dir.filePath(u"other.sock"_s),
                    {physical ? ConsoleSeat::Adapter::PhysicalUser : ConsoleSeat::Adapter::VirtualUser, session, 1000}, token));
                worker.connectToServer(other.socketName()); QVERIFY(worker.waitForConnected(1000));
                worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{session, 1000, token}));
                worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
                QVERIFY(worker.waitForBytesWritten(1000)); QTRY_VERIFY(other.ready());
                t.m_endpoint = &other;
                QVERIFY(!t.authorized(1000));
                QVERIFY(!ok(media(t, 1000)));
                QVERIFY(!t.m_microphonePolicy.enabled);
                t.m_endpoint = &original;
            }
        });
    }
    void microphoneRevokeRejectsReentrantConsent() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            QVERIFY(media(t, 1000).isEmpty()); const auto policy = t.m_microphonePolicy;
            int callbacks = 0;
            QObject::connect(t.m_connection->videoStream(), &VideoStream::enabledChanged, &t, [&] {
                if (t.m_connection->videoStream()->enabled()) return;
                ++callbacks;
                QVERIFY(t.m_revoking); QVERIFY(!t.m_microphonePolicy.enabled);
                QVERIFY(!t.m_microphonePump.isActive()); QVERIFY(!t.m_microphoneDeadline.isActive());
                QVERIFY(media(t, 1000).isEmpty());
                QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).isEmpty());
            });
            t.revoke(); QCOMPARE(callbacks, 1);
            bool sourceOff = false;
            for (const auto &record : workerRecords(worker)) {
                if (auto p = ConsoleWorkerWire::microphonePolicy(record); p && !p->enabled) sourceOff = true;
                if (record.kind == ConsoleWorkerWire::Kind::ControlState) {
                    const auto state = ConsoleWorkerWire::controlState(record); QVERIFY(state);
                    if (!state->active) QVERIFY(sourceOff); // Off dispatched before endpoint/grant teardown.
                }
            }
            QVERIFY(sourceOff); QVERIFY(!t.m_microphoneReady);
        });
    }
    void microphoneSocketLossPendingAndReady() {
        for (bool ready : {false, true}) microphoneFixture([&](auto &t, auto &control, auto &endpoint, auto &worker) {
            QVERIFY(media(t, 1000).isEmpty()); const auto policy = t.m_microphonePolicy;
            if (ready) QVERIFY(ok(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000)));
            worker.abort();
            QTRY_VERIFY(!endpoint.ready());
            QVERIFY(!control.attachment(1)); QVERIFY(!t.m_endpoint); QVERIFY(!t.m_handle);
            QVERIFY(!t.m_microphoneReady); QVERIFY(!t.m_microphonePolicy.enabled);
            QVERIFY(!t.m_microphoneDeadline.isActive()); QVERIFY(!t.m_microphonePump.isActive());
            QVERIFY(!t.m_playback); QVERIFY(!t.m_silenceHost);
            QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).isEmpty());
        });
    }
    void microphoneActiveRevokeMayDestroyTransport() {
        microphoneFixture([&](auto &t, auto &control, auto &, auto &worker) {
            QVERIFY(media(t, 1000).isEmpty()); const auto policy = t.m_microphonePolicy;
            QVERIFY(ok(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000)));
            QVERIFY(t.m_microphoneReady); QVERIFY(t.m_microphonePump.isActive());
            const auto connection = t.m_connection;
            QPointer<VirtualSessionTransport> alive(&t);
            QObject::connect(connection->videoStream(), &VideoStream::enabledChanged, connection, [alive] {
                if (alive && !alive->m_connection->videoStream()->enabled()) delete alive.data();
            });
            t.closed();
            QVERIFY(!alive); QVERIFY(!control.attachment(1));
            bool off = false;
            for (const auto &record : workerRecords(worker))
                if (auto p = ConsoleWorkerWire::microphonePolicy(record); p && !p->enabled) off = true;
            QVERIFY(off);
        });
    }
    void closedRejectsReentrantReplacementAttachment()
    {
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            return VirtualSessionSupervisor::Launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        });
        VirtualSessionControl control(supervisor, {});
        const auto first = supervisor.create(1000), second = supervisor.create(1000); QVERIFY(first); QVERIFY(second);
        bool readyFirst = false, readySecond = false;
        QTRY_VERIFY(readyFirst || (readyFirst = supervisor.captureReady(*first)));
        QTRY_VERIFY(readySecond || (readySecond = supervisor.captureReady(*second)));
        const auto attach = [](const QString &id, const QString &session) {
            return QJsonObject{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, id},
                {u"action"_s, u"attach"_s}, {u"session"_s, session}};
        };
        QVERIFY(control.request(1000, 1, attach(u"first"_s, first->id)).value(u"ok"_s).toBool());
        Server server; RdpConnection connection(&server, -1); quint64 sequence = 0;
        VirtualSessionTransport transport(1, &connection, control, {}, sequence);
        connection.videoStream()->setEnabled(true);
        int attempts = 0;
        QObject::connect(connection.videoStream(), &VideoStream::enabledChanged, &connection, [&] {
            if (connection.videoStream()->enabled()) return;
            ++attempts;
            QVERIFY(control.dispatchActive());
            QVERIFY(!control.attachment(1)); // Original lookup removed before revocation.
            control.disconnected(1);
            QVERIFY(!control.request(1000, 1, attach(u"nested"_s, second->id)).value(u"ok"_s).toBool());
        });
        transport.closed();
        QCOMPARE(attempts, 1); QVERIFY(!control.dispatchActive()); QVERIFY(!control.attachment(1));
        for (const auto &row : supervisor.list(1000)) QCOMPARE(row.phase, VirtualSessionState::Phase::Retained);
        // The replacement remains available for a later, nonnested command.
        QVERIFY(control.request(1000, 1, attach(u"later"_s, second->id)).value(u"ok"_s).toBool());
        QVERIFY(transport.attachmentMatches(*second));
    }
    void bindingRejectsCallbackAttachmentChanges_data()
    {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<bool>("reassign");
        for (const auto *stage : {"resolver", "revoke", "stream-active", "enabled"}) {
            QTest::newRow(qPrintable(QString::fromLatin1(stage) + u"-detach"_s)) << QString::fromLatin1(stage) << false;
            QTest::newRow(qPrintable(QString::fromLatin1(stage) + u"-reassign"_s)) << QString::fromLatin1(stage) << true;
        }
    }
    void bindingRejectsCallbackAttachmentChanges()
    {
        QFETCH(QString, stage); QFETCH(bool, reassign);
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            return VirtualSessionSupervisor::Launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        });
        QPointer<VirtualSessionTransport> transport;
        VirtualSessionControl control(supervisor, [&](quint64, const auto &) { if (transport) transport->revoke(); });
        const auto first = supervisor.create(1000), second = supervisor.create(1000); QVERIFY(first); QVERIFY(second);
        bool readyFirst = false, readySecond = false;
        QTRY_VERIFY(readyFirst || (readyFirst = supervisor.captureReady(*first)));
        QTRY_VERIFY(readySecond || (readySecond = supervisor.captureReady(*second)));
        const auto attach = [](const QString &id, const QString &session) {
            return QJsonObject{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, id},
                {u"action"_s, u"attach"_s}, {u"session"_s, session}};
        };
        QVERIFY(control.request(1000, 1, attach(u"first"_s, first->id)).value(u"ok"_s).toBool());
        QTemporaryDir dir; ConsoleWorkerEndpoint endpoint; QLocalSocket worker;
        const QByteArray token(32, 't');
        QVERIFY(endpoint.listen(dir.filePath(u"worker.sock"_s), {ConsoleSeat::Adapter::VirtualUser, first->id, 1000}, token));
        worker.connectToServer(endpoint.socketName()); QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{first->id, 1000, token}));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000)); QTRY_VERIFY(endpoint.ready());
        int changes = 0;
        const auto changeAttachment = [&] {
            ++changes;
            control.disconnected(1);
            if (reassign) QVERIFY(control.request(1000, 1, attach(u"second"_s, second->id)).value(u"ok"_s).toBool());
        };
        Server server; RdpConnection connection(&server, -1); quint64 sequence = 0;
        transport = new VirtualSessionTransport(1, &connection, control, [&](const auto &) -> ConsoleWorkerEndpoint * {
            changeAttachment(); return &endpoint;
        }, sequence);
        QVERIFY(transport->attachmentMatches(*first));
        auto wrong = *first; ++wrong.generation; QVERIFY(!transport->attachmentMatches(wrong));
        wrong = *first; wrong.manager = QUuid::createUuid(); QVERIFY(!transport->attachmentMatches(wrong));
        if (stage == u"resolver"_s) {
            QVERIFY(!transport->bind());
            QCOMPARE(sequence, quint64(0)); // Never reached the worker grant.
        } else {
            if (stage == u"revoke"_s) {
                connection.videoStream()->setEnabled(true);
                QObject::connect(connection.videoStream(), &VideoStream::enabledChanged, &connection,
                    [&] { if (!connection.videoStream()->enabled()) changeAttachment(); });
            } else if (stage == u"stream-active"_s) {
                QObject::connect(&transport->m_session, &AbstractSession::streamActiveChanged, &connection,
                    [&](bool active) { if (active) changeAttachment(); });
            } else {
                QObject::connect(connection.videoStream(), &VideoStream::enabledChanged, &connection,
                    [&] { if (connection.videoStream()->enabled()) changeAttachment(); });
            }
            // Exercise the post-validation callback continuation directly. The
            // RDP connection remains unauthenticated; no successful bind is faked.
            QVERIFY(!transport->activateBinding(*first, &endpoint));
            QCOMPARE(sequence, stage == u"revoke"_s ? quint64(0) : quint64(2)); // No grant, or grant then revoke.
        }
        QCOMPARE(changes, 1); QVERIFY(transport); QVERIFY(!transport->m_handle); QVERIFY(!transport->m_endpoint);
        QVERIFY(!connection.videoStream()->enabled());
        if (reassign) {
            QVERIFY(transport->attachmentMatches(*second));
            QVERIFY(!transport->attachmentMatches(*first));
        } else QVERIFY(!control.attachment(1));
        for (const auto &row : supervisor.list(1000)) {
            if (row.id == first->id) QCOMPARE(row.phase, VirtualSessionState::Phase::Retained);
        }
        delete transport.data();
    }
    void revokeSignalMayDestroyTransport()
    {
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            return VirtualSessionSupervisor::Launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        });
        VirtualSessionControl control(supervisor, {});
        const auto handle = supervisor.create(1000); QVERIFY(handle);
        bool ready = false;
        QTRY_VERIFY(ready || (ready = supervisor.captureReady(*handle)));
        const QJsonObject attach{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"attach"_s},
            {u"action"_s, u"attach"_s}, {u"session"_s, handle->id}};
        QVERIFY(control.request(1000, 1, attach).value(u"ok"_s).toBool());
        QVERIFY(control.attachment(1));
        QCOMPARE(supervisor.list(1000).first().phase, VirtualSessionState::Phase::Attached);
        Server server; RdpConnection connection(&server, -1); quint64 sequence = 0;
        QPointer<VirtualSessionTransport> transport = new VirtualSessionTransport(1, &connection, control, {}, sequence);
        connection.videoStream()->setEnabled(true);
        QObject::connect(connection.videoStream(), &VideoStream::enabledChanged, &connection, [&] {
            delete transport.data();
        });
        transport->unavailable();
        QVERIFY(transport.isNull());
        QCOMPARE(sequence, quint64(0));
        QVERIFY(!control.attachment(1));
        QCOMPARE(supervisor.list(1000).first().phase, VirtualSessionState::Phase::Retained);
    }
    void controlDestructionDuringRequest()
    {
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> { return {}; });
        auto control = std::make_unique<VirtualSessionControl>(supervisor, VirtualSessionControl::Release{});
        Server server; RdpConnection connection(&server, -1); quint64 sequence = 0;
        VirtualSessionTransport transport(1, &connection, *control, {}, sequence);
        int calls = 0;
        control->setCreateHandler([&](quint32) -> std::optional<VirtualSessionRegistry::Handle> {
            ++calls; control.reset(); return {};
        });
        const QJsonObject command{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"delete"_s}, {u"action"_s, u"create"_s}};
        QVERIFY(transport.request(command, 1000).isEmpty()); QCOMPARE(calls, 1);
        QVERIFY(transport.request(command, 1000).isEmpty());
        transport.closed(); // Surviving transport must not dereference dead Control.
    }
    void destructionDuringBindOrDisconnect_data()
    {
        QTest::addColumn<bool>("duringBind");
        QTest::newRow("resolver-deletes-transport") << true;
        QTest::newRow("unavailable-release-deletes-transport") << false;
    }
    void destructionDuringBindOrDisconnect()
    {
        QFETCH(bool, duringBind);
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            return VirtualSessionSupervisor::Launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        });
        QPointer<VirtualSessionTransport> transport;
        int releases = 0, resolves = 0;
        VirtualSessionControl control(supervisor, [&](quint64, const auto &) {
            ++releases;
            if (!duringBind) delete transport.data();
        });
        const auto handle = supervisor.create(1000); QVERIFY(handle);
        bool ready = false;
        QTRY_VERIFY(ready || (ready = supervisor.captureReady(*handle)));
        Server server; RdpConnection connection(&server, -1); quint64 sequence = 0;
        transport = new VirtualSessionTransport(1, &connection, control, [&](const auto &) -> ConsoleWorkerEndpoint * {
            ++resolves; delete transport.data(); return nullptr;
        }, sequence);
        const QJsonObject attach{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"attach"_s},
            {u"action"_s, u"attach"_s}, {u"session"_s, handle->id}};
        if (duringBind) QVERIFY(transport->request(attach, 1000).isEmpty());
        else {
            QVERIFY(control.request(1000, 1, attach).value(u"ok"_s).toBool());
            transport->unavailable();
        }
        QVERIFY(transport.isNull()); QCOMPARE(releases, 1);
        QCOMPARE(resolves, duringBind ? 1 : 0); QCOMPARE(sequence, quint64(0));
        QVERIFY(!control.attachment(1));
    }
    void destroyedDuringControlRequest_data()
    {
        QTest::addColumn<bool>("delivery");
        QTest::addColumn<bool>("deleteConnection");
        QTest::newRow("request-deletes-transport") << false << false;
        QTest::newRow("delivery-deletes-transport") << true << false;
        QTest::newRow("request-deletes-connection") << false << true;
        QTest::newRow("delivery-deletes-connection") << true << true;
    }
    void destroyedDuringControlRequest()
    {
        QFETCH(bool, delivery); QFETCH(bool, deleteConnection);
        QTest::failOnWarning(QRegularExpression(u"KRDPCTL: dropping.*"_s)); // No reply after destruction.
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> { return {}; });
        VirtualSessionControl control(supervisor, {});
        Server server;
        QPointer<RdpConnection> connection = new RdpConnection(&server, -1);
        quint64 sequence = 0;
        int resolved = 0, called = 0;
        QPointer<VirtualSessionTransport> transport = new VirtualSessionTransport(1, connection, control,
            [&](const auto &) -> ConsoleWorkerEndpoint * { ++resolved; return nullptr; }, sequence);
        control.setCreateHandler([&](quint32 uid) -> std::optional<VirtualSessionRegistry::Handle> {
            ++called;
            if (uid != 1000) return {};
            if (deleteConnection) delete connection.data();
            else delete transport.data();
            return {};
        });
        const QJsonObject command{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"delete"_s}, {u"action"_s, u"create"_s}};
        // Exercise the exact production dispatch/delivery path, supplying the
        // authenticated identity at its private seam instead of opening PAM/RDP.
        if (delivery) transport->deliverControlRecord(command, 1000);
        else QVERIFY(transport->request(command, 1000).isEmpty());
        QCOMPARE(called, 1); QCOMPARE(resolved, 0); QCOMPARE(sequence, quint64(0));
        if (deleteConnection) QVERIFY(connection.isNull());
        else QVERIFY(transport.isNull());
        delete transport.data(); delete connection.data();
    }
    void externalPlaybackRevocationDropsBufferedAudio()
    {
        ExternalAudioQueue queue;
        int sent = 0;
        const auto sink = [&](const QByteArray &pcm) { sent += pcm.size(); };
        queue.submit(QByteArray(3528, 'a'));
        QVERIFY(!queue.deliver(sink));
        queue.setEnabled(true);
        queue.submit(QByteArray(7056, 'b'));
        QVERIFY(queue.deliver(sink));
        QCOMPARE(sent, 3528);
        queue.setEnabled(false);
        queue.submit(QByteArray(3528, 'c'));
        QVERIFY(!queue.deliver(sink));
        queue.setEnabled(true);
        QVERIFY(!queue.deliver(sink)); // old queued bytes cannot survive off/on
        queue.submit(QByteArray(3, 'd'));
        QVERIFY(!queue.deliver(sink));
        queue.submit(QByteArray(4, 'e'));
        QVERIFY(queue.deliver(sink));
        QCOMPARE(sent, 3532);
    }
    void refusedUpdateClearsPreviousPlayback()
    {
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> { return {}; });
        VirtualSessionControl control(supervisor, {});
        Server server;
        RdpConnection connection(&server, -1);
        quint64 sequence = 0;
        VirtualSessionTransport transport(1, &connection, control, {}, sequence);
        // Simulate an earlier consent followed by loss of authorization. No
        // real RDP socket, PipeWire graph or desktop is used by this fixture.
        transport.m_playback = true;
        const auto result = transport.request(device(u"microphone"_s, u"on"_s));
        QCOMPARE(result.value(u"type"_s).toString(), u"error"_s);
        QCOMPARE(result.value(u"code"_s).toString(), u"not-owner"_s);
        QVERIFY(!transport.m_playback);
        QCOMPARE(sequence, quint64(0));
    }
    // --- AUD-D4: stock RDP clients (no `virtual-session` record) ---
private:
    struct Stock {
        VirtualSessionSupervisor *supervisor = nullptr;
        VirtualSessionControl *control = nullptr;
        std::function<VirtualSessionControl::CreateResult(quint32)> create; // unset: supervisor.create
        QList<VirtualSessionControl::InitialOutputs> created;
        QList<VirtualSessionRegistry::Handle> handles; // what supervisor.create returned for them
        QMap<quint64, QList<quint32>> refused; // per client: the ERRINFO codes it was closed with
        QList<quint64> displaced;
        ClientDisplay::Info display{QSize(1366, 768), {}};
        VirtualSessionTransport::StockPolicy policy;
        std::function<VirtualSessionRegistry::Handle(const VirtualSessionRegistry::Handle &)> ready;
        std::function<VirtualSessionTransport *(quint64)> connect;
        std::map<QString, QLocalSocket *> workers; // per desktop id
        bool failLaunch = false;
    };
    void stockFixture(const std::function<void(Stock &)> &test)
    {
        m_workerDeframer = {};
        Stock stock;
        // failLaunch: the desktop's process exits at once, so it is Failed, never ready.
        VirtualSessionSupervisor supervisor([&stock](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            if (stock.failLaunch) return VirtualSessionSupervisor::Launch{u"/usr/bin/false"_s, {}, {}, {}};
            return VirtualSessionSupervisor::Launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        });
        std::map<quint64, QPointer<VirtualSessionTransport>> transports;
        VirtualSessionControl control(supervisor, [&](quint64 client, const auto &) {
            if (auto transport = transports[client]) transport->revoke();
        });
        control.setDisplacedHandler([&](quint64 client) {
            stock.displaced.append(client);
            if (auto transport = transports[client]) transport->displaced();
        });
        control.setSelectedCreateHandler([&](quint32 uid, const auto &outputs) {
            stock.created.append(outputs);
            if (stock.create) return stock.create(uid);
            const auto handle = supervisor.create(uid);
            if (handle) stock.handles.append(*handle);
            return VirtualSessionControl::CreateResult(handle);
        });
        control.setInitialLayoutPreviewCapabilities({.maxOutputs = 16, .maxOutputDimension = 4096, .maxAtlasDimension = 8192});
        QTemporaryDir dir;
        std::vector<std::unique_ptr<ConsoleWorkerEndpoint>> endpoints;
        std::vector<std::unique_ptr<QLocalSocket>> workers;
        Server server;
        std::vector<std::unique_ptr<RdpConnection>> connections;
        quint64 sequence = 0;
        stock.supervisor = &supervisor;
        stock.control = &control;
        // A ready desktop: its capture is proven and an authenticated worker is connected.
        // The worker endpoint first, as in the host, where readiness is proven by its frames.
        // (Waits are checked by the tests themselves: attachment, authorized().)
        stock.ready = [&](const VirtualSessionRegistry::Handle &handle) {
            auto endpoint = std::make_unique<ConsoleWorkerEndpoint>();
            const QByteArray token(32, 't');
            endpoint->listen(dir.filePath(handle.id.left(8) + u".sock"_s), {ConsoleSeat::Adapter::VirtualUser, handle.id, 1000}, token);
            auto worker = std::make_unique<QLocalSocket>();
            worker->connectToServer(endpoint->socketName());
            worker->waitForConnected(1000);
            worker->write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{handle.id, 1000, token}));
            worker->write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
            worker->waitForBytesWritten(1000);
            (void)QTest::qWaitFor([&] { return endpoint->ready(); }, 5000);
            stock.workers[handle.id] = worker.get();
            endpoints.push_back(std::move(endpoint));
            workers.push_back(std::move(worker));
            (void)QTest::qWaitFor([&] { return supervisor.captureReady(handle); }, 5000);
            return handle;
        };
        stock.connect = [&](quint64 client) {
            auto connection = std::make_unique<RdpConnection>(&server, -1);
            // No socket/PAM/capture: remove only this fixture's queued initialization.
            QCoreApplication::removePostedEvents(connection.get(), QEvent::MetaCall);
            auto *transport = new VirtualSessionTransport(client, connection.get(), control,
                [&endpoints](const auto &handle) -> ConsoleWorkerEndpoint * {
                    for (const auto &endpoint : endpoints)
                        if (endpoint->target().sessionId == handle.id) return endpoint.get();
                    return nullptr;
                }, sequence, connection.get());
            transport->m_clientDisplayInfo = [&stock] { return stock.display; };
            transport->m_refused = [&stock, client](quint32 code) { stock.refused[client].append(code); };
            transport->m_stockGate.setInterval(100);
            transport->m_stockWait.setInterval(10);
            if (stock.policy) transport->setStockClientPolicy(stock.policy);
            transports[client] = transport;
            connections.push_back(std::move(connection));
            return transport;
        };
        test(stock);
        connections.clear();
    }
    static QJsonObject sessionCommand(const QString &id, const QString &action, const QString &session = {})
    {
        QJsonObject record{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, id}, {u"action"_s, action}};
        if (!session.isEmpty()) record.insert(u"session"_s, session);
        return record;
    }
    void stockPaths_data()
    {
        QTest::addColumn<bool>("controlChannel");
        QTest::newRow("no KRDPCTL") << false;
        QTest::newRow("KRDPCTL without virtual-session") << true;
    }
    // A client with KRDPCTL waits for the 3 s gate (here 100 ms); without it the broker picks at once.
    static void startStock(VirtualSessionTransport &t, bool controlChannel)
    {
        t.startStockGate(controlChannel, 1000);
        if (controlChannel) {
            QVERIFY(t.m_stockGate.isActive());
            QVERIFY(!t.m_control->attachment(t.m_client));
        }
    }

private Q_SLOTS:
    void stockClientAttachesMostRecentDesktop_data() { stockPaths_data(); }
    void stockClientAttachesMostRecentDesktop()
    {
        QFETCH(bool, controlChannel);
        stockFixture([&](Stock &stock) {
            const auto older = stock.ready(*stock.supervisor->create(1000));
            const auto newer = stock.ready(*stock.supervisor->create(1000));
            QVERIFY(stock.supervisor->attach(1000, older.id, 99) && stock.supervisor->disconnect(older, 99)); // both retained
            // Used order: `newer` first, then `older` (whatever the ids sort as).
            stock.control->noteUsed(newer.id);
            stock.control->noteUsed(older.id);
            auto *t = stock.connect(1);
            startStock(*t, controlChannel);
            QTRY_VERIFY(stock.control->attachment(1));
            QCOMPARE(stock.control->attachment(1)->id, older.id);
            QVERIFY(t->m_handle && t->m_handle->id == older.id);
            QVERIFY(t->authorized(1000)); // bound: frames and input flow
            QVERIFY(stock.created.isEmpty());
            QVERIFY(stock.refused.isEmpty());

            // A second stock connection of the same user takes that desktop over,
            // like the owner's own `stop` releases it; the first is told with
            // ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION.
            auto *second = stock.connect(2);
            startStock(*second, controlChannel);
            QTRY_VERIFY(stock.control->attachment(2));
            QCOMPARE(stock.control->attachment(2)->id, older.id);
            QVERIFY(!stock.control->attachment(1));
            QVERIFY(!t->m_handle);
            QCOMPARE(stock.displaced, QList<quint64>{1});
            QTRY_COMPARE(stock.refused.value(1), QList<quint32>{quint32(ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION)});
            QVERIFY(!stock.refused.contains(2));
            QVERIFY(stock.created.isEmpty());
        });
    }

    void stockClientCreatesDesktopFromStandardMonitorData_data() { stockPaths_data(); }
    void stockClientCreatesDesktopFromStandardMonitorData()
    {
        QFETCH(bool, controlChannel);
        stockFixture([&](Stock &stock) {
            // A failed desktop is not "existing": it is never attached.
            stock.failLaunch = true;
            const auto failed = stock.supervisor->create(1000);
            QVERIFY(failed);
            QTRY_COMPARE(stock.supervisor->list(1000).first().phase, VirtualSessionState::Phase::Failed);
            stock.failLaunch = false;
            stock.display = {QSize(1366, 768), {}}; // TS_UD_CS_CORE only
            auto *t = stock.connect(1);
            startStock(*t, controlChannel);
            QTRY_COMPARE(stock.created.size(), 1);
            const VirtualSessionControl::InitialOutputs expected{{QPoint(0, 0), QSize(1366, 768), 1.0, true}};
            QCOMPARE(stock.created.first(), expected);
            QVERIFY(t->m_stockWait.isActive()); // starting: attached once it is ready
            QVERIFY(!stock.control->attachment(1));
            // The client's window changed meanwhile (MS-RDPEDISP): applied once bound.
            t->displayLayout({{QRect(0, 0, 1600, 900), true}}, 1000);
            QVERIFY(t->m_displaySize);
            QCOMPARE(stock.handles.size(), 1);
            const auto created = stock.ready(stock.handles.first());
            QTRY_VERIFY(stock.control->attachment(1));
            QCOMPARE(stock.control->attachment(1)->id, created.id);
            QVERIFY(t->m_handle && t->m_handle->id == created.id);
            QVERIFY(t->authorized(1000));
            QVERIFY(!t->m_stockWait.isActive());
            QVERIFY(stock.refused.isEmpty());
            std::optional<ConsoleWorkerWire::Resize> resize;
            QTRY_VERIFY([&] {
                for (const auto &record : workerRecords(*stock.workers[created.id]))
                    if (const auto parsed = ConsoleWorkerWire::resize(record)) resize = parsed;
                return resize.has_value();
            }());
            QCOMPARE(resize->pixels, QSize(1600, 900));
            QCOMPARE(resize->scale, 1.0);
        });
    }

    void stockClientGetsSeveralOutputsFromMcsMonitorData()
    {
        stockFixture([&](Stock &stock) {
            // TS_UD_CS_MONITOR: two monitors, the primary on the right.
            stock.display = {QSize(3840, 1080), {{QRect(-1920, 0, 1920, 1080), false}, {QRect(0, 0, 1920, 1080), true}}};
            startStock(*stock.connect(1), false);
            QCOMPARE(stock.created.size(), 1);
            QCOMPARE(stock.created.first().size(), 2);
            QVERIFY(stock.created.first()[0].primary);
            QCOMPARE(stock.created.first()[0].position, QPoint(1920, 0));
            QCOMPARE(stock.created.first()[1].position, QPoint(0, 0));
        });
    }

    void stockClientRefusedWithStandardCode_data()
    {
        QTest::addColumn<bool>("controlChannel");
        QTest::addColumn<QString>("failure");
        QTest::addColumn<quint32>("code");
        for (const bool channel : {false, true}) {
            const char *path = channel ? "KRDPCTL without virtual-session" : "no KRDPCTL";
            QTest::addRow("%s, slot limit", path) << channel << u"limit"_s << quint32(ERRINFO_CB_DESTINATION_POOL_NOT_FREE);
            QTest::addRow("%s, create failed", path) << channel << u"failed"_s << quint32(ERRINFO_CB_SESSION_ONLINE_VM_SESSMON_FAILED);
            QTest::addRow("%s, desktop failed while starting", path) << channel << u"start-failed"_s << quint32(ERRINFO_CB_SESSION_ONLINE_VM_SESSMON_FAILED);
            QTest::addRow("%s, desktop never ready", path) << channel << u"timeout"_s << quint32(ERRINFO_CB_SESSION_ONLINE_VM_BOOT_TIMEOUT);
        }
    }
    void stockClientRefusedWithStandardCode()
    {
        QFETCH(bool, controlChannel);
        QFETCH(QString, failure);
        QFETCH(quint32, code);
        stockFixture([&](Stock &stock) {
            if (failure == u"limit"_s) {
                stock.create = [](quint32) { return VirtualSessionControl::CreateResult(VirtualSessionControl::CreateResult::Refusal::Limit); };
            } else if (failure == u"failed"_s) {
                stock.create = [](quint32) { return VirtualSessionControl::CreateResult(); };
            }
            auto *t = stock.connect(1);
            t->m_stockWaitLimitMs = failure == u"timeout"_s ? 300 : 20000;
            stock.failLaunch = failure == u"start-failed"_s;
            startStock(*t, controlChannel);
            QTRY_COMPARE(stock.refused.value(1), QList<quint32>{code});
            QVERIFY(!stock.control->attachment(1));
            QVERIFY(!t->m_handle);
            QVERIFY(!t->m_stockWait.isActive() && !t->m_stockGate.isActive());
            QCOMPARE(stock.created.size(), 1); // one attempt, never a retry loop
        });
    }

    void stockClientRefusedByPolicy_data() { stockPaths_data(); }
    void stockClientRefusedByPolicy()
    {
        QFETCH(bool, controlChannel);
        stockFixture([&](Stock &stock) {
            const auto existing = stock.ready(*stock.supervisor->create(1000));
            QList<quint32> asked;
            stock.policy = [&asked](quint32 uid) {
                asked.append(uid);
                return VirtualStockClient::Policy::Refuse;
            };
            auto *t = stock.connect(1);
            startStock(*t, controlChannel);
            QTRY_COMPARE(stock.refused.value(1), QList<quint32>{quint32(ERRINFO_SERVER_DENIED_CONNECTION)});
            QCOMPARE(asked, QList<quint32>{1000}); // the authenticated uid's own policy
            QVERIFY(!stock.control->attachment(1));
            QVERIFY(stock.created.isEmpty());
            for (const auto &entry : stock.supervisor->list(1000))
                if (entry.id == existing.id) QCOMPARE(entry.phase, VirtualSessionState::Phase::Retained);
        });
    }

    void ownClientVirtualSessionRecordKeepsItsOwnChoice()
    {
        stockFixture([&](Stock &stock) {
            const auto desktop = stock.ready(*stock.supervisor->create(1000));
            stock.control->noteUsed(desktop.id);
            int policyCalls = 0;
            stock.policy = [&policyCalls](quint32) { ++policyCalls; return VirtualStockClient::Policy::Refuse; };
            auto *t = stock.connect(1);
            startStock(*t, true);
            // Our client lists first and may take its time choosing (a dialog).
            QVERIFY(t->request(sessionCommand(u"list-1"_s, u"list"_s), 1000).value(u"ok"_s).toBool());
            QVERIFY(!t->m_stockGate.isActive());
            QTest::qWait(300); // well past the gate
            QVERIFY(!stock.control->attachment(1));
            QVERIFY(stock.created.isEmpty());
            QVERIFY(stock.refused.isEmpty());
            QCOMPARE(policyCalls, 0);
            // Its explicit attach is the unchanged path (bound by the tests above).
            QVERIFY(stock.control->request(1000, 1, sessionCommand(u"attach-1"_s, u"attach"_s, desktop.id)).value(u"ok"_s).toBool());
            QCOMPARE(stock.control->attachment(1)->id, desktop.id);
            // A `virtual-session` record that arrives before authentication finished counts too.
            auto *early = stock.connect(2);
            QVERIFY(!early->request(sessionCommand(u"list-2"_s, u"list"_s), 1000).isEmpty());
            startStock(*early, false);
            QTest::qWait(150);
            QVERIFY(!stock.control->attachment(2));
            QVERIFY(stock.refused.isEmpty());
        });
    }

    // AUD-FIX2 F2: our client's first record on :3395 was `codec` (then unsupported here), so
    // the broker took it for a stock client and bound a desktop for it behind its back. A
    // client that speaks KRDPCTL v2 (a requestId, or any record this broker knows) is never
    // bound automatically; `codec` is answered (AVC); StandardClientMedia stays off.
    void krdpctlClientIsNeverBoundAutomatically()
    {
        stockFixture([&](Stock &stock) {
            const auto desktop = stock.ready(*stock.supervisor->create(1000));
            stock.control->noteUsed(desktop.id);
            int policyCalls = 0;
            stock.policy = [&policyCalls](quint32) { ++policyCalls; return VirtualStockClient::Policy::AttachOrCreate; };
            auto *t = stock.connect(1);
            startStock(*t, true);
            const auto answer = t->request(QJsonObject{{u"type"_s, u"codec"_s}, {u"v"_s, 1}, {u"codecs"_s, QJsonArray{u"hevc"_s}}}, 1000);
            QCOMPARE(answer.value(u"type"_s).toString(), u"codec"_s);
            QCOMPARE(answer.value(u"selected"_s).toString(), u"avc"_s);
            QVERIFY(!answer.contains(u"backend"_s));
            QVERIFY(t->m_krdpctlClient);
            QVERIFY(t->m_spokeKrdpctl);
            QTest::qWait(300); // well past the gate
            QVERIFY(!stock.control->attachment(1));
            QCOMPARE(policyCalls, 0);
            QVERIFY(stock.refused.isEmpty());
            QVERIFY(stock.created.isEmpty());

            // Even a type the broker does not know counts, when it carries a requestId.
            auto *other = stock.connect(2);
            startStock(*other, true);
            other->deliverControlRecord(QJsonObject{{u"type"_s, u"hello"_s}, {u"v"_s, 1}, {u"requestId"_s, u"r1"_s}}, 1000);
            QTest::qWait(300);
            QVERIFY(!stock.control->attachment(2));
            QCOMPARE(policyCalls, 0);
            // Its own explicit choice still works.
            const auto attached = stock.control->request(1000, 2, sessionCommand(u"attach-2"_s, u"attach"_s, desktop.id));
            QVERIFY2(attached.value(u"ok"_s).toBool(), qPrintable(QString::fromUtf8(QJsonDocument(attached).toJson(QJsonDocument::Compact))));
            QCOMPARE(stock.control->attachment(2)->id, desktop.id);
        });
    }

    // Unchanged for stock clients: KRDPCTL opened but nothing v2 in it (no requestId, unknown type).
    void channelWithoutV2RecordIsStillAStockClient()
    {
        stockFixture([&](Stock &stock) {
            const auto desktop = stock.ready(*stock.supervisor->create(1000));
            stock.control->noteUsed(desktop.id);
            auto *t = stock.connect(1);
            startStock(*t, true);
            t->deliverControlRecord(QJsonObject{{u"type"_s, u"hello"_s}, {u"v"_s, 1}}, 1000);
            QVERIFY(!t->m_krdpctlClient);
            QTRY_VERIFY(stock.control->attachment(1));
            QCOMPARE(stock.control->attachment(1)->id, desktop.id);
        });
    }

    // AUD-FIX2 F4: the same user opens the desktop on another device with our client: an
    // explicit attach takes it over, and the first connection hears why before 0x5.
    void ownClientTakeOverTellsTheOtherDevice()
    {
        stockFixture([&](Stock &stock) {
            const auto desktop = stock.ready(*stock.supervisor->create(1000));
            auto *first = stock.connect(1);
            QList<QJsonObject> pushed;
            first->m_recordPushed = [&pushed](const QJsonObject &record) { pushed.append(record); };
            first->m_capabilitiesSent = true; // it joined KRDPCTL
            // (Control level: this fixture's connections have no PAM identity to bind capture with.)
            const auto attached = stock.control->request(1000, 1, sessionCommand(u"attach-1"_s, u"attach"_s, desktop.id));
            QVERIFY2(attached.value(u"ok"_s).toBool(), qPrintable(QString::fromUtf8(QJsonDocument(attached).toJson(QJsonDocument::Compact))));
            QCOMPARE(stock.control->attachment(1)->id, desktop.id);

            stock.connect(2);
            const auto taken = stock.control->request(1000, 2, sessionCommand(u"attach-2"_s, u"attach"_s, desktop.id));
            QVERIFY2(taken.value(u"ok"_s).toBool(), qPrintable(QString::fromUtf8(QJsonDocument(taken).toJson(QJsonDocument::Compact))));
            QCOMPARE(stock.control->attachment(2)->id, desktop.id);
            QVERIFY(!stock.control->attachment(1));
            QCOMPARE(stock.displaced, QList<quint64>{1});
            QTRY_VERIFY(std::any_of(pushed.cbegin(), pushed.cend(), [](const QJsonObject &r) { return r.value(u"type"_s) == u"session-end"_s; }));
            const auto end = *std::find_if(pushed.cbegin(), pushed.cend(), [](const QJsonObject &r) { return r.value(u"type"_s) == u"session-end"_s; });
            QCOMPARE(end.value(u"reason"_s).toString(), u"opened-elsewhere"_s);
            QCOMPARE(end.value(u"errorInfo"_s).toInteger(), qint64(ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION));
            QVERIFY(!end.contains(u"requestId"_s));
            QVERIFY(stock.refused.value(1).isEmpty()); // the record goes out before the close
            QTRY_COMPARE(stock.refused.value(1), QList<quint32>{quint32(ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION)});
            QVERIFY(!stock.refused.contains(2));
        });
    }

    void displayControlResizesOneOutputDesktop()
    {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            workerRecords(worker);
            const auto sentResizes = [&] {
                QList<ConsoleWorkerWire::Resize> resizes;
                for (const auto &record : workerRecords(worker))
                    if (const auto parsed = ConsoleWorkerWire::resize(record)) resizes.append(*parsed);
                return resizes;
            };
            QList<QJsonObject> pushed;
            t.m_recordPushed = [&pushed](const QJsonObject &record) { pushed.append(record); };
            // Two monitors cannot drive a one-output desktop: ignored.
            t.displayLayout({{QRect(0, 0, 1920, 1080), true}, {QRect(1920, 0, 1920, 1080), false}}, 1000);
            QVERIFY(sentResizes().isEmpty());
            t.displayLayout({{QRect(0, 0, 1601, 900), true}}, 1000);
            auto resizes = sentResizes();
            QCOMPARE(resizes.size(), 1);
            QCOMPARE(resizes.first().pixels, QSize(1600, 900));
            QCOMPARE(resizes.first().output, u"virtual-desktop"_s);
            // A newer window size while that one is in flight waits for it.
            t.displayLayout({{QRect(0, 0, 1280, 720), true}}, 1000);
            QVERIFY(sentResizes().isEmpty());
            // The result is not a KRDPCTL reply (nobody asked over KRDPCTL) ...
            QVERIFY(t.resizeResult({resizes.first().requestId, resizes.first().generation, {}}, 1000).isEmpty());
            // ... and the waiting size goes out next.
            resizes = sentResizes();
            QCOMPARE(resizes.size(), 1);
            QCOMPARE(resizes.first().pixels, QSize(1280, 720));
            QVERIFY(t.resizeResult({resizes.first().requestId, resizes.first().generation, {}}, 1000).isEmpty());
            QVERIFY(pushed.isEmpty());
            // KRDPCTL's own `virtual-resize` still gets its reply.
            QVERIFY(t.request(resizeRequest(), 1000).isEmpty());
            resizes = sentResizes();
            QCOMPARE(resizes.size(), 1);
            QCOMPARE(t.resizeResult({resizes.first().requestId, resizes.first().generation, {}}, 1000).value(u"id"_s).toString(), u"fit-1"_s);
            // With a published topology, the current size is not resized again,
            // and a multi-output desktop is left to the KRDP client.
            RemoteTopologyCatalog catalog;
            auto current = *catalog.observe({{.backendKey = u"Virtual-0"_s, .name = u"Virtual-0"_s,
                .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1024, 576), .scale = 1.25, .enabled = true,
                .primary = true, .physical = false, .owner = t.m_handle->id}});
            t.setTopologyResolver([&current](const auto &) { return std::optional(current); });
            t.displayLayout({{QRect(0, 0, 1280, 720), true}}, 1000);
            QVERIFY(sentResizes().isEmpty());
            t.displayLayout({{QRect(0, 0, 1920, 1080), true}}, 1000);
            resizes = sentResizes();
            QCOMPARE(resizes.size(), 1);
            QCOMPARE(resizes.first().scale, 1.25); // the desktop keeps its scale
            QVERIFY(t.resizeResult({resizes.first().requestId, resizes.first().generation, {}}, 1000).isEmpty());
            current = *catalog.observe({
                {.backendKey = u"Virtual-0"_s, .name = u"Virtual-0"_s, .nativePixels = QSize(1280, 720),
                    .logicalGeometry = QRect(0, 0, 1280, 720), .scale = 1, .enabled = true, .primary = true, .physical = false, .owner = t.m_handle->id},
                {.backendKey = u"Virtual-1"_s, .name = u"Virtual-1"_s, .nativePixels = QSize(1280, 720),
                    .logicalGeometry = QRect(1280, 0, 1280, 720), .scale = 1, .enabled = true, .primary = false, .physical = false, .owner = t.m_handle->id}});
            t.displayLayout({{QRect(0, 0, 1600, 900), true}}, 1000);
            QVERIFY(sentResizes().isEmpty());
            QVERIFY(!t.m_displaySize);
            // Not the owner of an attached desktop: nothing is resized.
            t.displayLayout({{QRect(0, 0, 1600, 900), true}}, 1001);
            QVERIFY(sentResizes().isEmpty());
        });
    }

    void unauthenticatedConnectionCannotCreateOrEnableMedia()
    {
        int launched = 0;
        VirtualSessionSupervisor supervisor([&](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            ++launched;
            return {};
        });
        VirtualSessionControl control(supervisor, {});
        Server server;
        RdpConnection connection(&server, -1);
        quint64 sequence = 0;
        VirtualSessionTransport transport(1, &connection, control, {}, sequence);
        const auto response = transport.request({{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"1"_s}, {u"action"_s, u"create"_s}});
        QVERIFY(!response.value(u"ok"_s).toBool());
        QCOMPARE(launched, 0);
        const auto playback = transport.request(device(u"playback"_s, u"on"_s));
        QCOMPARE(playback.value(u"code"_s).toString(), u"not-owner"_s);
        QVERIFY(!transport.m_playback);
        QVERIFY(!control.attachment(1));
        transport.revoke();
        transport.revoke();
        QCOMPARE(sequence, quint64(0));
    }

    // AUD-FIX7: the virtual host's codec policy. The `codec` request is answered from the host's
    // encoders (a fake probe here: hardware HEVC, software AV1); the choice goes to the bound
    // worker with the binding's generation; the worker's own probe replaces the host's, and a
    // private codec it cannot encode is left at once.
    void codecPolicyRunsThroughTheWorker()
    {
        microphoneFixture([&](auto &t, auto &, auto &endpoint, auto &worker) {
            VideoCodecHost host;
            host.probe.encoders.avc = {true, true, true};
            host.probe.encoders.hevc = {true, false, false};
            host.probe.encoders.av1 = {false, true, false};
            t.setVideoCodecHost(host);
            workerRecords(worker);
            QVERIFY(t.m_codec->bound());
            const auto configs = [&](QLocalSocket &socket) {
                QList<ConsoleWorkerWire::EncoderConfig> result;
                for (int i = 0; i < 5; ++i) {
                    for (const auto &record : workerRecords(socket)) {
                        if (const auto config = ConsoleWorkerWire::encoderConfig(record)) result.append(*config);
                    }
                }
                return result;
            };

            // The client decodes HEVC and AV1; on a normal link the hardware codec wins.
            const auto reply = t.request(QJsonObject{{u"type"_s, u"codec"_s}, {u"v"_s, 1}, {u"codecs"_s, QJsonArray{u"hevc"_s, u"av1"_s}}}, 1000);
            QCOMPARE(reply.value(u"type"_s).toString(), u"codec"_s);
            QCOMPARE(reply.value(u"selected"_s).toString(), u"hevc"_s);
            QCOMPARE(reply.value(u"backend"_s).toString(), u"hardware"_s);
            auto *stream = t.m_connection->videoStream();
            QCOMPARE(stream->codecForSessions(), VideoCodec::Hevc);
            auto sent = configs(worker);
            QVERIFY(!sent.isEmpty());
            QCOMPARE(sent.last().generation, t.m_controlGeneration);
            QCOMPARE(sent.last().codec, VideoCodec::Hevc);
            QVERIFY(sent.last().settings && sent.last().settings->hardware);

            // The worker's own probe has no HEVC encoder (another render node): AVC at once.
            ConsoleWorkerWire::EncoderCaps caps;
            caps.encoders.avc = {true, true, true};
            worker.write(ConsoleWorkerWire::frame(caps));
            QVERIFY(worker.waitForBytesWritten(1000));
            QTRY_VERIFY(endpoint.encoderCaps().has_value());
            QTRY_COMPARE(stream->codecForSessions(), VideoCodec::Avc420);
            sent = configs(worker);
            QVERIFY(!sent.isEmpty());
            QCOMPARE(sent.last().codec, VideoCodec::Avc420);

            // Encoder events and the CPU time come back through the endpoint.
            worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::EncoderLoad{987654321}));
            QVERIFY(worker.waitForBytesWritten(1000));
            QTRY_COMPARE(endpoint.workerCpuNs(), qint64(987654321));
            QSignalSpy backend(&t.m_session, &AbstractSession::encoderBackendReported);
            worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::EncoderReport{ConsoleWorkerWire::EncoderReport::Event::Backend, VideoCodec::Avc420, false}));
            QVERIFY(worker.waitForBytesWritten(1000));
            QTRY_COMPARE(backend.size(), 1);
            QCOMPARE(backend.first().at(1).toBool(), false);

            // Detached: the bridge stops steering (the worker resets with the control change).
            t.revoke();
            QVERIFY(!t.m_codec->bound());
        });
    }

    // AUD-FIX7: forced software selection: only software HEVC, SoftwareEncoding=prefer. The
    // worker gets the software backend and the 30 fps cap of software HEVC/AV1; under `auto`
    // on a normal link the same host answers AVC and says why.
    void codecPolicyForcedSoftwareSelection()
    {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            VideoCodecHost host;
            host.probe.encoders.avc = {false, true, true};
            host.probe.encoders.hevc = {false, true, true};
            host.mode = CodecPolicy::SoftwareEncoding::Auto;
            t.setVideoCodecHost(host);
            auto reply = t.request(QJsonObject{{u"type"_s, u"codec"_s}, {u"v"_s, 1}, {u"codecs"_s, QJsonArray{u"hevc"_s}}}, 1000);
            QCOMPARE(reply.value(u"selected"_s).toString(), u"avc"_s);
            QCOMPARE(reply.value(u"backend"_s).toString(), u"software"_s);
            QVERIFY2(reply.value(u"reason"_s).toString().contains(u"software not selected"_s), qPrintable(reply.value(u"reason"_s).toString()));

            host.mode = CodecPolicy::SoftwareEncoding::Prefer;
            t.setVideoCodecHost(host);
            workerRecords(worker);
            reply = t.request(QJsonObject{{u"type"_s, u"codec"_s}, {u"v"_s, 1}, {u"codecs"_s, QJsonArray{u"hevc"_s}}, {u"adaptive"_s, false}}, 1000);
            QCOMPARE(reply.value(u"selected"_s).toString(), u"hevc"_s);
            QCOMPARE(reply.value(u"backend"_s).toString(), u"software"_s);
            std::optional<ConsoleWorkerWire::EncoderConfig> last;
            for (int i = 0; i < 5; ++i) {
                for (const auto &record : workerRecords(worker)) {
                    if (const auto config = ConsoleWorkerWire::encoderConfig(record)) last = *config;
                }
            }
            QVERIFY(last);
            QCOMPARE(last->codec, VideoCodec::Hevc);
            QVERIFY(last->settings && !last->settings->hardware);
            QCOMPARE(last->settings->maxFrameRate, CodecPolicy::SoftwarePrivateMaxFrameRate);
            QCOMPARE(last->frameRate, quint32(CodecPolicy::SoftwarePrivateMaxFrameRate));

            // Not an array, or not hevc/av1: invalid, nothing changes.
            QCOMPARE(t.request(QJsonObject{{u"type"_s, u"codec"_s}, {u"v"_s, 1}, {u"codecs"_s, u"hevc"_s}}, 1000).value(u"code"_s).toString(), u"invalid"_s);
            QCOMPARE(t.request(QJsonObject{{u"type"_s, u"codec"_s}, {u"v"_s, 1}, {u"codecs"_s, QJsonArray{u"vp9"_s}}}, 1000).value(u"code"_s).toString(), u"invalid"_s);
            QCOMPARE(t.m_connection->videoStream()->codecForSessions(), VideoCodec::Hevc);
        });
    }
};
}
QTEST_GUILESS_MAIN(KRdp::VirtualSessionTransportTest)
#include "VirtualSessionTransportTest.moc"
