// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "ConsoleWorkerEndpoint.h"

using namespace KRdp;

class ConsoleWorkerEndpointTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void authenticatesThenForwardsFrames();
    void rejectsFramesBeforeCaptureReady();
    void rejectsTakeoverBeforeCaptureReady();
    void rejectsWrongWorkerToken();
    void displayDemandRequiresHelloButNotFirstCapture();
    void reportsWorkerFailures_data();
    void reportsWorkerFailures();
    void clipboardCrossesOnlyAfterReadyAndOnlyWhenAllowed();
    void clipboardIdsAreEndpointWideAndForgottenWithTheWorker();
    void clipboardRecordBeforeReadyFailsTheWorker();
    void clipboardImagesCrossWithTheirKindAndLimits();
    void hostPointerIsRelayedAfterReadyAndForgottenWithTheWorker();
    void hostPointerBeforeReadyOrMalformedFailsTheWorker();
};

void ConsoleWorkerEndpointTest::reportsWorkerFailures_data()
{
    QTest::addColumn<bool>("ready");
    QTest::addColumn<QByteArray>("reason");
    QTest::newRow("before-ready") << false << QByteArray("unsupported retained monitor arrangement");
    QTest::newRow("after-ready") << true << QByteArray("unsupported retained monitor arrangement");
    QTest::newRow("empty-after-ready") << true << QByteArray();
    QTest::newRow("bounded-after-ready") << true << QByteArray(512, 'x');
}

void ConsoleWorkerEndpointTest::reportsWorkerFailures()
{
    QFETCH(bool, ready);
    QFETCH(QByteArray, reason);
    QTemporaryDir directory;
    ConsoleWorkerEndpoint endpoint;
    const ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), quint32(getuid())};
    const QByteArray token(24, 'e');
    QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("error.sock")), target, token));
    QSignalSpy errors(&endpoint, &ConsoleWorkerEndpoint::protocolError);
    QLocalSocket worker;
    worker.connectToServer(endpoint.socketName());
    QVERIFY(worker.waitForConnected());
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{target.sessionId, target.uid, token}));
    worker.flush();
    QTRY_VERIFY(endpoint.authenticated());
    if (ready) {
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        worker.flush();
        QTRY_VERIFY(endpoint.ready());
    }
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Error, reason));
    worker.flush();
    QTRY_COMPARE(errors.size(), 1);
    const QString context = ready ? QStringLiteral("worker capture failed") : QStringLiteral("worker failed before confirming capture");
    QCOMPARE(errors.first().first().toString(), reason.isEmpty() ? context : context + QStringLiteral(": ") + QString::fromUtf8(reason.left(256)));
    QVERIFY(!endpoint.ready());
    QTRY_COMPARE(worker.state(), QLocalSocket::UnconnectedState);
}

void ConsoleWorkerEndpointTest::displayDemandRequiresHelloButNotFirstCapture()
{
    QTemporaryDir directory;
    ConsoleWorkerEndpoint endpoint;
    const ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), quint32(getuid())};
    const QByteArray token(24, 'd');
    QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("wake.sock")), target, token));
    QVERIFY(endpoint.setDisplayPolicy(true, true));
    QLocalSocket worker;
    worker.connectToServer(endpoint.socketName());
    QVERIFY(worker.waitForConnected());
    QTest::qWait(20);
    QCOMPARE(worker.bytesAvailable(), 0); // No pre-authentication policy or input grant.
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{target.sessionId, target.uid, token}));
    worker.flush();
    QTRY_VERIFY(endpoint.authenticated());
    QVERIFY(!endpoint.ready());
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    ConsoleWorkerWire::Deframer reader;
    reader.feed(worker.readAll());
    const auto first = reader.next();
    QVERIFY(first);
    const auto wake = ConsoleWorkerWire::displayPolicy(*first);
    QVERIFY(wake && wake->active && wake->wakeEnabled);
    endpoint.setControlState({99, true}); // Input remains gated before Ready.
    QVERIFY(endpoint.setDisplayPolicy(false, false));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    reader.feed(worker.readAll());
    const auto second = reader.next();
    QVERIFY(second);
    const auto release = ConsoleWorkerWire::displayPolicy(*second);
    QVERIFY(release && !release->active && !release->wakeEnabled);
    QVERIFY(release->revision > wake->revision);
    QVERIFY(!reader.next()); // In particular, no early ControlState.
    endpoint.stopWorker();
    QVERIFY(!endpoint.setDisplayPolicy(true, true));
}

