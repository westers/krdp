// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-C-2, AUD-C-6, AUD-C-9, AUD-C-10: the broker's worker endpoint.

#include <QDataStream>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QJsonDocument>
#include <QUuid>

#include <unistd.h>

#include "ConsoleWorkerEndpoint.h"

using namespace KRdp;

namespace
{
const QByteArray Token(24, 't');

ConsoleHandoff::Target self()
{
    return {ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), quint32(getuid())};
}

QByteArray hello()
{
    return ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("3"), quint32(getuid()), Token});
}

/// A well-formed record from a worker built with another paired wire version.
QByteArray foreignVersionRecord(quint16 version)
{
    QByteArray body;
    QDataStream stream(&body, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << version << quint8(ConsoleWorkerWire::Kind::Hello) << QByteArray("x");
    QByteArray result;
    QDataStream header(&result, QIODevice::WriteOnly);
    header.setByteOrder(QDataStream::BigEndian);
    header << quint32(body.size());
    return result + body;
}

std::vector<ConsoleWorkerWire::Kind> brokerKinds(QLocalSocket &worker)
{
    ConsoleWorkerWire::Deframer deframer;
    deframer.feed(worker.readAll());
    std::vector<ConsoleWorkerWire::Kind> kinds;
    while (const auto record = deframer.next()) {
        kinds.push_back(record->kind);
    }
    return kinds;
}
}

class ConsoleWorkerEndpointHardeningTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void wireVersionIsFive()
    {
        // Reset to 1 on 2026-09-27 (AUD-C-6); 2 since AUD-FIX7 (codec-tagged frames, encoder
        // records); 3 since STATS-S6 (EncoderConfig::statsWanted, EncoderStats); 4 since
        // FIX-CURSOR (Cursor); 5 since AV1-Q (EncoderConfig carries the AV1 tile count).
        QCOMPARE(ConsoleWorkerWire::ProtocolVersion, quint16(11));
    }

    void pointerPolicyRequiresFreshWorkerGrantAndSnapshot()
    {
        QTemporaryDir directory;
        ConsoleWorkerEndpoint endpoint;
        QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("w.sock")), self(), Token));
        QLocalSocket worker;
        worker.connectToServer(endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        QTRY_VERIFY(endpoint.m_worker);
        worker.write(hello() + ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(endpoint.ready());
        endpoint.setControlState({7,true});
        const auto epoch=QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonObject state{{QStringLiteral("v"),1},{QStringLiteral("supported"),true},
            {QStringLiteral("generation"),QStringLiteral("7")},{QStringLiteral("epoch"),epoch},
            {QStringLiteral("revision"),QStringLiteral("1")},{QStringLiteral("requested"),true},
            {QStringLiteral("locked"),true},{QStringLiteral("leased"),false},
            {QStringLiteral("permitted"),false},{QStringLiteral("blocked"),false}};
        QJsonObject request{{QStringLiteral("v"),1},{QStringLiteral("id"),QStringLiteral("capture")},
            {QStringLiteral("generation"),QStringLiteral("7")},{QStringLiteral("epoch"),epoch},{QStringLiteral("enabled"),true}};
        QVERIFY(!endpoint.setPointerCapture(request)); // No compositor snapshot yet.
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::PointerState,QJsonDocument(state).toJson(QJsonDocument::Compact)));
        QTRY_VERIFY(!endpoint.pointerState().isEmpty());
        QVERIFY(endpoint.setPointerCapture(request));
        request.insert(QStringLiteral("generation"),QStringLiteral("6"));
        QVERIFY(!endpoint.setPointerCapture(request));
        request.insert(QStringLiteral("generation"),QStringLiteral("7"));
        request.insert(QStringLiteral("epoch"),QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVERIFY(!endpoint.setPointerCapture(request));
        endpoint.setControlState({8,false});
        QVERIFY(endpoint.pointerState().isEmpty());
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::PointerState,QJsonDocument(state).toJson(QJsonDocument::Compact)));
        QTest::qWait(20);
        QVERIFY(endpoint.pointerState().isEmpty()); // Late old-owner state cannot revive permission.
        QVERIFY(!endpoint.setPointerCapture(request));
    }

    void stopBeforeAuthenticationIsDeliveredAfterIt()
    {
        QTemporaryDir directory;
        ConsoleWorkerEndpoint endpoint;
        QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("w.sock")), self(), Token));
        QSignalSpy ready(&endpoint, &ConsoleWorkerEndpoint::workerReady);
        QLocalSocket worker;
        worker.connectToServer(endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        QTRY_VERIFY(endpoint.m_worker);

        endpoint.stopWorker(); // The broker drains before the replacement authenticated.
        QVERIFY(endpoint.stopPending());
        QTest::qWait(20);
        QCOMPARE(worker.bytesAvailable(), 0); // Nothing may cross an unauthenticated socket.

        worker.write(hello());
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_VERIFY(worker.bytesAvailable() > 0);
        QCOMPARE(brokerKinds(worker), std::vector<ConsoleWorkerWire::Kind>{ConsoleWorkerWire::Kind::Stop});
        QVERIFY(!endpoint.stopPending());

        // Ready while draining: the worker must never become the input endpoint.
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTest::qWait(30);
        QCOMPARE(ready.count(), 0);
        QVERIFY(!endpoint.ready());
    }

    void rejectsForeignPeerUid()
    {
        QTemporaryDir directory;
        ConsoleWorkerEndpoint endpoint;
        auto target = self();
        target.uid = quint32(getuid()) + 1; // Some other account's worker.
        QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("w.sock")), target, Token));
        QSignalSpy errors(&endpoint, &ConsoleWorkerEndpoint::protocolError);
        QLocalSocket worker;
        worker.connectToServer(endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(errors.first().first().toString().contains(QStringLiteral("peer credentials")));
        QTRY_COMPARE(worker.state(), QLocalSocket::UnconnectedState);
        QVERIFY(!endpoint.m_worker);
    }

    void closesUnauthenticatedPeerAfterTimeout()
    {
        QTemporaryDir directory;
        ConsoleWorkerEndpoint endpoint;
        QCOMPARE(ConsoleWorkerEndpoint::AuthenticationTimeoutMs, 5000);
        endpoint.setAuthenticationTimeout(50); // Injected clock: the production budget is 5 s.
        QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("w.sock")), self(), Token));
        QSignalSpy errors(&endpoint, &ConsoleWorkerEndpoint::protocolError);
        QLocalSocket worker;
        worker.connectToServer(endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(errors.first().first().toString().contains(QStringLiteral("authenticate")));
        QTRY_COMPARE(worker.state(), QLocalSocket::UnconnectedState);

        // An authenticated worker is not subject to the deadline.
        QSignalSpy later(&endpoint, &ConsoleWorkerEndpoint::protocolError);
        QLocalSocket good;
        good.connectToServer(endpoint.socketName());
        QVERIFY(good.waitForConnected(1000));
        good.write(hello());
        QVERIFY(good.waitForBytesWritten(1000));
        QTRY_VERIFY(endpoint.authenticated());
        QTest::qWait(120);
        QCOMPARE(later.count(), 0);
        QCOMPARE(good.state(), QLocalSocket::ConnectedState);
    }

    void capsPreAuthenticationBuffer()
    {
        QTemporaryDir directory;
        ConsoleWorkerEndpoint endpoint;
        QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("w.sock")), self(), Token));
        QSignalSpy errors(&endpoint, &ConsoleWorkerEndpoint::protocolError);
        QLocalSocket worker;
        worker.connectToServer(endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        // A length prefix announcing 1 MiB, then more than 64 KiB of it.
        QByteArray flood;
        QDataStream header(&flood, QIODevice::WriteOnly);
        header.setByteOrder(QDataStream::BigEndian);
        header << quint32(1024 * 1024);
        flood.append(QByteArray(ConsoleWorkerEndpoint::MaxPreAuthenticationBytes + 1024, 'a'));
        worker.write(flood);
        worker.flush();
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(errors.first().first().toString().contains(QStringLiteral("before authenticating")));
        QVERIFY(!endpoint.authenticated());
    }

    void acceptsLargeFramesCoalescedAfterHello()
    {
        // The cap must not punish a legitimate worker whose Hello, Ready and
        // first large keyframe arrive in one read.
        QTemporaryDir directory;
        ConsoleWorkerEndpoint endpoint;
        QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("w.sock")), self(), Token));
        QSignalSpy errors(&endpoint, &ConsoleWorkerEndpoint::protocolError);
        QSignalSpy frames(&endpoint, &ConsoleWorkerEndpoint::frameReceived);
        QLocalSocket worker;
        worker.connectToServer(endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        VideoFrame frame;
        frame.size = QSize(1920, 1080);
        frame.isKeyFrame = true;
        frame.data = QByteArray(200 * 1024, 'k');
        worker.write(hello() + ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready) + ConsoleWorkerWire::frame(frame));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_COMPARE(frames.count(), 1);
        QCOMPARE(errors.count(), 0);
    }

    void reportsVersionMismatch()
    {
        QTemporaryDir directory;
        ConsoleWorkerEndpoint endpoint;
        QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("w.sock")), self(), Token));
        QSignalSpy mismatch(&endpoint, &ConsoleWorkerEndpoint::versionMismatch);
        QSignalSpy errors(&endpoint, &ConsoleWorkerEndpoint::protocolError);
        QLocalSocket worker;
        worker.connectToServer(endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        worker.write(foreignVersionRecord(12));
        QVERIFY(worker.waitForBytesWritten(1000));
        QTRY_COMPARE(mismatch.count(), 1);
        QCOMPARE(mismatch.first().first().value<quint16>(), quint16(12));
        QCOMPARE(errors.count(), 1);
    }

    void deletesDisconnectedSockets()
    {
        QTemporaryDir directory;
        ConsoleWorkerEndpoint endpoint;
        QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("w.sock")), self(), Token));
        auto *worker = new QLocalSocket(this);
        worker->connectToServer(endpoint.socketName());
        QVERIFY(worker->waitForConnected(1000));
        QTRY_VERIFY(endpoint.m_worker);
        const QPointer<QLocalSocket> accepted = endpoint.m_worker.data();
        worker->disconnectFromServer();
        delete worker;
        QTRY_VERIFY(!endpoint.m_worker);
        QTRY_VERIFY(!accepted); // deleteLater ran; no socket leaks per launch.
    }

    void rejectsOutOfBoundsVideoFrames()
    {
        VideoFrame frame;
        frame.size = QSize(1280, 720);
        frame.data = "x";
        const auto roundTrip = [](const VideoFrame &value) {
            ConsoleWorkerWire::Deframer deframer;
            deframer.feed(ConsoleWorkerWire::frame(value));
            const auto record = deframer.next();
            return record ? ConsoleWorkerWire::videoFrame(*record) : std::nullopt;
        };
        QVERIFY(roundTrip(frame));
        auto bad = frame;
        bad.monitorIndex = 1; // No monitor list: only index 0 exists.
        QVERIFY(!roundTrip(bad));
        bad = frame;
        bad.monitorIndex = -1;
        QVERIFY(!roundTrip(bad));
        bad = frame;
        bad.monitors = {{QRect(0, 0, 640, 720), true}, {QRect(640, 0, 640, 720), false}};
        bad.monitorIndex = 2;
        QVERIFY(!roundTrip(bad));
        bad.monitorIndex = 1;
        QVERIFY(roundTrip(bad));
        bad = frame;
        bad.size = QSize(0, 720);
        QVERIFY(!roundTrip(bad));
        bad.size = QSize(ConsoleWorkerWire::MaxFrameDimension + 1, 720);
        QVERIFY(!roundTrip(bad));
    }
};

QTEST_GUILESS_MAIN(ConsoleWorkerEndpointHardeningTest)

#include "ConsoleWorkerEndpointHardeningTest.moc"
