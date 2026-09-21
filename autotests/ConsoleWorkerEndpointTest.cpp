// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QLocalSocket>
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

    int ready = 0;
    int frames = 0;
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
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
    QVERIFY(worker.waitForBytesWritten(1000));
    QTRY_COMPARE(ready, 1);

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