void ConsoleWorkerEndpointTest::authenticatesThenForwardsFrames()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ConsoleWorkerEndpoint endpoint;
    const ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000};
    const QByteArray token(24, 't');
    QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), target, token));
    const ConsoleWorkerWire::Resize resize{11, 42, QStringLiteral("DP-1"), QSize(1280, 720), 1};
    QVERIFY(!endpoint.resize(resize)); // Never send a display mutation before Ready.
    const ConsoleWorkerWire::Position position{12, 42, QStringLiteral("Virtual-1"), QPoint(1280, 100)};
    QVERIFY(!endpoint.position(position));
    const ConsoleWorkerWire::PositionBatch batch{13, 42, {{QStringLiteral("Virtual-0"), QPoint(1280, 0)},
        {QStringLiteral("Virtual-1"), QPoint(0, 0)}}};
    QVERIFY(!endpoint.positionBatch(batch));
    const ConsoleWorkerWire::ManagedFit fit{14, 42, QStringLiteral("Virtual-0"), QSize(1600, 900), 1.25,
        {{QStringLiteral("Virtual-0"), QStringLiteral("Virtual-1"), 1, 0}}};
    QVERIFY(!endpoint.managedFit(fit));
    const ConsoleWorkerWire::Primary primary{15, 42, QStringLiteral("Virtual-1")};
    QVERIFY(!endpoint.primary(primary));
    const ConsoleWorkerWire::Mixed mixed{16, 42, {
        {ConsoleWorkerWire::MixedOperation::Kind::Move, QStringLiteral("Virtual-1"), QPoint(1280, 100), {}, 1},
        {ConsoleWorkerWire::MixedOperation::Kind::Primary, QStringLiteral("Virtual-1"), {}, {}, 1}}};
    QVERIFY(!endpoint.mixed(mixed));
    const ConsoleWorkerWire::MixedCreate mixedCreate{17, 42, QStringLiteral("Virtual-krdp-added-test"),
        QSize(960, 540), 1, QPoint(2560, 100), {
            {ConsoleWorkerWire::MixedOperation::Kind::Primary, QStringLiteral("Virtual-krdp-added-test"), {}, {}, 1}}};
    QVERIFY(!endpoint.mixedCreate(mixedCreate));
    const ConsoleWorkerWire::PhysicalLayout physical{18, 42, QStringLiteral("console-generation"), 1, true,
        {{QStringLiteral("DP-1"), QSize(1280, 720), QRect(0, 0, 1280, 720), 1, true, 1}},
        {{ConsoleWorkerWire::MixedOperation::Kind::Move, QStringLiteral("DP-1"), QPoint(-100, 0), {}, 1}}};
    QVERIFY(!endpoint.physicalLayout(physical));
    QVERIFY(!endpoint.requestTopology());

    int ready = 0;
    int frames = 0;
    int layouts = 0;
    int topologies = 0;
    quint64 takeoverGeneration = 0;
    connect(&endpoint, &ConsoleWorkerEndpoint::localTakeover, this, [&takeoverGeneration](quint64 generation) {
        takeoverGeneration = generation;
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [&layouts](const auto &outputs) {
        QCOMPARE(outputs.monitors.first().name, QStringLiteral("DP-1"));
        ++layouts;
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::topologyReceived, this, [&topologies](const auto &topology) {
        QCOMPARE(topology.outputs.first().name, QStringLiteral("DP-1"));
        ++topologies;
    });
    VideoFrame received;
    connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, this, [&ready](const auto &) { ++ready; });
    connect(&endpoint, &ConsoleWorkerEndpoint::frameReceived, this, [&frames, &received](const VideoFrame &frame) {
        ++frames;
        received = frame;
    });

    QLocalSocket worker;
    worker.connectToServer(endpoint.socketName());
    QVERIFY(worker.waitForConnected(1000));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), 1000, token}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTest::qWait(20);
    QCOMPARE(ready, 0);
    QVERIFY(!endpoint.ready());
    QVERIFY(!endpoint.setVideoQuality({42, 60}));
    QVERIFY(!endpoint.setMicrophone({42, 7, true}));
    QVERIFY(!endpoint.sendMicrophoneAudio({42, 7, QByteArray(4, 'a')}));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(ready, 1);

    endpoint.setControlState({42, true});
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    ConsoleWorkerWire::Deframer brokerMessages;
    brokerMessages.feed(worker.readAll());
    const auto controlRecord = brokerMessages.next();
    QVERIFY(controlRecord);
    const auto control = ConsoleWorkerWire::controlState(*controlRecord);
    QVERIFY(control);
    QCOMPARE(control->generation, quint64(42));
    QVERIFY(control->active);

    QVERIFY(endpoint.requestTopology());
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto topologyQuery = brokerMessages.next();
    QVERIFY(topologyQuery);
    QCOMPARE(topologyQuery->kind, ConsoleWorkerWire::Kind::TopologyQuery);
    QVERIFY(topologyQuery->payload.isEmpty());

    QSignalSpy positioned(&endpoint, &ConsoleWorkerEndpoint::positionFinished);
    QVERIFY(endpoint.position(position));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto positionRecord = brokerMessages.next();
    QVERIFY(positionRecord);
    QCOMPARE(ConsoleWorkerWire::position(*positionRecord), std::optional<ConsoleWorkerWire::Position>(position));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PositionResult{12, 42, {}}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(positioned.count(), 1);

    QSignalSpy batchPositioned(&endpoint, &ConsoleWorkerEndpoint::positionBatchFinished);
    QVERIFY(endpoint.positionBatch(batch));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto batchRecord = brokerMessages.next();
    QVERIFY(batchRecord);
    QCOMPARE(ConsoleWorkerWire::positionBatch(*batchRecord), std::optional<ConsoleWorkerWire::PositionBatch>(batch));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PositionBatchResult{13, 42, {}}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(batchPositioned.count(), 1);

    QSignalSpy fitted(&endpoint, &ConsoleWorkerEndpoint::managedFitFinished);
    QVERIFY(endpoint.managedFit(fit));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto fitRecord = brokerMessages.next();
    QVERIFY(fitRecord);
    QCOMPARE(ConsoleWorkerWire::managedFit(*fitRecord), std::optional<ConsoleWorkerWire::ManagedFit>(fit));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ManagedFitResult{14, 42, {}}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(fitted.count(), 1);

    QSignalSpy primaryChanged(&endpoint, &ConsoleWorkerEndpoint::primaryFinished);
    QVERIFY(endpoint.primary(primary));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto primaryRecord = brokerMessages.next();
    QVERIFY(primaryRecord);
    QCOMPARE(ConsoleWorkerWire::primary(*primaryRecord), std::optional<ConsoleWorkerWire::Primary>(primary));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PrimaryResult{15, 42, {}}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(primaryChanged.count(), 1);

    QSignalSpy mixedChanged(&endpoint, &ConsoleWorkerEndpoint::mixedFinished);
    QVERIFY(endpoint.mixed(mixed));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto mixedRecord = brokerMessages.next();
    QVERIFY(mixedRecord);
    QCOMPARE(ConsoleWorkerWire::mixed(*mixedRecord), std::optional<ConsoleWorkerWire::Mixed>(mixed));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::MixedResult{16, 42, {}}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(mixedChanged.count(), 1);

    QSignalSpy mixedCreateChanged(&endpoint, &ConsoleWorkerEndpoint::mixedCreateFinished);
    QVERIFY(endpoint.mixedCreate(mixedCreate));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto mixedCreateRecord = brokerMessages.next();
    QVERIFY(mixedCreateRecord);
    QCOMPARE(ConsoleWorkerWire::mixedCreate(*mixedCreateRecord),
        std::optional<ConsoleWorkerWire::MixedCreate>(mixedCreate));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::MixedCreateResult{17, 42, {}}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(mixedCreateChanged.count(), 1);

    QSignalSpy physicalChanged(&endpoint, &ConsoleWorkerEndpoint::physicalLayoutFinished);
    QVERIFY(endpoint.physicalLayout(physical));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto physicalRecord = brokerMessages.next();
    QVERIFY(physicalRecord);
    QCOMPARE(ConsoleWorkerWire::physicalLayout(*physicalRecord),
        std::optional<ConsoleWorkerWire::PhysicalLayout>(physical));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PhysicalLayoutResult{18, 42, QStringLiteral("unsupported")}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(physicalChanged.count(), 1);
    QSignalSpy leaseReleased(&endpoint, &ConsoleWorkerEndpoint::physicalLeaseReleased);
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::PhysicalLeaseReleased{42, true}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(leaseReleased.count(), 1);
    QCOMPARE(leaseReleased.takeFirst().at(0).value<ConsoleWorkerWire::PhysicalLeaseReleased>(),
        (ConsoleWorkerWire::PhysicalLeaseReleased{42, true}));

    QVERIFY(!endpoint.setVideoQuality({0, 60}));
    QVERIFY(!endpoint.setVideoQuality({42, 101}));
    QVERIFY(endpoint.setVideoQuality({42, 60}));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto qualityRecord = brokerMessages.next();
    QVERIFY(qualityRecord);
    QCOMPARE(ConsoleWorkerWire::videoQuality(*qualityRecord), std::optional<ConsoleWorkerWire::VideoQuality>({42, 60}));

    QVERIFY(endpoint.setMicrophone({42, 7, true}));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto micPolicy = brokerMessages.next();
    QVERIFY(micPolicy);
    QCOMPARE(ConsoleWorkerWire::microphonePolicy(*micPolicy), std::optional<ConsoleWorkerWire::MicrophonePolicy>({42, 7, true}));
    QVERIFY(!endpoint.sendMicrophoneAudio({42, 7, QByteArray(3, 'a')}));
    QVERIFY(endpoint.sendMicrophoneAudio({42, 7, QByteArray(4, 'a')}));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto micAudio = brokerMessages.next();
    QVERIFY(micAudio);
    QCOMPARE(ConsoleWorkerWire::microphoneAudio(*micAudio), std::optional<ConsoleWorkerWire::MicrophoneAudio>({42, 7, QByteArray(4, 'a')}));
    QSignalSpy micReplies(&endpoint, &ConsoleWorkerEndpoint::microphoneFinished);
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::MicrophoneResult{42, 7, {}}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(micReplies.count(), 1);

    QVERIFY(endpoint.resize(resize));
    QTRY_VERIFY(worker.bytesAvailable() > 0);
    brokerMessages.feed(worker.readAll());
    const auto resizeRecord = brokerMessages.next();
    QVERIFY(resizeRecord);
    QCOMPARE(ConsoleWorkerWire::resize(*resizeRecord), std::optional<ConsoleWorkerWire::Resize>(resize));
    QSignalSpy resizeReplies(&endpoint, &ConsoleWorkerEndpoint::resizeFinished);
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ResizeResult{11, 42, {}}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(resizeReplies.count(), 1);

    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Outputs{{{QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true}}}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(layouts, 1);
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Topology{{
        {QStringLiteral("DP-1"), QSize(1280, 720), QRect(0, 0, 1280, 720), 1.0, true, 1}}}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(topologies, 1);
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ControlState{42, true}, ConsoleWorkerWire::Kind::LocalTakeover));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(takeoverGeneration, quint64(42));
    quint64 hostChangeGeneration = 0;
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::hostScreensChanged, &endpoint, [&](quint64 generation) { hostChangeGeneration = generation; });
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ControlState{43, true}, ConsoleWorkerWire::Kind::HostScreensChanged));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(hostChangeGeneration, quint64(43)); // M-8: reaches the broker, distinct from a local takeover
    QCOMPARE(takeoverGeneration, quint64(42));

    VideoFrame sent;
    sent.size = QSize(1280, 720);
    sent.data = "frame";
    sent.isKeyFrame = true;
    worker.write(ConsoleWorkerWire::frame(sent));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(frames, 1);
    QCOMPARE(received.size, sent.size);
    QCOMPARE(received.data, sent.data);
    QVERIFY(received.isKeyFrame);
}

