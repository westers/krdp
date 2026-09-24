// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

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
};

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
    const ConsoleHandoff::Target target{ConsoleSeat::Adapter::Greeter, QStringLiteral("1"), 107};
    QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), target, QByteArray(24, 't')));
    int errors = 0;
    connect(&endpoint, &ConsoleWorkerEndpoint::protocolError, this, [&errors](const QString &) { ++errors; });

    QLocalSocket worker;
    worker.connectToServer(endpoint.socketName());
    QVERIFY(worker.waitForConnected(1000));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("1"), 107, QByteArray(24, 'x')}));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(errors, 1);
    QVERIFY(!endpoint.ready());
}

QTEST_GUILESS_MAIN(ConsoleWorkerEndpointTest)

#include "ConsoleWorkerEndpointTest.moc"
