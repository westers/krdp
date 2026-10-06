// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "CameraAvailability.h"
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

using namespace Qt::StringLiterals;

namespace KRdp
{
static QVector<VideoMonitor> oneMonitor() { return {{QRect(0, 0, 1366, 768), true}}; }


class ConsoleHostControllerTest : public QObject
{
    Q_OBJECT
    // OPT-060 D0: a one-monitor client sends no standard monitor block, so it asks with `console-screens-request`.
    struct RequestHarness {
        QTemporaryDir runtime;
        Server server;
        RdpConnection connection{&server, -1};
        ConsoleHostController host{&server, {}, {}};
        QList<QJsonObject> sent;
        ConsoleHostController::Client *client = nullptr;
        RequestHarness(const QByteArray &config, bool block, ConsoleSeat::Adapter adapter = ConsoleSeat::Adapter::PhysicalUser, bool advertised = true, bool admit = true)
        {
            host.m_recordSent = [this](RdpConnection *, const QJsonObject &record) { sent.append(record); };
            host.setUidResolver([](RdpConnection *) { return std::optional<quint32>(1000); });
            host.setUserSettingsReader([config](quint32) { return BrokerUserSettings::parse(config); });
            host.setDisplayInfoProvider([block](RdpConnection *) { return [block] { ClientDisplay::Info info; info.desktopSize = QSize(1920, 1080); if (block) info.monitors = {{QRect(0, 0, 1920, 1080), true}}; return info; }(); });
            host.m_endpoint.listen(runtime.filePath(QStringLiteral("worker.sock")), {adapter, QStringLiteral("3"), 1000}, QByteArray(32, 'x'));
            host.addClient(&connection);
            client = host.m_clients.front().get();
            client->uid = 1000;
            host.loadUserSettings(*client);
            client->capabilitiesSent = true;
            client->screensAdvertised = advertised;
            if (admit) {
                host.m_control.admit(client->id);
                host.syncControlState(); // admitted and in control with normal capture, as after a plain connect
            }
        }
        QJsonObject ask(const QVector<VideoMonitor> &monitors, const QString &id)
        {
            auto record = LayoutControl::consoleScreensRequestRecord(monitors);
            record.insert(QStringLiteral("requestId"), id);
            host.onControlRecord(&connection, client->id, record);
            return sent.last();
        }
    };
    // OPT-060 M-3/M-4: a connection that may carry a standard monitor block, a KRDPCTL channel and a grace window.
    struct MappedHarness {
        QTemporaryDir runtime;
        Server server;
        RdpConnection connection{&server, -1};
        ConsoleHostController host{&server, {}, {}};
        QList<QJsonObject> sent;
        ConsoleHostController::Client *client = nullptr;
        MappedHarness(const QByteArray &config, const QVector<VideoMonitor> &block, bool channel, int graceMs, int hostScreens = 2,
                      ConsoleSeat::Adapter adapter = ConsoleSeat::Adapter::PhysicalUser, bool advertised = true, bool admit = true)
        {
            host.m_recordSent = [this](RdpConnection *, const QJsonObject &record) { sent.append(record); };
            host.m_screensGraceMs = graceMs;
            host.m_controlChannelOf = [channel](RdpConnection *) { return channel; };
            host.m_authenticatedOf = [](RdpConnection *) { return true; };
            host.setUidResolver([](RdpConnection *) { return std::optional<quint32>(1000); });
            host.setUserSettingsReader([config](quint32) { return BrokerUserSettings::parse(config); });
            host.setDisplayInfoProvider([block](RdpConnection *) {
                ClientDisplay::Info info; info.desktopSize = QSize(1920, 1080); info.monitors = block; return info;
            });
            host.m_endpoint.listen(runtime.filePath(QStringLiteral("worker.sock")), {adapter, QStringLiteral("3"), 1000}, QByteArray(32, 'x'));
            host.m_hostScreens = hosts(hostScreens);
            host.addClient(&connection);
            client = host.m_clients.front().get();
            client->uid = 1000;
            host.loadUserSettings(*client); // the policy is first evaluated here, like at admission
            client->capabilitiesSent = true;
            client->screensAdvertised = advertised;
            if (admit) { host.m_control.admit(client->id); host.syncControlState(); }
        }
        // Hal-like host: DP-1 primary at (0,0), DP-2 at (2560,0), ... each 2560x1440 at scale 1.
        static QVector<OutputSnapshot::Output> hosts(int count)
        {
            QVector<OutputSnapshot::Output> result;
            for (int i = 0; i < count; ++i)
                result.append({QStringLiteral("DP-%1").arg(i + 1), true, QPoint(i * 2560, 0), i + 1, QSize(2560, 1440), 1.0});
            return result;
        }
        static LayoutControl::MappedClientMonitor mon(const QString &id, int x, int w, int h, int scalePercent = 100, bool primary = false)
        {
            return {id, QRect(x, 0, w, h), scalePercent, primary};
        }
        QJsonObject askMapped(const LayoutControl::ConsoleScreensMappedRequest &request, const QString &id, RdpConnection *from = nullptr)
        {
            auto record = LayoutControl::consoleScreensMappedRequestRecord(request);
            record.insert(QStringLiteral("requestId"), id);
            host.onControlRecord(from ? from : &connection, from ? host.m_clients.back()->id : client->id, record);
            for (qsizetype i = sent.size() - 1; i >= 0; --i)
                if (sent.at(i).value(u"type"_s).toString() == u"console-screens-request"_s) return sent.at(i);
            return {};
        }
        QList<QJsonObject> screens() const
        {
            QList<QJsonObject> result;
            for (const auto &record : sent) if (record.value(u"type"_s).toString() == u"console-screens"_s) result.append(record);
            return result;
        }
        ConsoleVirtualOutputPolicy policy() const { return client->codec->consoleVirtualPolicy(); }
    };
    static LayoutControl::ConsoleScreensMappedRequest oneClient(int scalePercent = 125)
    {
        LayoutControl::ConsoleScreensMappedRequest request;
        request.monitors = {MappedHarness::mon(u"eDP-1"_s, 0, 1920, 1080, scalePercent, true)};
        return request;
    }
    static QVector<VideoMonitor> twoBlock() { return {{QRect(0, 0, 1920, 1080), true}, {QRect(1920, 0, 1920, 1080), false}}; }
    static LayoutControl::ConsoleScreensMappedRequest twoClients()
    {
        LayoutControl::ConsoleScreensMappedRequest request;
        request.monitors = {MappedHarness::mon(u"a"_s, 0, 1920, 1080, 100, true), MappedHarness::mon(u"b"_s, 1920, 1920, 1080)};
        return request;
    }
    struct ViewHarness {
        MappedHarness base;
        RdpConnection viewerConnection{&base.server, -1};
        ConsoleHostController::Client *viewer = nullptr;
        QSignalSpy ownerFrames, viewerFrames;
        ViewHarness()
            : base("[General]\n", {}, true, 2000, 2)
            , ownerFrames(base.client->session.get(), &AbstractSession::frameReceived)
            , viewerFrames((base.host.addClient(&viewerConnection), base.host.m_clients.back()->session.get()), &AbstractSession::frameReceived)
        {
            viewer = base.host.m_clients.back().get();
            viewer->uid = 1000; base.host.loadUserSettings(*viewer); viewer->capabilitiesSent = viewer->screensAdvertised = true;
            base.host.m_control.admit(viewer->id); base.host.syncControlState();
            auto &host = base.host;
            // Two owned outputs published and verified, like an active Replace.
            QVERIFY(host.m_endpoint.target().adapter == ConsoleSeat::Adapter::PhysicalUser);
            host.m_configuredConsoleOutputs = host.m_physicalLeaseActive = true; host.m_physicalLeaseGeneration = host.m_controlGeneration;
            host.m_inputEnabled = true; base.client->session->setWorkerActive(true); viewer->session->setWorkerActive(true);
            Q_EMIT host.m_endpoint.outputsReceived({{{u"Virtual-krdp-h0-1920x1080"_s, QRect(0, 0, 1920, 1080), 1, true},
                                                        {u"Virtual-krdp-h1-1920x1080"_s, QRect(1920, 0, 1920, 1080), 1, false}}, QPoint(2560, 0)});
            Q_EMIT host.m_endpoint.topologyReceived({{{u"Virtual-krdp-h0-1920x1080"_s, QSize(1920, 1080), QRect(2560, 0, 1920, 1080), 1, true, 1, false},
                                                         {u"Virtual-krdp-h1-1920x1080"_s, QSize(1920, 1080), QRect(4480, 0, 1920, 1080), 1, false, 2, false}}, false});
        }
        QJsonObject view(ConsoleHostController::Client *who, const QJsonArray &visible, const QString &id)
        {
            QJsonObject record{{u"type"_s, u"console-screens-view"_s}, {u"v"_s, 1}, {u"visible"_s, visible}, {u"requestId"_s, id}};
            base.host.onControlRecord(who == viewer ? &viewerConnection : &base.connection, who->id, record);
            return base.sent.last();
        }
        void frame(int surface, bool key)
        {
            VideoFrame f; f.size = QSize(1920, 1080); f.isKeyFrame = key; f.monitorIndex = surface;
            f.monitors = {{QRect(0, 0, 1920, 1080), true}, {QRect(1920, 0, 1920, 1080), false}};
            Q_EMIT base.host.m_endpoint.frameReceived(f);
        }
    };

private Q_SLOTS:
    void physicalActivityReclaimIsGenerationBoundAndReleasesHeldKeys()
    {
        QTemporaryDir runtime; QVERIFY(runtime.isValid());
        Server server; RdpConnection connection(&server, -1), next(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.setUidResolver([](RdpConnection *) { return std::optional<quint32>(1000); });
        const QByteArray token(32, 't');
        QVERIFY(host.m_endpoint.listen(runtime.filePath(QStringLiteral("worker.sock")),
            {ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000}, token));
        host.addClient(&connection); host.addClient(&next);
        const auto first = host.m_clients.front()->id, second = host.m_clients.back()->id;
        host.m_control.admit(first); host.m_control.admit(second); host.syncControlState();
        QLocalSocket worker; worker.connectToServer(host.m_endpoint.socketName()); QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), 1000, token})
            + ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready)); QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(host.m_endpoint.ready());
        const auto oldGeneration = host.m_controlGeneration;
        host.physicalInputActivity();
        ConsoleWorkerWire::Deframer records;
        bool received = false;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            records.feed(worker.readAll());
            while (auto record = records.next())
                if (auto request = ConsoleWorkerWire::controlState(*record, ConsoleWorkerWire::Kind::ReclaimConsole))
                    received = request->active && request->generation == oldGeneration;
            return received;
        })(), 1000);
        QVERIFY(host.m_control.release(first));
        QVERIFY(host.m_control.acquire(second)); host.syncControlState();
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ControlState{oldGeneration, true}, ConsoleWorkerWire::Kind::LocalTakeover));
        worker.flush(); QTest::qWait(20);
        QVERIFY(host.m_control.ownsControl(second)); // An old device notification cannot revoke a newer owner.
        ConsoleWorkerWire::Input key; key.type = decltype(key.type)::Key; key.eventType = QEvent::KeyPress; key.nativeScanCode = 17;
        host.m_inputState.record(key);
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ControlState{host.m_controlGeneration, true}, ConsoleWorkerWire::Kind::LocalTakeover));
        worker.flush(); QTRY_COMPARE(host.m_control.owner(), quint64(0));
        QVERIFY(host.m_inputState.releaseAll().isEmpty());
    }
    void configuredOwnedResizeFitTransactions_data()
    {
        QTest::addColumn<int>("count"); QTest::addColumn<bool>("fit");
        QTest::addColumn<bool>("noOp"); QTest::addColumn<bool>("wrongReadback");
        QTest::newRow("single resize") << 1 << false << false << false;
        QTest::newRow("single Fit") << 1 << true << false << false;
        QTest::newRow("single no-op Fit") << 1 << true << true << false;
        QTest::newRow("two resize") << 2 << false << false << false;
        QTest::newRow("two Fit at large global origin") << 2 << true << false << false;
        QTest::newRow("wrong owned readback fails closed") << 2 << true << false << true;
    }

    void configuredOwnedResizeFitTransactions()
    {
        QFETCH(int, count); QFETCH(bool, fit); QFETCH(bool, noOp); QFETCH(bool, wrongReadback);
        QTemporaryDir runtime; QVERIFY(runtime.isValid());
        Server server; RdpConnection connection(&server, -1), viewer(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.setUidResolver([](RdpConnection *) { return std::optional<quint32>(1000); });
        const QByteArray credential(32, 't');
        QVERIFY(host.m_endpoint.listen(runtime.filePath(QStringLiteral("worker.sock")),
            {ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000}, credential));
        host.addClient(&connection); host.addClient(&viewer);
        const auto id = host.m_clients.front()->id, viewerId = host.m_clients.back()->id;
        QVERIFY(host.m_clients.front()->codec->setConsoleVirtualPolicy(*ConsoleVirtualOutputPolicy::parse(true,
            QStringLiteral("extend"), QStringLiteral("client"), QSize(1280, 720), {})));
        host.m_control.admit(id); host.m_control.admit(viewerId); host.syncControlState();
        QLocalSocket worker; worker.connectToServer(host.m_endpoint.socketName()); QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), 1000, credential})
            + ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready)); QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(host.m_endpoint.ready()); host.m_inputEnabled = true;
        ConsoleWorkerWire::Topology initial; initial.complete = false;
        ConsoleWorkerWire::Outputs outputs;
        for (int i = 0; i < count; ++i) {
            const auto name = QStringLiteral("Virtual-krdp-m%1-1280x720").arg(i);
            outputs.monitors.append({name, QRect(i * 1280, 0, 1280, 720), 1, i == 0});
            initial.outputs.append({name, QSize(1280, 720), QRect(16384 + i * 1280, 0, 1280, 720), 1, i == 0, quint8(i + 1), false});
        }
        outputs.compositorOrigin = QPoint(16384, 0);
        Q_EMIT host.m_endpoint.outputsReceived(outputs); Q_EMIT host.m_endpoint.topologyReceived(initial);
        QVERIFY(host.configuredOutputTopology()); QVERIFY(!host.m_experimentalPhysicalTopology); QVERIFY(!host.m_experimentalConsoleVirtual);
        const auto ownerCaps = host.consoleTopology(QStringLiteral("query"), id).value(QStringLiteral("capabilities")).toObject();
        QVERIFY(ownerCaps.value(QStringLiteral("consoleOwned")).toBool()); QVERIFY(ownerCaps.value(QStringLiteral("resize")).toBool());
        QVERIFY(!ownerCaps.value(QStringLiteral("physicalChange")).toBool()); QVERIFY(!ownerCaps.value(QStringLiteral("position")).toBool());
        QVERIFY(!ownerCaps.value(QStringLiteral("add")).toBool()); QVERIFY(!ownerCaps.value(QStringLiteral("primary")).toBool());
        const auto viewerCaps = host.consoleTopology(QStringLiteral("query"), viewerId).value(QStringLiteral("capabilities")).toObject();
        QVERIFY(viewerCaps.value(QStringLiteral("consoleOwned")).toBool()); QVERIFY(!viewerCaps.value(QStringLiteral("resize")).toBool());
        QJsonObject reply;
        host.m_recordSent = [&](RdpConnection *, const QJsonObject &record) { reply = record; };
        const auto before = host.m_topologyCatalog.snapshot();
        const auto selected = before.outputs[fit ? 0 : count - 1];
        const QSize pixels = noOp ? QSize(1280, 720) : fit ? QSize(1024, 768) : QSize(960, 540);
        QJsonObject request{{QStringLiteral("type"), fit ? QStringLiteral("topology-fit-preview") : QStringLiteral("topology-preview")},
            {QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("owned")},
            {QStringLiteral("generation"), before.generation}, {QStringLiteral("expectedRevision"), double(before.revision)}};
        const QJsonObject dimensions{{QStringLiteral("width"), pixels.width()}, {QStringLiteral("height"), pixels.height()}};
        if (fit) {
            QJsonArray relations;
            if (count == 2) relations.append(QJsonObject{{QStringLiteral("parent"), before.outputs[0].id},
                {QStringLiteral("child"), before.outputs[1].id}, {QStringLiteral("edge"), QStringLiteral("right")}, {QStringLiteral("offset"), 0}});
            request.insert(QStringLiteral("output"), selected.id); request.insert(QStringLiteral("pixels"), dimensions);
            request.insert(QStringLiteral("scale"), 1); request.insert(QStringLiteral("relations"), relations);
        } else {
            request.insert(QStringLiteral("allowRemoval"), false); request.insert(QStringLiteral("allowPhysicalChange"), false);
            request.insert(QStringLiteral("operations"), QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("resize")},
                {QStringLiteral("output"), selected.id}, {QStringLiteral("pixels"), dimensions}, {QStringLiteral("scale"), 1.25}}});
        }
        host.onControlRecord(&viewer, viewerId, request); QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("not-owner"));
        auto stale = request; stale.insert(QStringLiteral("expectedRevision"), double(before.revision + 1));
        host.onControlRecord(&connection, id, stale); QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("stale-revision"));
        host.onControlRecord(&connection, id, request); QVERIFY(host.m_virtualPreview); QVERIFY(host.m_virtualPreview->ownedMutation);
        QCOMPARE(reply.value(QStringLiteral("type")).toString(), QStringLiteral("topology-preview"));
        const auto expected = host.m_virtualPreview->draft.after;
        const QJsonObject commit{{QStringLiteral("type"), QStringLiteral("topology-commit")}, {QStringLiteral("v"), 1},
            {QStringLiteral("id"), QStringLiteral("owned")}, {QStringLiteral("token"), host.m_virtualPreview->token},
            {QStringLiteral("generation"), before.generation}, {QStringLiteral("expectedRevision"), double(before.revision)}};
        host.onControlRecord(&viewer, viewerId, commit); QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("not-owner"));
        QVERIFY(host.m_virtualPreview); QVERIFY(!host.m_pendingVirtual);
        auto staleCommit = commit; staleCommit.insert(QStringLiteral("expectedRevision"), double(before.revision + 1));
        host.onControlRecord(&connection, id, staleCommit); QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("stale-revision"));
        QVERIFY(!host.m_virtualPreview); QVERIFY(!host.m_pendingVirtual);
        host.onControlRecord(&connection, id, commit); QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("invalid"));
        host.onControlRecord(&connection, id, request); QVERIFY(host.m_virtualPreview);
        auto currentCommit = commit; currentCommit.insert(QStringLiteral("token"), host.m_virtualPreview->token);
        auto oldGeneration = currentCommit; oldGeneration.insert(QStringLiteral("generation"), QStringLiteral("retired"));
        host.onControlRecord(&connection, id, oldGeneration); QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("stale-generation"));
        QVERIFY(!host.m_virtualPreview); QVERIFY(!host.m_pendingVirtual);
        host.onControlRecord(&connection, id, request); QVERIFY(host.m_virtualPreview);
        currentCommit.insert(QStringLiteral("token"), host.m_virtualPreview->token);
        host.m_virtualPreview->age.invalidate();
        host.onControlRecord(&connection, id, currentCommit); QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("invalid"));
        QVERIFY(!host.m_virtualPreview); QVERIFY(!host.m_pendingVirtual);
        host.onControlRecord(&connection, id, request); QVERIFY(host.m_virtualPreview);
        currentCommit.insert(QStringLiteral("token"), host.m_virtualPreview->token);
        host.onControlRecord(&connection, id, currentCommit); QVERIFY(host.m_pendingVirtual); QVERIFY(!host.m_virtualPreview);
        const auto serial = host.m_pendingVirtual->serial, generation = host.m_pendingVirtual->controlGeneration;
        host.onControlRecord(&connection, id, currentCommit); QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("busy"));
        ConsoleWorkerWire::Deframer dispatched;
        int resizeCommands = 0, fitCommands = 0;
        QElapsedTimer deadline; deadline.start();
        while (resizeCommands + fitCommands == 0 && deadline.elapsed() < 1000) {
            QCoreApplication::processEvents();
            if (!worker.bytesAvailable()) worker.waitForReadyRead(20);
            dispatched.feed(worker.readAll());
            while (const auto record = dispatched.next()) {
                if (const auto command = ConsoleWorkerWire::resize(*record)) {
                    ++resizeCommands;
                    QCOMPARE(command->requestId, serial); QCOMPARE(command->generation, generation);
                    QCOMPARE(command->output, selected.output.backendKey); QCOMPARE(command->pixels, pixels);
                    QCOMPARE(command->scale, 1.25);
                }
                if (const auto command = ConsoleWorkerWire::managedFit(*record)) {
                    ++fitCommands;
                    QCOMPARE(command->requestId, serial); QCOMPARE(command->generation, generation);
                    QCOMPARE(command->output, selected.output.backendKey); QCOMPARE(command->pixels, pixels);
                    QCOMPARE(command->scale, 1.0); QCOMPARE(command->relations.size(), count - 1);
                    if (count == 2) {
                        QCOMPARE(command->relations.first().parent, before.outputs[0].output.backendKey);
                        QCOMPARE(command->relations.first().child, before.outputs[1].output.backendKey);
                        QCOMPARE(command->relations.first().edge, quint8(1));
                        QCOMPARE(command->relations.first().offset, 0);
                    }
                }
            }
        }
        QCOMPARE(resizeCommands, fit ? 0 : 1); QCOMPARE(fitCommands, fit ? 1 : 0);
        Q_EMIT host.m_endpoint.addVirtualFinished({serial, generation, {}}); QVERIFY(!host.m_pendingVirtual->waitingReadback);
        Q_EMIT host.m_endpoint.resizeFinished({serial + 1, generation, {}}); QVERIFY(!host.m_pendingVirtual->waitingReadback);
        Q_EMIT host.m_endpoint.managedFitFinished({serial, generation + 1, {}}); QVERIFY(!host.m_pendingVirtual->waitingReadback);
        if (fit) Q_EMIT host.m_endpoint.managedFitFinished({serial, generation, {}});
        else Q_EMIT host.m_endpoint.resizeFinished({serial, generation, {}});
        QVERIFY(host.m_pendingVirtual); QVERIFY(host.m_pendingVirtual->waitingReadback);
        ConsoleWorkerWire::Topology verified; verified.complete = false;
        ConsoleWorkerWire::Outputs captured;
        QRect workspace;
        for (const auto &entry : expected) workspace |= entry.output.logicalGeometry;
        for (int i = 0; i < expected.size(); ++i) {
            auto output = expected[i].output;
            if (wrongReadback && i == 0) { output.nativePixels = QSize(1000, 768); output.logicalGeometry.setWidth(1000); }
            captured.monitors.append({output.backendKey, output.logicalGeometry.translated(-workspace.topLeft()), output.scale, output.primary});
            verified.outputs.append({output.backendKey, output.nativePixels, output.logicalGeometry, output.scale,
                output.primary, quint8(i + 1), false});
        }
        captured.compositorOrigin = workspace.topLeft();
        Q_EMIT host.m_endpoint.outputsReceived(captured); QVERIFY(host.m_pendingVirtual);
        Q_EMIT host.m_endpoint.topologyReceived(verified); QVERIFY(!host.m_pendingVirtual);
        QVERIFY(host.m_physicalLeaseActive); // Socket/result is not verified creator release.
        if (wrongReadback) {
            QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("partial")); QVERIFY(!host.m_inputEnabled);
        } else {
            QVERIFY(reply.value(QStringLiteral("ok")).toBool()); QVERIFY(host.m_inputEnabled);
            QCOMPARE(host.m_topologyCatalog.snapshot().revision, before.revision + (noOp ? 0 : 1));
            QCOMPARE(host.m_topologyCatalog.snapshot().outputs, expected);
            host.onControlRecord(&connection, id, currentCommit); QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("invalid"));
        }
    }

    void configuredConsoleProjectionWaitsForOwnedReadbackAndVerifiedRelease()
    {
        QTemporaryDir runtime; QVERIFY(runtime.isValid());
        Server server; RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        QString error;
        QVERIFY(host.m_endpoint.listen(runtime.filePath(QStringLiteral("worker.sock")),
            {ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000}, QByteArray(32, 'x'), &error));
        host.addClient(&connection);
        auto &client = *host.m_clients.front();
        QVERIFY(client.codec->setConsoleVirtualPolicy(*ConsoleVirtualOutputPolicy::parse(true, QStringLiteral("extend"),
            QStringLiteral("client"), QSize(1280, 720), {})));
        host.m_control.admit(client.id); host.syncControlState();
        QVERIFY(host.m_configuredConsoleOutputs); QVERIFY(host.m_physicalLeaseActive); QVERIFY(host.m_layoutAwaitingReadback);
        host.m_inputEnabled = true; client.session->setWorkerActive(true);
        const ConsoleWorkerWire::Outputs outputs{{
            {QStringLiteral("Virtual-owned-0"), QRect(0, 0, 1280, 720), 1, true},
            {QStringLiteral("Virtual-owned-1"), QRect(1280, 0, 1280, 720), 1, false}}, QPoint(2560, 0)};
        Q_EMIT host.m_endpoint.outputsReceived(outputs);
        ConsoleWorkerWire::Topology projected{{
            {QStringLiteral("Virtual-owned-0"), QSize(1280, 720), QRect(2560, 0, 1280, 720), 1, true, 1, false},
            {QStringLiteral("Virtual-owned-1"), QSize(1280, 720), QRect(3840, 0, 1280, 720), 1, false, 2, false}}, false};
        auto unauthorized = projected; unauthorized.outputs[1].physical = true;
        Q_EMIT host.m_endpoint.topologyReceived(unauthorized);
        QVERIFY(!host.m_topologyAvailable); QVERIFY(host.m_layoutAwaitingReadback);
        auto complete = projected; complete.complete = true;
        Q_EMIT host.m_endpoint.topologyReceived(complete);
        QVERIFY(!host.m_topologyAvailable); QVERIFY(host.m_layoutAwaitingReadback);
        Q_EMIT host.m_endpoint.topologyReceived(projected);
        QVERIFY(host.m_topologyAvailable); QVERIFY(!host.m_topologyComplete); QVERIFY(!host.m_layoutAwaitingReadback);
        VideoFrame frame; frame.size = QSize(1280, 720); frame.isKeyFrame = true;
        frame.monitors = {{QRect(0, 0, 1280, 720), true}, {QRect(1280, 0, 1280, 720), false}};
        Q_EMIT host.m_endpoint.frameReceived(frame); QCOMPARE(client.wireLayout, frame.monitors);
        const auto generation = host.m_controlGeneration;
        QVERIFY(host.m_control.release(client.id)); host.syncControlState();
        QVERIFY(!host.m_inputEnabled); QVERIFY(host.m_physicalLeaseActive);
        Q_EMIT host.m_endpoint.physicalLeaseReleased({generation, false}); QVERIFY(host.m_physicalLeaseActive);
        Q_EMIT host.m_endpoint.physicalLeaseReleased({generation, true});
        QVERIFY(!host.m_physicalLeaseActive); QVERIFY(!host.m_configuredConsoleOutputs); QVERIFY(!host.m_inputEnabled);
    }

    static ClientDisplay::Info monitorBlock(bool sent)
    {
        ClientDisplay::Info info;
        info.desktopSize = QSize(1920, 1080);
        if (sent) info.monitors = {{QRect(0, 0, 1920, 1080), true}};
        return info;
    }

    // OPT-060 S2a: Replace needs the user's permission AND the connection's monitor block.
    void replaceNeedsPermissionAndMonitorBlock_data()
    {
        QTest::addColumn<QByteArray>("config");
        QTest::addColumn<bool>("block");
        QTest::addColumn<bool>("enabled");
        QTest::newRow("no block: normal capture") << QByteArray("[General]\n") << false << false;
        QTest::newRow("block, VirtualMonitorPolicy=off: normal capture") << QByteArray("[General]\nVirtualMonitorPolicy=off\n") << true << false;
        QTest::newRow("block, default asks: Replace") << QByteArray("[General]\n") << true << true;
        QTest::newRow("block, explicit replace: Replace") << QByteArray("[General]\nVirtualMonitorPolicy=replace\n") << true << true;
        QTest::newRow("legacy MonitorMode=virtual alone: still needs the block") << QByteArray("[General]\nMonitorMode=virtual\n") << false << false;
        QTest::newRow("legacy MonitorMode=virtual with block maps to ask") << QByteArray("[General]\nMonitorMode=virtual\n") << true << true;
        QTest::newRow("legacy extend with block: asks, keeps screens on") << QByteArray("[General]\nVirtualMonitorPolicy=extend\n") << true << true;
        QTest::newRow("off beats the legacy MonitorMode opt-in") << QByteArray("[General]\nVirtualMonitorPolicy=off\nMonitorMode=virtual\n") << true << false;
    }

    void replaceNeedsPermissionAndMonitorBlock()
    {
        QFETCH(QByteArray, config); QFETCH(bool, block); QFETCH(bool, enabled);
        Server server; RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.setUidResolver([](RdpConnection *) { return std::optional<quint32>(1000); });
        host.setUserSettingsReader([&](quint32) { return BrokerUserSettings::parse(config); });
        host.setDisplayInfoProvider([block](RdpConnection *) { return monitorBlock(block); });
        host.addClient(&connection);
        auto &client = *host.m_clients.front();
        client.uid = 1000;
        host.loadUserSettings(client);
        QCOMPARE(client.codec->consoleVirtualPolicy().enabled, enabled);
    }

    // OPT-060 S2b/d: one attempt per connection, never on the greeter.
    void replaceIsOneAttemptPerConnectionAndNeverOnTheGreeter()
    {
        QTemporaryDir runtime; QVERIFY(runtime.isValid());
        Server server; RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.setUidResolver([](RdpConnection *) { return std::optional<quint32>(1000); });
        host.setUserSettingsReader([](quint32) { return BrokerUserSettings::parse("[General]\n"); });
        host.setDisplayInfoProvider([](RdpConnection *) { return monitorBlock(true); });
        host.addClient(&connection);
        auto &client = *host.m_clients.front();
        client.uid = 1000;
        host.loadUserSettings(client);
        QVERIFY(client.codec->consoleVirtualPolicy().enabled);
        host.m_control.admit(client.id);
        // Greeter and any non-user target: plain capture, the attempt is not spent.
        QVERIFY(host.m_endpoint.listen(runtime.filePath(QStringLiteral("greeter.sock")),
            {ConsoleSeat::Adapter::Greeter, QStringLiteral("c1"), 970}, QByteArray(32, 'x')));
        host.syncControlState();
        QVERIFY(!host.m_configuredConsoleOutputs); QVERIFY(!client.replaceAttempted);
        host.m_endpoint.close();
        QVERIFY(host.m_endpoint.listen(runtime.filePath(QStringLiteral("user.sock")),
            {ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000}, QByteArray(32, 'y')));
        host.armConfiguredConsoleOutputs();
        QVERIFY(host.m_configuredConsoleOutputs); QVERIFY(client.replaceAttempted); QVERIFY(!client.replaceSpent);
        // The attempt fails and the worker dies: the next Ready must not re-arm, and the next
        // worker's policy is off, so it captures the host's screens as they are.
        host.clearPhysicalLease("test worker exit");
        QVERIFY(client.replaceSpent); QVERIFY(!host.m_configuredConsoleOutputs);
        QVERIFY(!client.codec->consoleVirtualPolicy().enabled);
        host.armConfiguredConsoleOutputs();
        QVERIFY(!host.m_configuredConsoleOutputs);
        host.updateClientDisplayPolicy(client);
        QVERIFY(!client.codec->consoleVirtualPolicy().enabled);
    }

    // OPT-060 S6: `console-screens` edges and `console-screens-restore`.
    void consoleScreensRecordAndRestoreRequest()
    {
        QTemporaryDir runtime; QVERIFY(runtime.isValid());
        Server server; RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        QList<QJsonObject> sent;
        host.m_recordSent = [&](RdpConnection *, const QJsonObject &record) { sent.append(record); };
        host.setUidResolver([](RdpConnection *) { return std::optional<quint32>(1000); });
        host.setUserSettingsReader([](quint32) { return BrokerUserSettings::parse("[General]\n"); });
        host.setDisplayInfoProvider([](RdpConnection *) { return monitorBlock(true); });
        QVERIFY(host.m_endpoint.listen(runtime.filePath(QStringLiteral("worker.sock")),
            {ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000}, QByteArray(32, 'x')));
        host.addClient(&connection);
        auto &client = *host.m_clients.front();
        client.uid = 1000;
        host.loadUserSettings(client);
        client.capabilitiesSent = client.screensAdvertised = true; // it received `capabilities.console.screens`
        const auto screens = [&] {
            QList<QJsonObject> result;
            for (const auto &record : sent) if (record.value(u"type"_s).toString() == u"console-screens"_s) result.append(record);
            return result;
        };
        host.m_control.admit(client.id); host.syncControlState();
        // Before Replace is up nothing is pushed and a restore request is refused with its requestId echoed.
        QVERIFY(host.m_configuredConsoleOutputs); QVERIFY(screens().isEmpty());
        host.onControlRecord(&connection, client.id, QJsonObject{{u"type"_s, u"console-screens-restore"_s}, {u"v"_s, 1}, {u"requestId"_s, u"r1"_s}});
        QCOMPARE(sent.last().value(u"type"_s).toString(), u"console-screens-restore"_s);
        QCOMPARE(sent.last().value(u"requestId"_s).toString(), u"r1"_s);
        QVERIFY(!sent.last().value(u"ok"_s).toBool());
        // The owned outputs are published and readback-verified: Replace is active.
        host.m_inputEnabled = true; client.session->setWorkerActive(true);
        const ConsoleWorkerWire::Outputs outputs{{
            {QStringLiteral("Virtual-owned-0"), QRect(0, 0, 1280, 720), 1, true}}, QPoint(2560, 0)};
        Q_EMIT host.m_endpoint.outputsReceived(outputs);
        Q_EMIT host.m_endpoint.topologyReceived({{{QStringLiteral("Virtual-owned-0"), QSize(1280, 720), QRect(2560, 0, 1280, 720), 1, true, 1, false}}, false});
        QVERIFY(host.m_topologyAvailable);
        host.syncScreensRecords();
        QCOMPARE(screens().size(), 1);
        QVERIFY(screens().last().value(u"active"_s).toBool()); QVERIFY(screens().last().value(u"canRestore"_s).toBool());
        host.syncScreensRecords(); QCOMPARE(screens().size(), 1); // one push per edge
        // Restore: the worker is asked to reclaim, Replace is over for this connection, the edge is pushed.
        host.onControlRecord(&connection, client.id, QJsonObject{{u"type"_s, u"console-screens-restore"_s}, {u"v"_s, 1}, {u"requestId"_s, u"r2"_s}});
        QCOMPARE(sent.last().value(u"requestId"_s).toString(), u"r2"_s);
        QVERIFY(sent.last().value(u"ok"_s).toBool());
        QCOMPARE(screens().size(), 2);
        QVERIFY(!screens().last().value(u"active"_s).toBool()); QVERIFY(!screens().last().value(u"canRestore"_s).toBool());
        QVERIFY(client.replaceSpent);
        QVERIFY(host.m_configuredConsoleOutputs); // the temporary outputs stay (extend) so capture continues
        QVERIFY(client.codec->consoleVirtualPolicy().enabled); // the live worker's plan is not torn down
        // Once that worker is gone the next one captures normally.
        host.clearPhysicalLease("test worker exit");
        QVERIFY(!client.codec->consoleVirtualPolicy().enabled);
        // A second restore request is refused.
        host.onControlRecord(&connection, client.id, QJsonObject{{u"type"_s, u"console-screens-restore"_s}, {u"v"_s, 1}, {u"requestId"_s, u"r3"_s}});
        QVERIFY(!sent.last().value(u"ok"_s).toBool());
    }


    void screensRequestOpensTheGateForAOneMonitorClient()
    {
        RequestHarness h("[General]\n", false);
        auto &client = *h.client;
        // Before the request: plain capture, nothing armed.
        QVERIFY(!client.codec->consoleVirtualPolicy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs); QVERIFY(!client.replaceAttempted);
        const auto reply = h.ask(oneMonitor(), u"q1"_s);
        QCOMPARE(reply.value(u"type"_s).toString(), u"console-screens-request"_s);
        QCOMPARE(reply.value(u"requestId"_s).toString(), u"q1"_s);
        QVERIFY(reply.value(u"ok"_s).toBool());
        // The late request (the worker already captures normally) behaves exactly like a standard block at Ready.
        QVERIFY(client.codec->consoleVirtualPolicy().enabled);
        QCOMPARE(client.codec->consoleVirtualPolicy().client.desktopSize, QSize(1366, 768));
        QVERIFY(h.host.m_configuredConsoleOutputs); QVERIFY(client.replaceAttempted); QVERIFY(!client.replaceSpent);
        // A duplicate (same monitors) is answered ok and changes nothing.
        const bool configured = h.host.m_configuredConsoleOutputs;
        const auto again = h.ask(oneMonitor(), u"q2"_s);
        QVERIFY(again.value(u"ok"_s).toBool()); QCOMPARE(again.value(u"requestId"_s).toString(), u"q2"_s);
        QCOMPARE(h.host.m_configuredConsoleOutputs, configured); QVERIFY(client.codec->consoleVirtualPolicy().enabled);
        // Different monitors after the attempt started: too late, refused, the running attempt is untouched.
        const auto late = h.ask({{QRect(0, 0, 1920, 1080), true}}, u"q3"_s);
        QVERIFY(!late.value(u"ok"_s).toBool()); QCOMPARE(late.value(u"requestId"_s).toString(), u"q3"_s);
        QCOMPARE(client.codec->consoleVirtualPolicy().client.desktopSize, QSize(1366, 768));
        // Once the attempt is over (worker gone) the next worker captures normally and a repeat does not re-arm it.
        h.host.clearPhysicalLease("test worker exit");
        QVERIFY(client.replaceSpent); QVERIFY(!client.codec->consoleVirtualPolicy().enabled);
        QVERIFY(h.ask(oneMonitor(), u"q4"_s).value(u"ok"_s).toBool()); // same request: idempotent, but nothing starts
        QVERIFY(!client.codec->consoleVirtualPolicy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs);
        QVERIFY(!h.ask({{QRect(0, 0, 1920, 1080), true}}, u"q5"_s).value(u"ok"_s).toBool());
        QVERIFY(!client.codec->consoleVirtualPolicy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs);
    }

    // OPT-060 D4: the user's MonitorMode must not stop Replace, in either order of block/request vs. saved preferences.
    void replaceIgnoresTheUsersMonitorMode_data()
    {
        QTest::addColumn<QByteArray>("config");
        QTest::addColumn<bool>("block");
        QTest::addColumn<int>("userMode");
        const struct { const char *name; const char *line; int mode; } modes[] = {
            {"specific", "MonitorMode=specific\nMonitorIndex=1\n", int(MonitorCapturePolicy::Mode::Specific)},
            {"primary", "MonitorMode=primary\n", int(MonitorCapturePolicy::Mode::Primary)},
            {"workspace", "MonitorMode=workspace\n", int(MonitorCapturePolicy::Mode::Workspace)},
            {"multi", "MonitorMode=multi\n", int(MonitorCapturePolicy::Mode::Multi)}};
        for (const auto &m : modes) {
            for (const bool block : {true, false}) {
                QTest::addRow("%s, %s", m.name, block ? "monitor block (policy after Replace)" : "request (policy before Replace)")
                    << QByteArray("[General]\n") + m.line << block << m.mode;
            }
        }
    }

    void replaceIgnoresTheUsersMonitorMode()
    {
        QFETCH(QByteArray, config); QFETCH(bool, block); QFETCH(int, userMode);
        RequestHarness h(config, block);
        auto &client = *h.client;
        if (!block) {
            QVERIFY(!client.codec->consoleVirtualPolicy().enabled);
            QVERIFY(h.ask({{QRect(0, 0, 1366, 768), true}}, u"d4"_s).value(u"ok"_s).toBool());
        }
        QVERIFY(client.codec->consoleVirtualPolicy().enabled);
        QCOMPARE(client.codec->capturePolicy(), MonitorCapturePolicy{}); // the worker captures its configured outputs
        // What the worker is sent is a valid config on the wire (the decoder refuses Replace with a non-default capture).
        client.codec->bind(&h.host.m_endpoint, 5);
        const auto sent = client.codec->config();
        QVERIFY(sent); QVERIFY(sent->consoleVirtual.enabled); QCOMPARE(sent->capture, MonitorCapturePolicy{});
        ConsoleWorkerWire::Deframer deframer; deframer.feed(ConsoleWorkerWire::frame(*sent));
        const auto record = deframer.next(); QVERIFY(record); QVERIFY(ConsoleWorkerWire::encoderConfig(*record));
        client.codec->unbind();
        QVERIFY(h.host.m_configuredConsoleOutputs); QVERIFY(client.replaceAttempted);
        // Fail-open: the attempt ends with the worker; the next one captures the way the user chose.
        h.host.clearPhysicalLease("test worker exit");
        QVERIFY(client.replaceSpent); QVERIFY(!client.codec->consoleVirtualPolicy().enabled);
        QCOMPARE(int(client.codec->capturePolicy().mode), userMode);
        QCOMPARE(client.codec->capturePolicy().index, userMode == int(MonitorCapturePolicy::Mode::Specific) ? 1 : 0);
        // The latch holds: a repeat does not re-arm it.
        h.ask({{QRect(0, 0, 1366, 768), true}}, u"d4b"_s);
        QVERIFY(!client.codec->consoleVirtualPolicy().enabled);
        QCOMPARE(int(client.codec->capturePolicy().mode), userMode);
    }

    void replaceIgnoresMonitorModeButPermissionOffStillBlocks()
    {
        RequestHarness h("[General]\nMonitorMode=specific\nVirtualMonitorPolicy=off\n", false);
        QVERIFY(!h.ask({{QRect(0, 0, 1366, 768), true}}, u"d4o"_s).value(u"ok"_s).toBool() || !h.client->codec->consoleVirtualPolicy().enabled);
        QVERIFY(!h.client->codec->consoleVirtualPolicy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs);
        QCOMPARE(h.client->codec->capturePolicy().mode, MonitorCapturePolicy::Mode::Specific);
    }

    void screensRequestBeforeControlArmsAtAdmission()
    {
        // The request can beat the admission/worker Ready (capabilities are sent first, control follows): the same
        // path as a monitor block then arms it when control is granted.
        RequestHarness h("[General]\n", false, ConsoleSeat::Adapter::PhysicalUser, true, false);
        auto &client = *h.client;
        QVERIFY(h.ask(oneMonitor(), u"e1"_s).value(u"ok"_s).toBool());
        QVERIFY(client.codec->consoleVirtualPolicy().enabled);
        QVERIFY(!h.host.m_configuredConsoleOutputs); // not in control yet: nothing armed
        h.host.m_control.admit(client.id); h.host.syncControlState();
        QVERIFY(h.host.m_configuredConsoleOutputs); QVERIFY(client.replaceAttempted);
    }

    void screensRequestRefusals()
    {
        { // The capability was not offered (the user's permission is Off): refused, plain capture, and nothing is armed.
            RequestHarness h("[General]\nVirtualMonitorPolicy=off\n", false, ConsoleSeat::Adapter::PhysicalUser, false);
            const auto reply = h.ask(oneMonitor(), u"o1"_s);
            QVERIFY(!reply.value(u"ok"_s).toBool()); QCOMPARE(reply.value(u"requestId"_s).toString(), u"o1"_s);
            QVERIFY(!h.client->codec->consoleVirtualPolicy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs);
            // Even if a client sends it although the capability said no: with permission Off the policy stays off.
            h.client->screensAdvertised = true;
            QVERIFY(h.ask(oneMonitor(), u"o2"_s).value(u"ok"_s).toBool());
            QVERIFY(!h.client->codec->consoleVirtualPolicy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs);
        }
        { // Malformed: refused, requestId echoed, nothing changes.
            RequestHarness h("[General]\n", false);
            for (const QString &bad : {u"bad1"_s, u"bad2"_s}) {
                QJsonObject record{{u"type"_s, u"console-screens-request"_s}, {u"v"_s, 1}, {u"requestId"_s, bad}, {u"replace"_s, true},
                    {u"monitors"_s, bad == u"bad1"_s ? QJsonArray{} : QJsonArray{QJsonObject{{u"x"_s, 0}, {u"y"_s, 0}, {u"width"_s, 100}, {u"height"_s, 100}}}}};
                h.host.onControlRecord(&h.connection, h.client->id, record);
                QVERIFY(!h.sent.last().value(u"ok"_s).toBool()); QCOMPARE(h.sent.last().value(u"requestId"_s).toString(), bad);
            }
            QVERIFY(!h.client->codec->consoleVirtualPolicy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs); QVERIFY(!h.client->screensRequest);
        }
        { // The greeter (or any non-user target): accepted as data, never armed, so capture stays normal.
            RequestHarness h("[General]\n", false, ConsoleSeat::Adapter::Greeter);
            QVERIFY(h.ask(oneMonitor(), u"g1"_s).value(u"ok"_s).toBool());
            QVERIFY(!h.host.m_configuredConsoleOutputs); QVERIFY(!h.client->replaceAttempted);
        }
        { // A standard monitor block is the request already: it wins and the explicit request changes nothing.
            RequestHarness h("[General]\n", true);
            QVERIFY(h.client->codec->consoleVirtualPolicy().enabled);
            QCOMPARE(h.client->codec->consoleVirtualPolicy().client.desktopSize, QSize(1920, 1080));
            QVERIFY(h.ask({{QRect(0, 0, 1366, 768), true}}, u"b1"_s).value(u"ok"_s).toBool());
            QVERIFY(!h.client->screensRequest);
            QCOMPARE(h.client->codec->consoleVirtualPolicy().client.desktopSize, QSize(1920, 1080));
        }
        { // An old client that never sends the record, and no block: plain capture, no record pushed.
            RequestHarness h("[General]\n", false);
            h.host.syncScreensRecords();
            QVERIFY(h.sent.isEmpty()); QVERIFY(!h.client->codec->consoleVirtualPolicy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs);
        }
    }


    // ---- OPT-060 M-3: `console-screens-request` v2 ------------------------------------------------------------------

    void mappedRequestAloneArmsAMappedReplace()
    {
        MappedHarness h("[General]\n", {}, true, 2000);
        QVERIFY(!h.policy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs); // no block, no request: plain capture
        auto request = oneClient(125);
        request.mapping = {{u"DP-2"_s, u"eDP-1"_s}, {u"DP-9"_s, u"eDP-1"_s}};
        const auto reply = h.askMapped(request, u"m1"_s);
        QCOMPARE(reply.value(u"type"_s).toString(), u"console-screens-request"_s);
        QCOMPARE(reply.value(u"v"_s).toInt(), 2);
        QCOMPARE(reply.value(u"requestId"_s).toString(), u"m1"_s);
        QVERIFY(reply.value(u"ok"_s).toBool());
        QCOMPARE(reply.value(u"unknownHosts"_s).toArray(), QJsonArray{u"DP-9"_s}); // not on the host: ignored and reported
        QVERIFY(h.policy().enabled);
        QCOMPARE(h.policy().layout, ConsoleVirtualOutputPolicy::Layout::Mapped);
        QCOMPARE(h.policy().mappedMonitors.size(), 1); QCOMPARE(h.policy().mappedMonitors.first().scalePercent, 125);
        QCOMPARE(h.policy().mapping.size(), 2);
        QVERIFY(h.host.m_configuredConsoleOutputs); QVERIFY(h.client->replaceAttempted); QVERIFY(!h.client->replaceSpent);
        // What the worker is sent is a valid wire config carrying the mapping (wire 14).
        h.client->codec->bind(&h.host.m_endpoint, 5);
        const auto config = h.client->codec->config();
        QVERIFY(config); QCOMPARE(config->consoleVirtual.layout, ConsoleVirtualOutputPolicy::Layout::Mapped);
        ConsoleWorkerWire::Deframer deframer; deframer.feed(ConsoleWorkerWire::frame(*config));
        const auto record = deframer.next(); QVERIFY(record); QVERIFY(ConsoleWorkerWire::encoderConfig(*record));
        h.client->codec->unbind();
        // The same request again is idempotent; the standard-block-less client keeps its first attempt.
        QVERIFY(h.askMapped(request, u"m2"_s).value(u"ok"_s).toBool());
        QCOMPARE(h.policy().layout, ConsoleVirtualOutputPolicy::Layout::Mapped);
    }

    void mappedRequestPlusEqualBlockWinsTheGrace()
    {
        MappedHarness h("[General]\n", twoBlock(), true, 5000);
        // A KRDPCTL client that sent a block waits for its v2 request: nothing armed yet.
        QVERIFY(!h.policy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs); QVERIFY(!h.client->replaceAttempted);
        // The same monitors (even moved to another origin) are the block's request: the mapping is added, no waiting.
        auto request = twoClients();
        for (auto &monitor : request.monitors) monitor.geometry.translate(100, 40);
        request.mapping = {{u"DP-1"_s, u"b"_s}};
        const auto reply = h.askMapped(request, u"b1"_s);
        QVERIFY(reply.value(u"ok"_s).toBool());
        QVERIFY(h.policy().enabled); QCOMPARE(h.policy().layout, ConsoleVirtualOutputPolicy::Layout::Mapped);
        QCOMPARE(h.policy().mappedMonitors.size(), 2);
        QVERIFY(h.host.m_configuredConsoleOutputs); QVERIFY(h.client->replaceAttempted);
        QVERIFY(!h.client->graceExpired);
    }

    void mappedRequestPlusDifferentBlockIsRefusedAndTheBlockApplies()
    {
        MappedHarness h("[General]\n", twoBlock(), true, 120);
        QVERIFY(!h.policy().enabled);
        auto other = twoClients();
        other.monitors[1].geometry = QRect(1920, 0, 1280, 1024); // not the block's rectangle
        const auto reply = h.askMapped(other, u"x1"_s);
        QVERIFY(!reply.value(u"ok"_s).toBool()); QCOMPARE(reply.value(u"refusal"_s).toString(), u"blockMismatch"_s);
        QCOMPARE(reply.value(u"requestId"_s).toString(), u"x1"_s); QCOMPARE(reply.value(u"v"_s).toInt(), 2);
        QVERIFY(!h.client->mappedRequest);
        // A different primary flag is a different set too.
        auto flipped = twoClients();
        flipped.monitors[0].primary = false; flipped.monitors[1].primary = true;
        QCOMPARE(h.askMapped(flipped, u"x2"_s).value(u"refusal"_s).toString(), u"blockMismatch"_s);
        // Grace expiry: the block arms the one-output-per-client-monitor layout exactly as before the grace existed.
        QTRY_VERIFY_WITH_TIMEOUT(h.client->graceExpired, 2000);
        QVERIFY(h.policy().enabled); QCOMPARE(h.policy().layout, ConsoleVirtualOutputPolicy::Layout::Client);
        QVERIFY(h.policy().mappedMonitors.isEmpty());
        QVERIFY(h.host.m_configuredConsoleOutputs); QVERIFY(h.client->replaceAttempted);
    }

    void graceExpiryArmsTheBlockAndALateMappedRequestIsTooLate()
    {
        MappedHarness h("[General]\n", twoBlock(), true, 100);
        QVERIFY(!h.host.m_configuredConsoleOutputs);
        QTRY_VERIFY_WITH_TIMEOUT(h.host.m_configuredConsoleOutputs, 2000);
        QVERIFY(h.client->graceExpired); QVERIFY(h.client->replaceAttempted);
        QCOMPARE(h.policy().layout, ConsoleVirtualOutputPolicy::Layout::Client);
        const auto late = h.askMapped(twoClients(), u"l1"_s);
        QVERIFY(!late.value(u"ok"_s).toBool()); QCOMPARE(late.value(u"refusal"_s).toString(), u"alreadyAttempted"_s);
        QVERIFY(late.value(u"message"_s).toString().contains(u"too late"_s));
        QCOMPARE(h.policy().layout, ConsoleVirtualOutputPolicy::Layout::Client); // the running attempt is untouched
    }

    void aStockClientNeverWaitsForTheGrace()
    {
        // No KRDPCTL channel (stock /multimon client): the block arms at once, one output per client monitor.
        MappedHarness h("[General]\n", twoBlock(), false, 5000);
        QVERIFY(h.policy().enabled); QCOMPARE(h.policy().layout, ConsoleVirtualOutputPolicy::Layout::Client);
        QVERIFY(h.host.m_configuredConsoleOutputs); QVERIFY(h.client->replaceAttempted);
        QVERIFY(!h.client->graceStarted);
    }

    void aKrdpctlClientWithoutABlockOrWithPermissionOffNeverWaits()
    {
        { MappedHarness h("[General]\n", {}, true, 5000); QVERIFY(!h.client->graceStarted); }
        { MappedHarness h("[General]\nVirtualMonitorPolicy=off\n", twoBlock(), true, 5000, 2, ConsoleSeat::Adapter::PhysicalUser, false);
          QVERIFY(!h.client->graceStarted); QVERIFY(!h.policy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs); }
    }

    void mappedRequestFromAViewerChangesNothingForTheController()
    {
        MappedHarness h("[General]\n", {}, true, 2000);
        RdpConnection viewerConnection(&h.server, -1);
        h.host.addClient(&viewerConnection);
        auto &viewer = *h.host.m_clients.back();
        viewer.uid = 1000; h.host.loadUserSettings(viewer); viewer.capabilitiesSent = viewer.screensAdvertised = true;
        h.host.m_control.admit(viewer.id); h.host.syncControlState();
        QVERIFY(h.host.m_control.ownsControl(h.client->id)); QVERIFY(!h.host.m_control.ownsControl(viewer.id));
        const auto reply = h.askMapped(oneClient(), u"v1"_s, &viewerConnection);
        QVERIFY(reply.value(u"ok"_s).toBool()); QCOMPARE(reply.value(u"requestId"_s).toString(), u"v1"_s);
        QVERIFY(viewer.mappedRequest); // kept for the day it holds control on a fresh connection
        QVERIFY(!h.policy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs); QVERIFY(!h.client->replaceAttempted);
    }

    void mappedRequestRespectsPermissionGreeterAndTheLatch()
    {
        { // Permission Off, capability not offered: refused; plain capture.
            MappedHarness h("[General]\nVirtualMonitorPolicy=off\n", {}, true, 2000, 2, ConsoleSeat::Adapter::PhysicalUser, false);
            const auto reply = h.askMapped(oneClient(), u"o1"_s);
            QVERIFY(!reply.value(u"ok"_s).toBool()); QCOMPARE(reply.value(u"refusal"_s).toString(), u"permissionOff"_s);
            QVERIFY(!h.policy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs);
            // Even if the client sends it although the capability said no, permission Off keeps the policy off.
            h.client->screensAdvertised = true;
            QVERIFY(h.askMapped(oneClient(), u"o2"_s).value(u"ok"_s).toBool());
            QVERIFY(!h.policy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs);
        }
        { // The greeter: accepted as data, never armed.
            MappedHarness h("[General]\n", {}, true, 2000, 2, ConsoleSeat::Adapter::Greeter);
            QVERIFY(h.askMapped(oneClient(), u"g1"_s).value(u"ok"_s).toBool());
            QVERIFY(h.client->mappedRequest); QVERIFY(!h.host.m_configuredConsoleOutputs); QVERIFY(!h.client->replaceAttempted);
        }
        { // One attempt per connection: a different request after the attempt started is too late; the same one is idempotent;
          // once the attempt is over nothing re-arms.
            MappedHarness h("[General]\n", {}, true, 2000);
            QVERIFY(h.askMapped(oneClient(125), u"a1"_s).value(u"ok"_s).toBool());
            QVERIFY(h.client->replaceAttempted);
            const auto other = h.askMapped(oneClient(150), u"a2"_s);
            QVERIFY(!other.value(u"ok"_s).toBool()); QCOMPARE(other.value(u"refusal"_s).toString(), u"alreadyAttempted"_s);
            QCOMPARE(h.policy().mappedMonitors.first().scalePercent, 125);
            h.host.clearPhysicalLease("test worker exit");
            QVERIFY(h.client->replaceSpent); QVERIFY(!h.policy().enabled);
            QVERIFY(h.askMapped(oneClient(125), u"a3"_s).value(u"ok"_s).toBool()); // same request: nothing starts
            QVERIFY(!h.policy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs);
        }
    }

    void mappedRequestRefusals()
    {
        { // Malformed and unknown client monitor in the mapping.
            MappedHarness h("[General]\n", {}, true, 2000);
            auto bad = LayoutControl::consoleScreensMappedRequestRecord(oneClient()); bad.insert(u"extra"_s, 1); bad.insert(u"requestId"_s, u"e1"_s);
            h.host.onControlRecord(&h.connection, h.client->id, bad);
            QVERIFY(!h.sent.last().value(u"ok"_s).toBool()); QCOMPARE(h.sent.last().value(u"refusal"_s).toString(), u"invalid"_s);
            QCOMPARE(h.sent.last().value(u"requestId"_s).toString(), u"e1"_s);
            auto unknown = oneClient(); unknown.mapping = {{u"DP-1"_s, u"nope"_s}};
            auto record = LayoutControl::consoleScreensMappedRequestRecord(unknown); record.insert(u"mapping"_s, QJsonArray{QJsonObject{{u"host"_s, u"DP-1"_s}, {u"monitor"_s, u"nope"_s}}});
            record.insert(u"requestId"_s, u"e2"_s);
            h.host.onControlRecord(&h.connection, h.client->id, record);
            QVERIFY(!h.sent.last().value(u"ok"_s).toBool()); QCOMPARE(h.sent.last().value(u"refusal"_s).toString(), u"unknownMonitor"_s);
            QVERIFY(!h.policy().enabled); QVERIFY(!h.client->mappedRequest); QVERIFY(!h.host.m_configuredConsoleOutputs);
        }
        { // Hard cap: 5 host screens -> refused with a clear reason, plain capture, even for a block client after its grace.
            MappedHarness h("[General]\n", twoBlock(), true, 80, 5);
            const auto reply = h.askMapped(twoClients(), u"c1"_s);
            QVERIFY(reply.value(u"ok"_s).toBool()); QCOMPARE(reply.value(u"refusal"_s).toString(), u"tooManyScreens"_s); // taken, then refused
            QVERIFY(reply.value(u"message"_s).toString().contains(u"5 screens"_s)); QVERIFY(reply.value(u"message"_s).toString().contains(u"4"_s));
            QCOMPARE(h.screens().size(), 1);
            QVERIFY(!h.screens().last().value(u"active"_s).toBool()); QCOMPARE(h.screens().last().value(u"reason"_s).toString(), u"tooManyScreens"_s);
            QVERIFY(h.screens().last().value(u"message"_s).toString().contains(u"5 screens"_s));
            QTRY_VERIFY_WITH_TIMEOUT(h.client->graceExpired, 2000);
            QVERIFY(!h.policy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs); QVERIFY(!h.client->replaceAttempted);
        }
        { // Exactly 4 host screens is the maximum and works.
            MappedHarness h("[General]\n", {}, true, 2000, 4);
            QVERIFY(h.askMapped(oneClient(), u"c4"_s).value(u"ok"_s).toBool());
            QVERIFY(h.policy().enabled); QVERIFY(h.host.m_configuredConsoleOutputs);
        }
        { // Union limit: three 4096-wide stand-ins cannot fit 8192 even packed.
            MappedHarness h("[General]\n", {}, true, 2000, 3);
            LayoutControl::ConsoleScreensMappedRequest big;
            big.monitors = {MappedHarness::mon(u"big"_s, 0, 4096, 2160, 100, true)};
            const auto reply = h.askMapped(big, u"u1"_s);
            QVERIFY(reply.value(u"ok"_s).toBool()); QCOMPARE(reply.value(u"refusal"_s).toString(), u"desktopTooLarge"_s);
            QCOMPARE(h.screens().last().value(u"reason"_s).toString(), u"failed"_s);
            QVERIFY(!h.policy().enabled); QVERIFY(!h.host.m_configuredConsoleOutputs);
        }
        { // The host's screens are not known yet (nothing captured plainly so far): taken, the worker plans and may refuse.
            MappedHarness h("[General]\n", {}, true, 2000, 0);
            QVERIFY(h.askMapped(oneClient(), u"k1"_s).value(u"ok"_s).toBool());
            QVERIFY(h.policy().enabled);
        }
    }

    void mappedRequestNeverFallsBackToAnotherLayoutAfterAV1Request()
    {
        // A v1 request after a v2 one replaces it (one-output-per-monitor); the mapped state does not linger.
        MappedHarness h("[General]\n", {}, true, 2000);
        h.host.m_endpoint.close();
        QVERIFY(h.host.m_endpoint.listen(h.runtime.filePath(QStringLiteral("greeter.sock")), {ConsoleSeat::Adapter::Greeter, QStringLiteral("c"), 970}, QByteArray(32, 'g')));
        QVERIFY(h.askMapped(oneClient(), u"p1"_s).value(u"ok"_s).toBool());
        QVERIFY(h.client->mappedRequest); QVERIFY(!h.host.m_configuredConsoleOutputs); // greeter: stored, not armed
        auto v1 = LayoutControl::consoleScreensRequestRecord({{QRect(0, 0, 1366, 768), true}}); v1.insert(u"requestId"_s, u"p2"_s);
        h.host.onControlRecord(&h.connection, h.client->id, v1);
        QVERIFY(h.sent.last().value(u"ok"_s).toBool());
        QVERIFY(!h.client->mappedRequest);
        QCOMPARE(h.policy().layout, ConsoleVirtualOutputPolicy::Layout::Client);
    }

    void capabilitiesAdvertiseMappedMaxScreensAndView()
    {
        for (const bool allowed : {true, false}) {
            MappedHarness h(allowed ? "[General]\n" : "[General]\nVirtualMonitorPolicy=off\n", {}, true, 2000, 2, ConsoleSeat::Adapter::PhysicalUser, false, false);
            h.client->capabilitiesSent = false; h.client->screensAdvertised = false;
            h.host.sendCapabilities(*h.client);
            QVERIFY(h.client->capabilitiesSent);
            const auto caps = h.sent.last();
            QCOMPARE(caps.value(u"type"_s).toString(), u"capabilities"_s);
            const auto group = caps.value(u"console"_s).toObject().value(u"screens"_s).toObject();
            if (allowed) {
                QCOMPARE(group, (QJsonObject{{u"replace"_s, true}, {u"restore"_s, true}, {u"request"_s, true}, {u"mapped"_s, true}, {u"maxScreens"_s, 4}, {u"view"_s, true}}));
            } else {
                QVERIFY(group.isEmpty()); QVERIFY(!caps.contains(u"console"_s));
            }
            QCOMPARE(h.client->screensAdvertised, allowed);
        }
    }

    void activeRecordCarriesTheMappedScreens()
    {
        MappedHarness h("[General]\n", {}, true, 2000, 2);
        auto request = oneClient(125);
        request.monitors.append(MappedHarness::mon(u"DP-2"_s, 1920, 1280, 800));
        QVERIFY(h.askMapped(request, u"s1"_s).value(u"ok"_s).toBool());
        QVERIFY(h.host.m_configuredConsoleOutputs);
        h.host.m_inputEnabled = true; h.client->session->setWorkerActive(true);
        // The worker created what the same planner predicted: h0 -> eDP-1 (default), h1 -> DP-2 (default).
        const QString n0 = u"Virtual-krdp-h0-1920x1080"_s, n1 = u"Virtual-krdp-h1-1280x800"_s;
        Q_EMIT h.host.m_endpoint.outputsReceived({{{n0, QRect(0, 0, 1536, 864), 1.25, true}, {n1, QRect(1536, 0, 1280, 800), 1, false}}, QPoint(2560, 0)});
        Q_EMIT h.host.m_endpoint.topologyReceived({{{n0, QSize(1920, 1080), QRect(2560, 0, 1536, 864), 1.25, true, 1, false},
                                                       {n1, QSize(1280, 800), QRect(4096, 0, 1280, 800), 1, false, 2, false}}, false});
        QVERIFY(h.host.m_topologyAvailable);
        h.host.syncScreensRecords();
        QCOMPARE(h.screens().size(), 1);
        const auto record = h.screens().last();
        QVERIFY(record.value(u"active"_s).toBool());
        QCOMPARE(record.value(u"reason"_s).toString(), u"connect"_s);
        QCOMPARE(record.value(u"layout"_s).toString(), u"mapped"_s);
        const auto screens = record.value(u"screens"_s).toArray();
        QCOMPARE(screens.size(), 2);
        const auto first = screens.at(0).toObject(), second = screens.at(1).toObject();
        QCOMPARE(first.value(u"host"_s).toString(), u"DP-1"_s);
        QCOMPARE(first.value(u"output"_s).toString(), n0);
        QCOMPARE(first.value(u"monitor"_s).toString(), u"eDP-1"_s);
        QCOMPARE(first.value(u"surface"_s).toInt(), 0);
        QCOMPARE(first.value(u"width"_s).toInt(), 1920); QCOMPARE(first.value(u"scale"_s).toDouble(), 1.25);
        QVERIFY(first.value(u"primary"_s).toBool()); QVERIFY(first.value(u"default"_s).toBool());
        QCOMPARE(second.value(u"host"_s).toString(), u"DP-2"_s);
        QCOMPARE(second.value(u"monitor"_s).toString(), u"DP-2"_s);
        QCOMPARE(second.value(u"surface"_s).toInt(), 1); QCOMPARE(second.value(u"height"_s).toInt(), 800);
        QVERIFY(!record.contains(u"unmappedMonitors"_s)); QVERIFY(!record.contains(u"unknownHosts"_s));
        // Restore: the next edge carries no screens.
        h.host.onControlRecord(&h.connection, h.client->id, QJsonObject{{u"type"_s, u"console-screens-restore"_s}, {u"v"_s, 1}, {u"requestId"_s, u"r"_s}});
        QCOMPARE(h.screens().size(), 2);
        QVERIFY(!h.screens().last().value(u"active"_s).toBool()); QVERIFY(!h.screens().last().contains(u"screens"_s));
    }

    void activeRecordOmitsScreensWhenTheWorkerOutputsDiffer()
    {
        MappedHarness h("[General]\n", {}, true, 2000, 2);
        QVERIFY(h.askMapped(oneClient(100), u"s2"_s).value(u"ok"_s).toBool());
        h.host.m_inputEnabled = true; h.client->session->setWorkerActive(true);
        Q_EMIT h.host.m_endpoint.outputsReceived({{{u"Virtual-other-0"_s, QRect(0, 0, 1920, 1080), 1, true}}, QPoint(2560, 0)});
        Q_EMIT h.host.m_endpoint.topologyReceived({{{u"Virtual-other-0"_s, QSize(1920, 1080), QRect(2560, 0, 1920, 1080), 1, true, 1, false}}, false});
        h.host.syncScreensRecords();
        QCOMPARE(h.screens().size(), 1);
        QCOMPARE(h.screens().last().value(u"layout"_s).toString(), u"mapped"_s);
        QVERIFY(!h.screens().last().contains(u"screens"_s)); // never a guess about which output replaced which screen
    }

    // ---- OPT-060 M-8: a host monitor plugged in while replaced -------------------------------------------------------

    void hostScreensChangedEndsReplaceAndContinuesAsExtendOnce()
    {
        MappedHarness h("[General]\n", {}, true, 2000, 2);
        QVERIFY(h.askMapped(oneClient(100), u"hc"_s).value(u"ok"_s).toBool());
        h.host.m_inputEnabled = true; h.client->session->setWorkerActive(true);
        const QString n0 = u"Virtual-krdp-h0-1920x1080"_s, n1 = u"Virtual-krdp-h1-1920x1080"_s;
        Q_EMIT h.host.m_endpoint.outputsReceived({{{n0, QRect(0, 0, 1920, 1080), 1, true}, {n1, QRect(1920, 0, 1920, 1080), 1, false}}, QPoint(2560, 0)});
        Q_EMIT h.host.m_endpoint.topologyReceived({{{n0, QSize(1920, 1080), QRect(2560, 0, 1920, 1080), 1, true, 1, false},
                                                       {n1, QSize(1920, 1080), QRect(4480, 0, 1920, 1080), 1, false, 2, false}}, false});
        h.host.syncScreensRecords();
        QCOMPARE(h.screens().size(), 1); QVERIFY(h.screens().last().value(u"active"_s).toBool());
        // A stale generation is ignored.
        Q_EMIT h.host.m_endpoint.hostScreensChanged(h.host.m_controlGeneration + 1);
        QCOMPARE(h.screens().size(), 1); QVERIFY(!h.client->replaceSpent);
        // The worker restored the host's screens (extend) and says why: Replace is over, the reason is the new one.
        Q_EMIT h.host.m_endpoint.hostScreensChanged(h.host.m_controlGeneration);
        QCOMPARE(h.screens().size(), 2);
        const auto record = h.screens().last();
        QVERIFY(!record.value(u"active"_s).toBool()); QVERIFY(!record.value(u"canRestore"_s).toBool());
        QCOMPARE(record.value(u"reason"_s).toString(), u"hostScreensChanged"_s);
        QVERIFY(!record.value(u"message"_s).toString().isEmpty()); QVERIFY(!record.contains(u"screens"_s));
        QVERIFY(h.client->replaceSpent);
        QVERIFY(h.host.m_configuredConsoleOutputs); // the temporary outputs stay: the remote session continues as extend
        QVERIFY(h.policy().enabled);                // the live worker's plan is not torn down
        // Never a loop: a repeat changes nothing, a restore request is refused, and no new attempt is armed.
        Q_EMIT h.host.m_endpoint.hostScreensChanged(h.host.m_controlGeneration);
        QCOMPARE(h.screens().size(), 2);
        h.host.onControlRecord(&h.connection, h.client->id, QJsonObject{{u"type"_s, u"console-screens-restore"_s}, {u"v"_s, 1}, {u"requestId"_s, u"rr"_s}});
        QVERIFY(!h.sent.last().value(u"ok"_s).toBool());
        h.host.clearPhysicalLease("test worker exit");
        QVERIFY(!h.policy().enabled);
        h.host.armConfiguredConsoleOutputs();
        QVERIFY(!h.host.m_configuredConsoleOutputs);
    }

    void hostScreensChangedWithoutAnActiveReplaceChangesNothing()
    {
        MappedHarness h("[General]\n", {}, true, 2000, 2);
        Q_EMIT h.host.m_endpoint.hostScreensChanged(h.host.m_controlGeneration);
        QVERIFY(h.screens().isEmpty()); QVERIFY(!h.client->replaceSpent);
    }

    void aMappedAttemptThatNeverBecameActiveIsReportedOnce()
    {
        MappedHarness h("[General]\n", {}, true, 2000);
        QVERIFY(h.askMapped(oneClient(), u"f1"_s).value(u"ok"_s).toBool());
        QVERIFY(h.screens().isEmpty());
        h.host.clearPhysicalLease("test worker exit"); // the plan failed at the worker: fail open
        QCOMPARE(h.screens().size(), 1);
        const auto record = h.screens().last();
        QVERIFY(!record.value(u"active"_s).toBool()); QCOMPARE(record.value(u"reason"_s).toString(), u"workerExit"_s);
        QVERIFY(!record.value(u"message"_s).toString().isEmpty());
        h.host.syncScreensRecords(); QCOMPARE(h.screens().size(), 1); // once
        // A v1/no-request client is unchanged: nothing is pushed for an attempt it never made active.
        MappedHarness plain("[General]\n", twoBlock(), false, 2000);
        plain.host.clearPhysicalLease("test worker exit");
        QVERIFY(plain.screens().isEmpty());
    }

    // ---- OPT-060 M-4: `console-screens-view` ------------------------------------------------------------------------

    void noViewRecordForwardsEverySurface()
    {
        ViewHarness h;
        QVERIFY(h.base.host.m_topologyAvailable);
        h.frame(0, true); h.frame(1, true); h.frame(0, false); h.frame(1, false);
        QCOMPARE(h.ownerFrames.size(), 4); QCOMPARE(h.viewerFrames.size(), 4);
        QVERIFY(!h.base.client->visibleSurfaces); QVERIFY(h.base.client->framesDropped.isEmpty());
    }

    void viewFiltersOneClientsStreamAndGatesNewlyVisibleSurfacesOnAKeyFrame()
    {
        ViewHarness h;
        h.frame(0, true); h.frame(1, true); // both surfaces installed on both clients
        QCOMPARE(h.ownerFrames.size(), 2); QCOMPARE(h.viewerFrames.size(), 2);
        h.ownerFrames.clear(); h.viewerFrames.clear();
        // The owner shows only surface 1 (by output name). No earlier record: everything was flowing, so no gate.
        auto reply = h.view(h.base.client, QJsonArray{u"Virtual-krdp-h1-1920x1080"_s}, u"w1"_s);
        QVERIFY(reply.value(u"ok"_s).toBool()); QCOMPARE(reply.value(u"requestId"_s).toString(), u"w1"_s);
        QCOMPARE(reply.value(u"surfaces"_s).toArray(), QJsonArray{1});
        QVERIFY(h.base.client->awaitingKeyFrame.isEmpty());
        h.frame(0, false); h.frame(1, false); h.frame(0, true);
        QCOMPARE(h.ownerFrames.size(), 1); // only surface 1's delta; surface 0 (even its key frame) is not forwarded
        QCOMPARE(h.viewerFrames.size(), 3); // the viewer never told us anything: independent, everything flows
        QCOMPARE(h.base.client->framesForwarded.value(1), quint64(1)); QCOMPARE(h.base.client->framesDropped.value(0), quint64(2));
        h.ownerFrames.clear(); h.viewerFrames.clear();
        // Switch to surface 0 (by host connector, via a bare output name, then by index): it missed frames, so it waits for
        // a key frame and the broker asks the worker for one.
        reply = h.view(h.base.client, QJsonArray{u"krdp-h0-1920x1080"_s}, u"w2"_s);
        QCOMPARE(reply.value(u"surfaces"_s).toArray(), QJsonArray{0});
        QCOMPARE(h.base.client->awaitingKeyFrame, QSet<int>{0});
        h.frame(1, true); h.frame(0, false);
        QCOMPARE(h.ownerFrames.size(), 0); // 1 is hidden now, 0 waits for its key frame
        h.frame(0, true); QCOMPARE(h.ownerFrames.size(), 1);
        QVERIFY(h.base.client->awaitingKeyFrame.isEmpty());
        h.frame(0, false); QCOMPARE(h.ownerFrames.size(), 2);
        // The viewer filters independently and a viewer's record never touches the owner.
        reply = h.view(h.viewer, QJsonArray{0, 1}, u"w3"_s);
        QVERIFY(reply.value(u"ok"_s).toBool()); QCOMPARE(reply.value(u"surfaces"_s).toArray(), (QJsonArray{0, 1}));
        h.ownerFrames.clear(); h.viewerFrames.clear();
        h.frame(1, false); QCOMPARE(h.ownerFrames.size(), 0); QCOMPARE(h.viewerFrames.size(), 1);
        reply = h.view(h.viewer, QJsonArray{1}, u"w4"_s); // the viewer drops surface 0 only for itself
        h.frame(0, false); h.frame(1, false);
        QCOMPARE(h.ownerFrames.size(), 1); QCOMPARE(h.viewerFrames.size(), 2);
    }

    void viewByHostConnectorAndFailOpenRules()
    {
        ViewHarness h;
        h.frame(0, true); h.frame(1, true);
        QVERIFY(h.base.askMapped(oneClient(100), u"hv"_s).value(u"ok"_s).toBool()); // gives the broker the plan (DP-1, DP-2)
        h.base.host.resolveVisibleSurfaces(*h.base.client);
        // Host connector names resolve through the mapped plan (h0 = DP-1, h1 = DP-2): outputs here are named after it.
        auto reply = h.view(h.base.client, QJsonArray{u"DP-2"_s}, u"n1"_s);
        QVERIFY(reply.value(u"ok"_s).toBool()); QCOMPARE(reply.value(u"surfaces"_s).toArray(), QJsonArray{1});
        // Names that match nothing never blank the picture: forward everything.
        reply = h.view(h.base.client, QJsonArray{u"nope"_s}, u"n2"_s);
        QVERIFY(reply.value(u"ok"_s).toBool()); QVERIFY(!h.base.client->visibleSurfaces);
        QCOMPARE(reply.value(u"surfaces"_s).toArray(), (QJsonArray{0, 1}));
        h.ownerFrames.clear(); h.frame(0, false); h.frame(1, false); QCOMPARE(h.ownerFrames.size(), 2);
        // An explicit empty list is valid: this client shows nothing now.
        reply = h.view(h.base.client, QJsonArray{}, u"n3"_s);
        QVERIFY(reply.value(u"ok"_s).toBool()); QVERIFY(h.base.client->visibleSurfaces); QVERIFY(h.base.client->visibleSurfaces->isEmpty());
        h.ownerFrames.clear(); h.frame(0, true); h.frame(1, true); QCOMPARE(h.ownerFrames.size(), 0);
        // Out-of-range indices are ignored; a malformed record is refused with its requestId echoed.
        reply = h.view(h.base.client, QJsonArray{0, 9}, u"n4"_s);
        QCOMPARE(reply.value(u"surfaces"_s).toArray(), QJsonArray{0});
        QJsonObject bad{{u"type"_s, u"console-screens-view"_s}, {u"v"_s, 1}, {u"visible"_s, u"x"_s}, {u"requestId"_s, u"n5"_s}};
        h.base.host.onControlRecord(&h.base.connection, h.base.client->id, bad);
        QVERIFY(!h.base.sent.last().value(u"ok"_s).toBool()); QCOMPARE(h.base.sent.last().value(u"requestId"_s).toString(), u"n5"_s);
    }

    void viewDoesNotFilterSingleSurfaceOrWorkspaceCaptureAndIsResetByANewCapture()
    {
        ViewHarness h;
        h.frame(0, true); h.frame(1, true);
        h.view(h.base.client, QJsonArray{1}, u"z1"_s);
        // One-surface frames are never filtered.
        VideoFrame single; single.size = QSize(1920, 1080); single.isKeyFrame = true; single.monitors = {{QRect(0, 0, 1920, 1080), true}};
        h.base.host.m_hostScreens.clear();
        QVERIFY(h.base.client->visibleSurfaces);
        QVERIFY(h.base.host.forwardsFrameTo(*h.base.client, single));
        // A changed output set (a new worker's capture) re-resolves the view against the new indices and clears the gates.
        h.view(h.base.client, QJsonArray{0}, u"z2"_s);
        QVERIFY(!h.base.client->awaitingKeyFrame.isEmpty());
        Q_EMIT h.base.host.m_endpoint.outputsReceived({{{u"Virtual-krdp-h0-1920x1080"_s, QRect(0, 0, 1920, 1080), 1, true}}, QPoint(2560, 0)});
        QVERIFY(h.base.client->awaitingKeyFrame.isEmpty());
        QVERIFY(h.base.client->visibleSurfaces); QCOMPARE(*h.base.client->visibleSurfaces, QSet<int>{0});
        // Not admitted: refused.
        MappedHarness lone("[General]\n", {}, true, 2000, 2, ConsoleSeat::Adapter::PhysicalUser, true, false);
        QJsonObject record{{u"type"_s, u"console-screens-view"_s}, {u"v"_s, 1}, {u"visible"_s, QJsonArray{}}, {u"requestId"_s, u"z3"_s}};
        lone.host.onControlRecord(&lone.connection, lone.client->id, record);
        QVERIFY(!lone.sent.last().value(u"ok"_s).toBool());
    }

    void workspaceCaptureIgnoresTheView()
    {
        ViewHarness h;
        h.base.client->codec->setCapturePolicy({MonitorCapturePolicy::Mode::Workspace, 0});
        h.view(h.base.client, QJsonArray{1}, u"ws"_s);
        h.ownerFrames.clear();
        VideoFrame f; f.size = QSize(3840, 1080); f.isKeyFrame = true; f.monitorIndex = 0;
        f.monitors = {{QRect(0, 0, 1920, 1080), true}, {QRect(1920, 0, 1920, 1080), false}};
        // Workspace capture is one aggregate surface (no per-output filter): the frame passes the filter step.
        QVERIFY(h.base.client->visibleSurfaces);
        QVERIFY(!h.base.client->visibleSurfaces->contains(0));
        const bool workspace = h.base.host.capturePolicy().mode == MonitorCapturePolicy::Mode::Workspace;
        QVERIFY(workspace || h.base.host.forwardsFrameTo(*h.base.client, f) == false);
    }

    // OPT-060 D2: after a mid-connection worker replacement (Replace over, plain capture again) the client still holds the
    // two RDPGFX surfaces of the ended Replace. The new worker's single keyframe was held back for as long as no topology
    // existed, so the client saw no picture until something asked the worker for one (about 15 s on Sol).
    void workerReplacementAfterReplaceResetsTheClientSurfacesPromptly()
    {
        RequestHarness h("[General]\n", false);
        auto &host = h.host; auto &client = *h.client;
        host.m_inputEnabled = true; client.session->setWorkerActive(true);
        const QVector<VideoMonitor> pair{{QRect(0, 0, 1920, 1080), true}, {QRect(1920, 0, 1920, 1080), false}};
        client.wireLayout = pair; // installed during the Replace
        // The replaced worker is gone: the broker forgets its outputs and topology (startWorker / setWorkerActive(false)).
        host.setWorkerActive(false);
        host.m_outputs = {};
        host.setWorkerActive(true); host.m_inputEnabled = true; client.session->setWorkerActive(true);
        // The replacement captures the host's one restored screen as plain workspace capture.
        Q_EMIT host.m_endpoint.outputsReceived({{{QStringLiteral("DP-3"), QRect(0, 0, 1920, 1080), 1, true}}});
        // Its topology is read back like any multi-to-single return, instead of waiting for someone else's query.
        QVERIFY(host.m_layoutAwaitingReadback);
        Q_EMIT host.m_endpoint.topologyReceived({{{QStringLiteral("DP-3"), QSize(1920, 1080), QRect(0, 0, 1920, 1080), 1, true, 1, true}}});
        QVERIFY(host.m_topologyAvailable); QVERIFY(!host.m_layoutAwaitingReadback);
        VideoFrame frame; frame.size = QSize(1920, 1080); frame.isKeyFrame = true; frame.monitors = {{QRect(0, 0, 1920, 1080), true}};
        Q_EMIT host.m_endpoint.frameReceived(frame);
        QVERIFY(client.wireLayout.isEmpty()); // one surface again, the picture is not held back
    }

    // OPT-060 D3: the Console worker exits after a configured release and the next connection finds a fresh one.
    // A connection that arrives while there is no worker is admitted, waits, and gets a fresh Replace attempt when the
    // new worker is ready; nothing of the previous connection's spent attempt carries over.
    void reconnectWhileTheWorkerIsBeingReplacedGetsItsOwnReplaceAttempt()
    {
        QTemporaryDir runtime; QVERIFY(runtime.isValid());
        Server server; RdpConnection first(&server, -1), second(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.setUidResolver([](RdpConnection *) { return std::optional<quint32>(1000); });
        host.setUserSettingsReader([](quint32) { return BrokerUserSettings::parse("[General]\n"); });
        host.setDisplayInfoProvider([](RdpConnection *) { return monitorBlock(true); });
        QVERIFY(host.m_endpoint.listen(runtime.filePath(QStringLiteral("worker-1.sock")),
            {ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000}, QByteArray(32, 'x')));
        host.addClient(&first);
        auto *a = host.m_clients.front().get();
        a->uid = 1000; host.loadUserSettings(*a); host.m_control.admit(a->id); host.syncControlState();
        QVERIFY(a->replaceAttempted);
        // The connection ends and the worker exits after releasing the outputs (D3: by design, a replacement proves capture).
        host.clearPhysicalLease("worker exited");
        QVERIFY(a->replaceSpent);
        host.removeClient(&first); // the first connection is gone
        host.m_endpoint.close();
        // A new connection arrives with no worker listening yet (the window is about 3 s after the fix of D1, 10 s before it).
        host.addClient(&second);
        auto *b = host.m_clients.back().get();
        b->uid = 1000; host.loadUserSettings(*b);
        QVERIFY(b->codec->consoleVirtualPolicy().enabled); QVERIFY(!b->replaceAttempted);
        host.m_control.admit(b->id); host.syncControlState();
        QVERIFY(!host.m_configuredConsoleOutputs); // no worker: nothing to arm yet, and nothing breaks
        // The replacement worker becomes ready: the same arming a first connection gets.
        QVERIFY(host.m_endpoint.listen(runtime.filePath(QStringLiteral("worker-2.sock")),
            {ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000}, QByteArray(32, 'y')));
        host.armConfiguredConsoleOutputs();
        QVERIFY(host.m_configuredConsoleOutputs); QVERIFY(b->replaceAttempted); QVERIFY(!b->replaceSpent);
        QVERIFY(host.m_clients.size() == 1); // only the new connection exists; it never inherited the old attempt
    }

    // OPT-060: `console-screens` carries why it ended (contract (h)).
    void consoleScreensEndReason_data()
    {
        QTest::addColumn<QString>("how");
        QTest::addColumn<QString>("reason");
        QTest::newRow("restore request") << u"restore"_s << u"restoreRequest"_s;
        QTest::newRow("desk input") << u"desk"_s << u"deskInput"_s;
        QTest::newRow("worker exit") << u"worker"_s << u"workerExit"_s;
        QTest::newRow("disconnect") << u"disconnect"_s << u"disconnect"_s;
    }
    void consoleScreensEndReason()
    {
        QFETCH(QString, how);
        QFETCH(QString, reason);
        QTemporaryDir runtime; QVERIFY(runtime.isValid());
        Server server; RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        QList<QJsonObject> sent;
        host.m_recordSent = [&](RdpConnection *, const QJsonObject &record) { sent.append(record); };
        host.setUidResolver([](RdpConnection *) { return std::optional<quint32>(1000); });
        host.setUserSettingsReader([](quint32) { return BrokerUserSettings::parse("[General]\n"); });
        host.setDisplayInfoProvider([](RdpConnection *) { return monitorBlock(true); });
        QVERIFY(host.m_endpoint.listen(runtime.filePath(QStringLiteral("worker.sock")),
            {ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000}, QByteArray(32, 'x')));
        host.addClient(&connection);
        auto &client = *host.m_clients.front();
        const auto clientId = client.id;
        client.uid = 1000;
        host.loadUserSettings(client);
        client.capabilitiesSent = client.screensAdvertised = true;
        const auto screens = [&] {
            QList<QJsonObject> result;
            for (const auto &record : sent) if (record.value(u"type"_s).toString() == u"console-screens"_s) result.append(record);
            return result;
        };
        host.m_control.admit(client.id); host.syncControlState();
        host.m_inputEnabled = true; client.session->setWorkerActive(true);
        Q_EMIT host.m_endpoint.outputsReceived({{{QStringLiteral("Virtual-owned-0"), QRect(0, 0, 1280, 720), 1, true}}, QPoint(2560, 0)});
        Q_EMIT host.m_endpoint.topologyReceived({{{QStringLiteral("Virtual-owned-0"), QSize(1280, 720), QRect(2560, 0, 1280, 720), 1, true, 1, false}}, false});
        host.syncScreensRecords();
        QCOMPARE(screens().size(), 1);
        QVERIFY(screens().last().value(u"active"_s).toBool());
        QCOMPARE(screens().last().value(u"reason"_s).toString(), u"connect"_s);
        if (how == u"restore"_s) {
            host.onControlRecord(&connection, clientId, QJsonObject{{u"type"_s, u"console-screens-restore"_s}, {u"v"_s, 1}, {u"requestId"_s, u"r"_s}});
        } else if (how == u"desk"_s) {
            host.physicalInputActivity();
        } else if (how == u"worker"_s) {
            host.clearPhysicalLease("test worker exit");
        } else {
            host.removeClient(&connection);
        }
        QCOMPARE(screens().size(), 2);
        QVERIFY(!screens().last().value(u"active"_s).toBool());
        QCOMPARE(screens().last().value(u"reason"_s).toString(), reason);
    }

    void consoleVirtualPolicyIsIdentityAndControllerScoped()
    {
        Server server;
        RdpConnection first(&server, -1), second(&server, -1);
        ConsoleHostController host(&server, {}, {});
        std::optional<quint32> firstUid;
        host.setUidResolver([&](RdpConnection *connection) {
            return connection == &first ? firstUid : std::optional<quint32>(1001);
        });
        int reads = 0;
        host.setUserSettingsReader([&](quint32 uid) {
            ++reads;
            return BrokerUserSettings::parse(uid == 1000
                ? "[General]\nMonitorMode=virtual\nVirtualMonitorPolicy=extend\nVirtualMonitorLayout=physical\nVirtualMonitorFallbackSize=1600x900\n"
                : "[General]\nMonitorMode=virtual\nVirtualMonitorPolicy=replace\nVirtualMonitorLayout=single\nVirtualMonitorFallbackSize=1280x720\n");
        });
        // OPT-060: the standard monitor block (one entry is enough) is the per-connection request.
        host.setDisplayInfoProvider([](RdpConnection *) {
            ClientDisplay::Info info; info.monitors = {{QRect(0, 0, 1920, 1080), true}}; return info;
        });
        host.addClient(&first); host.addClient(&second);
        auto &a = *host.m_clients[0]; auto &b = *host.m_clients[1];
        host.updateClientDisplayPolicy(a);
        QVERIFY(!a.codec->consoleVirtualPolicy().enabled); QCOMPARE(reads, 0);
        firstUid = 1000; a.uid = firstUid; b.uid = 1001;
        host.loadUserSettings(a); host.loadUserSettings(b); QCOMPARE(reads, 2);
        QVERIFY(a.codec->consoleVirtualPolicy().enabled);
        QCOMPARE(a.codec->consoleVirtualPolicy().policy, ConsoleVirtualOutputPolicy::Policy::Extend);
        QCOMPARE(a.codec->consoleVirtualPolicy().layout, ConsoleVirtualOutputPolicy::Layout::Physical);
        QCOMPARE(a.codec->consoleVirtualPolicy().client.desktopSize, QSize(1600, 900));
        QCOMPARE(b.codec->consoleVirtualPolicy().client.desktopSize, QSize(1280, 720));
        host.m_control.admit(a.id); host.m_control.admit(b.id); host.syncControlState();
        QVERIFY(a.codec->bound()); QVERIFY(!b.codec->bound());
        QCOMPARE(a.codec->config()->consoleVirtual, a.codec->consoleVirtualPolicy());
        const auto formerGeneration = a.codec->config()->generation;
        const auto saved = a.codec->consoleVirtualPolicy();
        firstUid = 1002; a.preferences.virtualMonitorFallbackSize = QSize(1920, 1080);
        host.updateClientDisplayPolicy(a); QCOMPARE(a.codec->consoleVirtualPolicy(), saved); // Cannot read/apply another identity.
        firstUid = 1000;
        a.preferences.virtualMonitorFallbackSize = QSize(0, 0);
        host.updateClientDisplayPolicy(a); QCOMPARE(a.codec->consoleVirtualPolicy(), saved); // Invalid changes are atomic.
        QVERIFY(host.m_control.release(a.id)); host.syncControlState();
        QVERIFY(host.m_control.acquire(b.id)); host.syncControlState();
        QVERIFY(!a.codec->bound()); QVERIFY(b.codec->bound());
        QCOMPARE(b.codec->config()->consoleVirtual, b.codec->consoleVirtualPolicy());
        QVERIFY(b.codec->config()->generation > formerGeneration);
        QCOMPARE(b.codec->config()->consoleVirtual.layout, ConsoleVirtualOutputPolicy::Layout::Single);
        QCOMPARE(reads, 2);
    }

    void savedAvcPreferenceAppliesOnlyToTheSoleAdmittedController()
    {
        Server server;
        RdpConnection first(&server, -1), second(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.m_sessions = {{QStringLiteral("3"), QStringLiteral("westers"), QStringLiteral("seat0"),
            QStringLiteral("wayland"), QStringLiteral("user"), QStringLiteral("active"), true, 1000}};
        host.addClient(&first); host.addClient(&second);
        auto &a = *host.m_clients[0]; auto &b = *host.m_clients[1];
        a.uid = b.uid = 1000;
        a.preferences.codec = CodecPreference::Avc444;
        host.m_control.admit(a.id);
        host.syncControlState();
        QCOMPARE(first.videoStream()->codecPreference(), CodecPreference::Avc444);
        QCOMPARE(first.videoStream()->codecForSessions(), VideoCodec::Avc420); // No worker probe yet.
        first.videoStream()->setAvc444Available(true);
        QCOMPARE(first.videoStream()->codecForSessions(), VideoCodec::Avc444v2);
        host.m_control.admit(b.id);
        host.syncCodecPolicy();
        QCOMPARE(first.videoStream()->codecPreference(), CodecPreference::Avc420);
        QCOMPARE(first.videoStream()->codecForSessions(), VideoCodec::Avc420);
        QCOMPARE(second.videoStream()->codecForSessions(), VideoCodec::Avc420);
        host.m_control.remove(b.id);
        host.syncCodecPolicy();
        QCOMPARE(first.videoStream()->codecPreference(), CodecPreference::Avc444);
        QCOMPARE(first.videoStream()->codecForSessions(), VideoCodec::Avc444v2);
        a.preferences.codec = CodecPreference::Avc420;
        host.syncCodecPolicy();
        QCOMPARE(first.videoStream()->codecForSessions(), VideoCodec::Avc420);
        QVERIFY(host.m_control.release(a.id));
        host.syncControlState();
        QCOMPARE(first.videoStream()->codecPreference(), CodecPreference::Avc420);
        a.preferences.codec = CodecPreference::Avc444;
        QVERIFY(host.m_control.acquire(a.id));
        host.syncControlState();
        QCOMPARE(first.videoStream()->codecPreference(), CodecPreference::Avc444);
        QCOMPARE(first.videoStream()->codecForSessions(), VideoCodec::Avc420); // New binding needs a probe.
        a.preferences.codec = CodecPreference::Avc444;
        a.uid = 1001; // Prior admission does not retain another user's unlocked desktop grant.
        host.syncCodecPolicy();
        QCOMPARE(first.videoStream()->codecPreference(), CodecPreference::Avc420);
    }

    void chromaPolicyIsControllerScopedAndResetsAfterTransfer()
    {
        Server server;
        RdpConnection first(&server, -1), second(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.m_sessions = {{QStringLiteral("3"), QStringLiteral("westers"), QStringLiteral("seat0"),
            QStringLiteral("wayland"), QStringLiteral("user"), QStringLiteral("active"), true, 1000}};
        host.addClient(&first);
        host.addClient(&second);
        auto &a = *host.m_clients[0];
        auto &b = *host.m_clients[1];
        a.uid = b.uid = 1000;
        a.preferences.chroma = ChromaPolicy{200, 300, 1200};
        b.preferences.chroma = ChromaPolicy{250, 400, 1500};
        host.m_control.admit(a.id);
        host.m_control.admit(b.id);
        host.syncControlState();
        QJsonObject reply;
        host.m_recordSent = [&](auto *, const QJsonObject &record) { reply = record; };
        const QJsonObject request{{QStringLiteral("type"), QStringLiteral("chroma")}, {QStringLiteral("v"), 1},
            {QStringLiteral("motionGapMs"), 280}};
        host.onControlRecord(&second, b.id, request);
        QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("not-owner"));
        host.onControlRecord(&first, a.id, request);
        QVERIFY(reply.value(QStringLiteral("ok")).toBool());
        QCOMPARE(a.codec->chromaPolicy().motionGapMs, 280);
        QCOMPARE(a.codec->chromaPolicy().restMs, 300);
        auto invalid = request;
        invalid[QStringLiteral("motionGapMs")] = 350;
        host.onControlRecord(&first, a.id, invalid);
        QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("invalid"));
        QCOMPARE(a.codec->chromaPolicy().motionGapMs, 280);
        QVERIFY(host.m_control.release(a.id));
        host.syncControlState();
        QVERIFY(host.m_control.acquire(b.id));
        host.syncControlState();
        QCOMPARE(a.codec->chromaPolicy(), *a.preferences.chroma);
        QCOMPARE(b.codec->chromaPolicy(), *b.preferences.chroma);
        b.uid = 1001; // A no-longer-admissible identity cannot retain its old grant.
        host.onControlRecord(&second, b.id, request);
        QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("not-owner"));
        QCOMPARE(b.codec->chromaPolicy(), *b.preferences.chroma);
    }

    void displayPolicyTracksAdmittedViewersAcrossControlTransfer()
    {
        Server server;
        RdpConnection first(&server, -1), second(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.m_sessions = {{QStringLiteral("3"), QStringLiteral("westers"), QStringLiteral("seat0"),
            QStringLiteral("wayland"), QStringLiteral("user"), QStringLiteral("active"), true, 1000}};
        host.addClient(&first);
        host.addClient(&second);
        auto &owner = *host.m_clients[0];
        auto &viewer = *host.m_clients[1];
        owner.uid = viewer.uid = 1000;
        first.videoStream()->setEnabled(true);
        second.videoStream()->setEnabled(true);
        QVERIFY(!host.displayPolicy().active); // Streaming alone conveys no admission.
        host.m_control.admit(owner.id);
        host.m_control.admit(viewer.id);
        owner.preferences.wakeDisplayOnConnect = false;
        viewer.preferences.wakeDisplayOnConnect = true;
        QVERIFY(host.displayPolicy().active && host.displayPolicy().wakeEnabled);
        QVERIFY(host.m_control.acquire(owner.id));
        QVERIFY(host.m_control.release(owner.id));
        QVERIFY(host.m_control.acquire(viewer.id));
        QVERIFY(host.displayPolicy().wakeEnabled); // Input ownership does not remove viewer demand.
        second.videoStream()->setEnabled(false);
        QVERIFY(host.displayPolicy().active && !host.displayPolicy().wakeEnabled);
        first.videoStream()->setEnabled(false);
        QVERIFY(!host.displayPolicy().active);
        second.videoStream()->setEnabled(true);
        viewer.uid = 1001; // A different account on the unlocked seat is inadmissible.
        QVERIFY(!host.displayPolicy().active);
    }

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
                ? "[General]\nQuality=43\nAdaptiveQuality=false\nPreferAudioQuality=true\nAv1Tiles=8\nSoftwareEncoding=prefer\nAvc444MotionGapMs=200\nAvc444RestMs=300\nAvc444MaxGapMs=1200\nMonitorMode=specific\nMonitorIndex=1\n"
                : "[General]\nQuality=64\nPreferAudioQuality=false\nAv1Tiles=2\nSoftwareEncoding=never\nMonitorMode=workspace\n");
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
        QCOMPARE(a.codec->chromaPolicy(), (ChromaPolicy{200, 300, 1200}));
        QCOMPARE(second.videoStream()->softwareEncoding(), CodecPolicy::SoftwareEncoding::Never);
        QCOMPARE(a.codec->capturePolicy(), (MonitorCapturePolicy{MonitorCapturePolicy::Mode::Specific, 1}));
        QCOMPARE(b.codec->capturePolicy(), (MonitorCapturePolicy{MonitorCapturePolicy::Mode::Workspace, 0}));
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

    void selectedCaptureWaitsForProjectionAndCannotGrantLayoutWrites()
    {
        Server server; RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.addClient(&connection);
        auto &client = *host.m_clients.front();
        host.m_control.admit(client.id); host.syncControlState();
        host.m_inputEnabled = true; client.session->setWorkerActive(true);
        host.m_experimentalPhysicalTopology = host.m_experimentalConsoleVirtual = true;
        client.codec->setCapturePolicy({MonitorCapturePolicy::Mode::Specific, 1});
        QSignalSpy received(client.session.get(), &AbstractSession::frameReceived);
        Q_EMIT host.m_endpoint.outputsReceived({{{QStringLiteral("DP-2"), QRect(0, 0, 1280, 720), 1.5, true}}, QPoint(-1280, 100)});
        QVERIFY(host.m_layoutAwaitingReadback);
        VideoFrame frame; frame.size = QSize(1920, 1080); frame.isKeyFrame = true;
        frame.monitors = {{QRect(0, 0, 1920, 1080), true}};
        Q_EMIT host.m_endpoint.frameReceived(frame);
        QVERIFY(received.isEmpty());
        ConsoleWorkerWire::Topology projection;
        projection.complete = false;
        projection.outputs = {{QStringLiteral("DP-2"), frame.size, QRect(-1280, 100, 1280, 720), 1.5, true, 1, true}};
        Q_EMIT host.m_endpoint.topologyReceived(projection);
        QVERIFY(!host.m_layoutAwaitingReadback);
        QVERIFY(host.m_topologyAvailable); QVERIFY(!host.m_topologyComplete);
        Q_EMIT host.m_endpoint.frameReceived(frame);
        QCOMPARE(received.size(), 1);
        QVERIFY(client.wireLayout.isEmpty()); // One surface for this selected screen.
        const auto caps = host.consoleTopology(QStringLiteral("projection"), client.id).value(QStringLiteral("capabilities")).toObject();
        QVERIFY(!caps.value(QStringLiteral("add")).toBool());
        QVERIFY(!caps.value(QStringLiteral("remove")).toBool());
        QVERIFY(!caps.value(QStringLiteral("consoleVirtual")).toBool());
        auto wrong = frame; wrong.size.rwidth() -= 2;
        Q_EMIT host.m_endpoint.frameReceived(wrong);
        QCOMPARE(received.size(), 1);
    }

    void workspaceCaptureKeepsOneAggregateSurface()
    {
        Server server; RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        host.addClient(&connection);
        auto &client = *host.m_clients.front();
        host.m_control.admit(client.id); host.syncControlState();
        host.m_inputEnabled = true; client.session->setWorkerActive(true);
        client.codec->setCapturePolicy({MonitorCapturePolicy::Mode::Workspace, 0});
        const QVector<VideoMonitor> monitors{{QRect(0, 0, 1280, 720), true}, {QRect(1280, 0, 1280, 720), false}};
        client.wireLayout = monitors; // Previous independent-screen ownership period.
        Q_EMIT host.m_endpoint.outputsReceived({{
            {QStringLiteral("DP-1"), monitors[0].geometry, 1, true},
            {QStringLiteral("DP-2"), monitors[1].geometry, 1, false}}, QPoint(100, 50)});
        Q_EMIT host.m_endpoint.topologyReceived({{
            {QStringLiteral("DP-1"), QSize(1280, 720), QRect(100, 50, 1280, 720), 1, true, 1, true},
            {QStringLiteral("DP-2"), QSize(1280, 720), QRect(1380, 50, 1280, 720), 1, false, 2, true}}});
        QSignalSpy received(client.session.get(), &AbstractSession::frameReceived);
        VideoFrame frame; frame.size = QSize(2560, 720); frame.monitors = monitors; frame.isKeyFrame = true;
        Q_EMIT host.m_endpoint.frameReceived(frame);
        QCOMPARE(received.size(), 1);
        QVERIFY(client.wireLayout.isEmpty()); // Never partition an aggregate encoded payload.
        auto wrong = frame; wrong.monitors[1].geometry.moveLeft(1281);
        Q_EMIT host.m_endpoint.frameReceived(wrong);
        QCOMPARE(received.size(), 1);
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
        QVERIFY(!host.consoleTopology(QStringLiteral("query"), id).value(QStringLiteral("capabilities")).toObject()
            .value(QStringLiteral("add")).toBool()); // Physical-only opt-in does not expose new Console virtual writes.
        host.m_experimentalConsoleVirtual = true;
        QVERIFY(host.consoleTopology(QStringLiteral("query"), id).value(QStringLiteral("capabilities")).toObject()
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
        QVERIFY(host.consoleTopology(QStringLiteral("query"), id).value(QStringLiteral("capabilities")).toObject()
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
        server.setCameraLoopbackDevice(QStringLiteral("/dev/video10")); // only counts where a loopback bridge exists
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
        QCOMPARE(last(&owner).value(QStringLiteral("state")).toString(), QStringLiteral("error"));
        QCOMPARE(last(&owner).value(QStringLiteral("code")).toString(), QStringLiteral("needs-session"));
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
        QCOMPARE(last(&owner).value(QStringLiteral("code")).toString(), QStringLiteral("needs-session"));
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

    // --- OPT-058: device availability follows the Console session ---
    void greeterRefusesCameraAndMicrophoneWithNeedsSession()
    {
        Server server;
        const auto bridge = QStringLiteral("/dev/video10");
        const bool haveBridge = CameraAvailability::reason(bridge).isEmpty(); // the camera pre-check needs a real loopback
        server.setCameraLoopbackDevice(bridge);
        RdpConnection owner(&server, -1);
        ConsoleHostController host(&server, {}, {});
        QList<QJsonObject> sent;
        host.m_recordSent = [&sent](RdpConnection *, const QJsonObject &record) { sent.append(record); };
        host.addClient(&owner);
        const auto id = host.m_clients.front()->id;
        host.m_control.admit(id);
        host.syncControlState();
        host.m_deviceSessionForTest = false; // greeter: no PhysicalUser worker
        host.m_clients.front()->externalMicrophone = true;
        host.onControlRecord(&owner, id, deviceRecord(QStringLiteral("m1"), QStringLiteral("microphone"), QStringLiteral("on")));
        QCOMPARE(sent.last().value(QStringLiteral("state")).toString(), QStringLiteral("error"));
        QCOMPARE(sent.last().value(QStringLiteral("code")).toString(), QStringLiteral("needs-session"));
        QCOMPARE(sent.last().value(QStringLiteral("requestId")).toString(), QStringLiteral("m1"));
        QCOMPARE(host.m_microphoneClient, quint64(0));
        host.m_deviceSessionForTest = true;
        host.m_clients.front()->externalMicrophone = false; // a client with no audio input channel: genuine `unavailable`
        host.onControlRecord(&owner, id, deviceRecord(QStringLiteral("m2"), QStringLiteral("microphone"), QStringLiteral("on")));
        QCOMPARE(sent.last().value(QStringLiteral("code")).toString(), QStringLiteral("unavailable"));
        if (!haveBridge) QSKIP("no V4L2 loopback bridge on this host; the camera half needs /dev/video10");
        host.m_deviceSessionForTest = false;
        host.onControlRecord(&owner, id, deviceRecord(QStringLiteral("c1"), QStringLiteral("camera"), QStringLiteral("on")));
        QCOMPARE(sent.last().value(QStringLiteral("state")).toString(), QStringLiteral("error"));
        QCOMPARE(sent.last().value(QStringLiteral("code")).toString(), QStringLiteral("needs-session"));
        host.onControlRecord(&owner, id, deviceRecord(QStringLiteral("c2"), QStringLiteral("camera"), QStringLiteral("query")));
        QCOMPARE(sent.last().value(QStringLiteral("state")).toString(), QStringLiteral("off"));
        QCOMPARE(host.m_cameraClient, quint64(0));
        host.m_deviceSessionForTest = true;
        host.onControlRecord(&owner, id, deviceRecord(QStringLiteral("c3"), QStringLiteral("camera"), QStringLiteral("on")));
        QCOMPARE(sent.last().value(QStringLiteral("code")).toString(), QStringLiteral("unavailable"));
    }

    void availabilityIsPushedOncePerEdgeToKrdpctlClientsOnly()
    {
        Server server;
        RdpConnection krdpctl(&server, -1);
        RdpConnection stock(&server, -1);
        ConsoleHostController host(&server, {}, {});
        QList<std::pair<RdpConnection *, QJsonObject>> sent;
        host.m_recordSent = [&sent](RdpConnection *connection, const QJsonObject &record) { sent.append({connection, record}); };
        host.addClient(&krdpctl);
        host.addClient(&stock);
        host.m_clients.at(0)->capabilitiesSent = true; // received `capabilities`
        const auto count = [&sent](RdpConnection *connection) {
            int n = 0;
            for (const auto &entry : sent) n += entry.first == connection && entry.second.value(QStringLiteral("type")).toString() == u"device-availability";
            return n;
        };
        host.m_deviceSessionForTest = false;
        host.publishDeviceAvailability(); // still the greeter: no edge
        QCOMPARE(count(&krdpctl), 0);
        // T2: the PhysicalUser worker becomes ready.
        host.m_deviceSessionForTest = true;
        host.publishDeviceAvailability();
        host.publishDeviceAvailability(); // idempotent
        QCOMPARE(count(&krdpctl), 1);
        QCOMPARE(count(&stock), 0);
        const auto up = sent.last().second;
        QCOMPARE(up.value(QStringLiteral("v")).toInt(), 1);
        QVERIFY(up.value(QStringLiteral("camera")).toObject().value(QStringLiteral("available")).toBool());
        QVERIFY(up.value(QStringLiteral("microphone")).toObject().value(QStringLiteral("available")).toBool());
        QVERIFY(!up.contains(QStringLiteral("requestId")));
        // T3: the worker goes away.
        host.m_deviceSessionForTest = false;
        host.publishDeviceAvailability();
        QCOMPARE(count(&krdpctl), 2);
        const auto down = sent.last().second.value(QStringLiteral("camera")).toObject();
        QVERIFY(!down.value(QStringLiteral("available")).toBool());
        QCOMPARE(down.value(QStringLiteral("reason")).toString(), QStringLiteral("needs-session"));
    }

    void workerStopSendsCameraErrorBeforeUnavailable()
    {
        Server server;
        RdpConnection connection(&server, -1);
        ConsoleHostController host(&server, {}, {});
        QList<QJsonObject> sent;
        host.m_recordSent = [&sent](RdpConnection *, const QJsonObject &record) { sent.append(record); };
        host.addClient(&connection);
        const auto id = host.m_clients.front()->id;
        host.m_clients.front()->capabilitiesSent = true;
        host.m_control.admit(id);
        host.syncControlState();
        host.m_inputEnabled = true;
        host.m_deviceSessionForTest = true;
        host.publishDeviceAvailability();
        sent.clear();
        host.m_cameraClient = id;
        host.m_cameraReady = true;
        host.m_cameraPolicy = {host.m_controlGeneration, ++host.m_nextCameraId, true, {}};
        host.m_deviceSessionForTest = false;
        host.setWorkerActive(false);
        QCOMPARE(sent.size(), 2);
        QCOMPARE(sent.at(0).value(QStringLiteral("type")).toString(), QStringLiteral("device"));
        QCOMPARE(sent.at(0).value(QStringLiteral("device")).toString(), QStringLiteral("camera"));
        QCOMPARE(sent.at(1).value(QStringLiteral("type")).toString(), QStringLiteral("device-availability"));
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