void ConsoleWorkerEndpointTest::rejectsFramesBeforeCaptureReady()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ConsoleWorkerEndpoint endpoint;
    const ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000};
    const QByteArray token(24, 't');
    QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), target, token));
    int errors = 0;
    connect(&endpoint, &ConsoleWorkerEndpoint::protocolError, this, [&errors](const QString &) { ++errors; });

    QLocalSocket worker;
    worker.connectToServer(endpoint.socketName());
    QVERIFY(worker.waitForConnected(1000));
    VideoFrame premature;
    premature.size = QSize(1, 1);
    premature.data = "not-ready";
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), 1000, token}) + ConsoleWorkerWire::frame(premature));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(errors, 1);
    QVERIFY(!endpoint.ready());
}

void ConsoleWorkerEndpointTest::rejectsTakeoverBeforeCaptureReady()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ConsoleWorkerEndpoint endpoint;
    const ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), 1000};
    const QByteArray token(24, 't');
    QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), target, token));
    QSignalSpy errors(&endpoint, &ConsoleWorkerEndpoint::protocolError);
    QSignalSpy takeovers(&endpoint, &ConsoleWorkerEndpoint::localTakeover);
    QLocalSocket worker;
    worker.connectToServer(endpoint.socketName());
    QVERIFY(worker.waitForConnected(1000));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), 1000, token})
                 + ConsoleWorkerWire::frame(ConsoleWorkerWire::ControlState{42, true}, ConsoleWorkerWire::Kind::LocalTakeover));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(errors.count(), 1);
    QCOMPARE(takeovers.count(), 0);
    QVERIFY(!endpoint.ready());
}

