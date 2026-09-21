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

    int ready = 0;
    int frames = 0;
    int layouts = 0;
    quint64 takeoverGeneration = 0;
    connect(&endpoint, &ConsoleWorkerEndpoint::localTakeover, this, [&takeoverGeneration](quint64 generation) {
        takeoverGeneration = generation;
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [&layouts](const auto &outputs) {
        QCOMPARE(outputs.monitors.first().name, QStringLiteral("DP-1"));
        ++layouts;
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