void ConsoleWorkerEndpointTest::rejectsWrongWorkerToken()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ConsoleWorkerEndpoint endpoint;
    // The peer uid must match (AUD-C-9) so this exercises the token check itself.
    const ConsoleHandoff::Target target{ConsoleSeat::Adapter::Greeter, QStringLiteral("1"), quint32(getuid())};
    QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), target, QByteArray(24, 't')));
    int errors = 0;
    connect(&endpoint, &ConsoleWorkerEndpoint::protocolError, this, [&errors](const QString &) { ++errors; });

    QLocalSocket worker;
    worker.connectToServer(endpoint.socketName());
    QVERIFY(worker.waitForConnected(1000));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("1"), quint32(getuid()), QByteArray(24, 'x')}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(errors, 1);
    QVERIFY(!endpoint.ready());
}


namespace
{
// A worker that said Hello (and Ready when asked) on a fresh endpoint.
struct ClipboardWorker {
    QTemporaryDir directory;
    ConsoleWorkerEndpoint endpoint;
    ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), quint32(getuid())};
    QByteArray token = QByteArray(24, 'c');
    QLocalSocket worker;
    ConsoleWorkerWire::Deframer reader;

    bool ok = false;
    void start(bool ready)
    {
        ok = false;
        if (!endpoint.listen(directory.filePath(QStringLiteral("clip.sock")), target, token)) return;
        worker.connectToServer(endpoint.socketName());
        if (!worker.waitForConnected(1000)) return;
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{target.sessionId, target.uid, token}));
        worker.flush();
        QTRY_VERIFY_WITH_TIMEOUT(endpoint.authenticated(), 3000);
        if (ready) {
            worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
            worker.flush();
            QTRY_VERIFY_WITH_TIMEOUT(endpoint.ready(), 3000);
        }
        ok = true;
    }
    QList<ConsoleWorkerWire::Record> drain()
    {
        QTest::qWait(30);
        reader.feed(worker.readAll());
        QList<ConsoleWorkerWire::Record> records;
        while (const auto record = reader.next()) records.append(*record);
        return records;
    }
};
}

void ConsoleWorkerEndpointTest::clipboardCrossesOnlyAfterReadyAndOnlyWhenAllowed()
{
    ClipboardWorker w;
    w.start(false);
    QVERIFY(w.ok);
    QSignalSpy offered(&w.endpoint, &ConsoleWorkerEndpoint::clipboardOffered);
    QList<ConsoleWorkerWire::ClipboardData> data;
    QList<ConsoleWorkerWire::ClipboardSetResult> results;
    connect(&w.endpoint, &ConsoleWorkerEndpoint::clipboardDataReceived, this, [&data](const auto &answer) { data.append(answer); });
    connect(&w.endpoint, &ConsoleWorkerEndpoint::clipboardSetFinished, this, [&results](const auto &result) { results.append(result); });
    // Before Ready nothing crosses: the policy is kept and sent once the worker is Ready, and nothing is requested.
    QVERIFY(w.endpoint.setClipboardPolicy(1, true, true, false, false));
    QCOMPARE(w.endpoint.requestClipboard(), quint64(0));
    QCOMPARE(w.endpoint.setClipboard(1, QStringLiteral("x")), quint64(0));
    QVERIFY(w.drain().isEmpty());
    w.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
    w.worker.flush();
    QTRY_VERIFY(w.endpoint.ready());
    auto records = w.drain();
    QCOMPARE(records.size(), 1);
    const auto policy = ConsoleWorkerWire::clipboardPolicy(records.first());
    QVERIFY(policy && policy->hostToClient && policy->clientToHost && policy->controlGeneration == 1 && policy->revision >= 1);

    // An identical policy is not resent; a change bumps the revision.
    QVERIFY(w.endpoint.setClipboardPolicy(1, true, true, false, false));
    QVERIFY(w.drain().isEmpty());
    QVERIFY(w.endpoint.setClipboardPolicy(1, true, false, true, false));
    records = w.drain();
    QCOMPARE(records.size(), 1);
    const auto changed = ConsoleWorkerWire::clipboardPolicy(records.first());
    QVERIFY(changed && changed->revision > policy->revision && !changed->clientToHost && changed->locked);

    // A set is refused while client to host is off, and under another control generation.
    QCOMPARE(w.endpoint.setClipboard(1, QStringLiteral("x")), quint64(0));
    QVERIFY(w.endpoint.setClipboardPolicy(1, true, true, false, false));
    w.drain();
    QCOMPARE(w.endpoint.setClipboard(2, QStringLiteral("x")), quint64(0));
    QCOMPARE(w.endpoint.setClipboard(1, QString(ConsoleWorkerWire::MaxClipboardBytes + 1, QLatin1Char('a'))), quint64(0));
    const auto setId = w.endpoint.setClipboard(1, QStringLiteral("from the client"));
    QVERIFY(setId);
    const auto requestId = w.endpoint.requestClipboard(1000);
    QVERIFY(requestId && requestId != setId); // one counter for both
    records = w.drain();
    QCOMPARE(records.size(), 2);
    const auto sent = ConsoleWorkerWire::clipboardSet(records.at(0));
    QVERIFY(sent && sent->setId == setId && sent->text == QStringLiteral("from the client"));
    const auto asked = ConsoleWorkerWire::clipboardRequest(records.at(1));
    QVERIFY(asked && asked->requestId == requestId && asked->maxBytes == 1000);

    // The worker's answers reach the broker; an answer for an id never issued is dropped without failing the worker.
    w.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardData{requestId + 100, 1, ConsoleWorkerWire::ClipboardStatus::Ok, QStringLiteral("nobody asked")}));
    w.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardData{requestId, 1, ConsoleWorkerWire::ClipboardStatus::Ok, QStringLiteral("host text")}));
    w.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardSetResult{setId, ConsoleWorkerWire::ClipboardSetStatus::Written}));
    w.worker.flush();
    QTRY_COMPARE(data.size(), 1);
    QCOMPARE(data.first().text, QStringLiteral("host text"));
    QTRY_COMPARE(results.size(), 1);
    QVERIFY(w.endpoint.ready());

    // Offers: newest only, and only while the policy allows host to client.
    w.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardOffer{5, 1}));
    w.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardOffer{4, 1})); // older: dropped
    w.worker.flush();
    QTRY_COMPARE(offered.size(), 1);
    QCOMPARE(offered.first().first().toULongLong(), quint64(5));
    QVERIFY(w.endpoint.setClipboardPolicy(1, false, true, false, false));
    w.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardOffer{6, 1}));
    w.worker.flush();
    QTest::qWait(60);
    QCOMPARE(offered.size(), 1); // denied: never forwarded
    QCOMPARE(w.endpoint.requestClipboard(), quint64(0)); // and nothing is requested
    QVERIFY(w.endpoint.ready());
}

void ConsoleWorkerEndpointTest::clipboardIdsAreEndpointWideAndForgottenWithTheWorker()
{
    ClipboardWorker w;
    w.start(true);
    QVERIFY(w.ok);
    int data = 0;
    connect(&w.endpoint, &ConsoleWorkerEndpoint::clipboardDataReceived, this, [&data](const auto &) { ++data; });
    QVERIFY(w.endpoint.setClipboardPolicy(3, true, true, false, false));
    w.drain();
    const auto first = w.endpoint.requestClipboard();
    QVERIFY(first);
    // At most eight requests wait for the worker; a ninth is refused instead of queued.
    for (int i = 0; i < 7; ++i) QVERIFY(w.endpoint.requestClipboard());
    QCOMPARE(w.endpoint.requestClipboard(), quint64(0));
    // The worker goes away: everything it was asked is forgotten, policy included.
    w.worker.disconnectFromServer();
    QTRY_VERIFY(!w.endpoint.ready());
    QVERIFY(!w.endpoint.clipboardPolicy());
    QCOMPARE(w.endpoint.requestClipboard(), quint64(0));
    // A new worker cannot answer an id from before it, and ids keep increasing.
    QLocalSocket again;
    again.connectToServer(w.endpoint.socketName());
    QVERIFY(again.waitForConnected(1000));
    again.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{w.target.sessionId, w.target.uid, w.token}));
    again.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
    again.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardData{first, 1, ConsoleWorkerWire::ClipboardStatus::Ok, QStringLiteral("late")}));
    again.flush();
    QTRY_VERIFY(w.endpoint.ready());
    QTest::qWait(60);
    QCOMPARE(data, 0);
    QVERIFY(w.endpoint.setClipboardPolicy(3, true, true, false, false));
    QTest::qWait(30);
    const auto next = w.endpoint.requestClipboard();
    QVERIFY(next > first + 7);
}

void ConsoleWorkerEndpointTest::clipboardRecordBeforeReadyFailsTheWorker()
{
    ClipboardWorker w;
    w.start(false);
    QVERIFY(w.ok);
    int errors = 0;
    connect(&w.endpoint, &ConsoleWorkerEndpoint::protocolError, this, [&errors](const QString &) { ++errors; });
    w.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardOffer{1, 1}));
    w.worker.flush();
    QTRY_COMPARE(errors, 1);
    QVERIFY(!w.endpoint.ready());
}

// OPT-072 (wire 19).
void ConsoleWorkerEndpointTest::clipboardImagesCrossWithTheirKindAndLimits()
{
    using namespace ConsoleWorkerWire;
    ClipboardWorker w;
    w.start(true);
    QVERIFY(w.ok);
    QSignalSpy offered(&w.endpoint, &ConsoleWorkerEndpoint::clipboardOffered);
    QList<ClipboardData> data;
    connect(&w.endpoint, &ConsoleWorkerEndpoint::clipboardDataReceived, this, [&data](const auto &answer) { data.append(answer); });
    const QByteArray png(2000, 'p');
    // Nothing without a policy; client to host off refuses a set, host to client off refuses a request.
    QCOMPARE(w.endpoint.requestClipboardImage(), quint64(0));
    QCOMPARE(w.endpoint.setClipboardImage(1, png), quint64(0));
    QVERIFY(w.endpoint.setClipboardPolicy(1, true, false, false, false));
    w.drain();
    QCOMPARE(w.endpoint.setClipboardImage(1, png), quint64(0));
    QVERIFY(w.endpoint.setClipboardPolicy(1, false, true, false, false));
    w.drain();
    QCOMPARE(w.endpoint.requestClipboardImage(), quint64(0));
    QVERIFY(w.endpoint.setClipboardPolicy(1, true, true, false, false));
    w.drain();
    // Bounds: empty, over 16 MiB, another generation; and a request over the image limit.
    QCOMPARE(w.endpoint.setClipboardImage(1, {}), quint64(0));
    QCOMPARE(w.endpoint.setClipboardImage(1, QByteArray(MaxClipboardImageBytes + 1, 'p')), quint64(0));
    QCOMPARE(w.endpoint.setClipboardImage(2, png), quint64(0));
    QCOMPARE(w.endpoint.requestClipboardImage(0), quint64(0));
    QCOMPARE(w.endpoint.requestClipboardImage(MaxClipboardImageBytes + 1), quint64(0));
    QCOMPARE(w.endpoint.requestClipboard(MaxClipboardBytes + 1), quint64(0)); // text keeps its own limit

    const auto setId = w.endpoint.setClipboardImage(1, png);
    const auto requestId = w.endpoint.requestClipboardImage();
    QVERIFY(setId && requestId && setId != requestId);
    auto records = w.drain();
    QCOMPARE(records.size(), 2);
    const auto sent = clipboardSet(records.at(0));
    QVERIFY(sent && sent->kind == ClipboardKind::Image && sent->image == png && sent->setId == setId);
    const auto asked = clipboardRequest(records.at(1));
    QVERIFY(asked && asked->kind == ClipboardKind::Image && asked->requestId == requestId && asked->maxBytes == MaxClipboardImageBytes);

    // The answer must be of the kind that was asked: a text answer to an image request is dropped (and forgets the id).
    w.worker.write(frame(ClipboardData{requestId, 1, ClipboardStatus::Ok, QStringLiteral("text"), ClipboardKind::Text, {}}));
    w.worker.write(frame(ClipboardData{requestId, 1, ClipboardStatus::Ok, {}, ClipboardKind::Image, png}));
    w.worker.flush();
    QTest::qWait(80);
    QCOMPARE(data.size(), 0);
    QVERIFY(w.endpoint.ready());
    // A right answer reaches the broker with its picture.
    const auto second = w.endpoint.requestClipboardImage(1000);
    QVERIFY(second);
    w.drain();
    w.worker.write(frame(ClipboardData{second, 1, ClipboardStatus::Ok, {}, ClipboardKind::Image, png}));
    w.worker.flush();
    QTRY_COMPARE(data.size(), 1);
    QCOMPARE(data.first().kind, ClipboardKind::Image);
    QCOMPARE(data.first().image, png);
    // An offer says what it covers.
    w.worker.write(frame(ClipboardOffer{9, ClipboardOfferImage}));
    w.worker.write(frame(ClipboardOffer{10, ClipboardOfferText | ClipboardOfferImage}));
    w.worker.flush();
    QTRY_COMPARE(offered.size(), 2);
    QCOMPARE(offered.at(0).at(1).value<quint8>(), ClipboardOfferImage);
    QCOMPARE(offered.at(1).at(1).value<quint8>(), quint8(ClipboardOfferText | ClipboardOfferImage));
    // Eight requests at most wait for the worker, images included.
    for (int i = 0; i < 8; ++i) QVERIFY(w.endpoint.requestClipboardImage());
    QCOMPARE(w.endpoint.requestClipboardImage(), quint64(0));
    QVERIFY(w.endpoint.ready());
}

// OPT-071 (wire 18).
void ConsoleWorkerEndpointTest::hostPointerIsRelayedAfterReadyAndForgottenWithTheWorker()
{
    ClipboardWorker w;
    w.start(true);
    QVERIFY(w.ok);
    QList<ConsoleWorkerWire::HostPointer> seen;
    connect(&w.endpoint, &ConsoleWorkerEndpoint::hostPointerReceived, this, [&seen](const ConsoleWorkerWire::HostPointer &pointer) { seen.append(pointer); });
    QVERIFY(!w.endpoint.hostPointer());
    w.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::HostPointer{100, 200, true}) + ConsoleWorkerWire::frame(ConsoleWorkerWire::HostPointer{101, 201, false}));
    w.worker.flush();
    QTRY_COMPARE(seen.size(), 2);
    QCOMPARE(seen.first(), (ConsoleWorkerWire::HostPointer{100, 200, true}));
    QCOMPARE(w.endpoint.hostPointer(), std::optional(ConsoleWorkerWire::HostPointer{101, 201, false}));
    QVERIFY(w.endpoint.ready()); // an ordinary record does not disturb the worker
    w.worker.disconnectFromServer();
    QTRY_VERIFY(!w.endpoint.ready());
    QVERIFY(!w.endpoint.hostPointer());
}

void ConsoleWorkerEndpointTest::hostPointerBeforeReadyOrMalformedFailsTheWorker()
{
    {
        ClipboardWorker w;
        w.start(false);
        QVERIFY(w.ok);
        int errors = 0;
        connect(&w.endpoint, &ConsoleWorkerEndpoint::protocolError, this, [&errors](const QString &) { ++errors; });
        w.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::HostPointer{1, 2, true}));
        w.worker.flush();
        QTRY_COMPARE(errors, 1);
        QVERIFY(!w.endpoint.ready());
    }
    {
        ClipboardWorker w;
        w.start(true);
        QVERIFY(w.ok);
        int errors = 0;
        QString why;
        connect(&w.endpoint, &ConsoleWorkerEndpoint::protocolError, this, [&](const QString &reason) { ++errors; why = reason; });
        w.worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::HostPointer, QByteArray(5, 'x')));
        w.worker.flush();
        QTRY_COMPARE(errors, 1);
        QVERIFY(why.contains(QStringLiteral("host pointer")));
    }
}

QTEST_GUILESS_MAIN(ConsoleWorkerEndpointTest)

#include "ConsoleWorkerEndpointTest.moc"
