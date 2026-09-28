// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX4 D1 end to end: a real KRdp::Server on 127.0.0.1, krdpctl-probe (the real libfreerdp
// client, RDPGFX with frame acknowledgements, --gfx-count so the fake frames are not decoded)
// behind a proxy in this test that throttles and then blocks the server-to-client direction,
// the way the Sol/Buzz tbf run saturated the link. The test feeds numbered frames at 30 fps
// straight into the connection's VideoStream (standing in for the encoder) and checks:
// - the frames in flight (sent, not acknowledged) never exceed the window's cap;
// - while the link is throttled the server keeps reading: acknowledgements keep arriving;
// - frames that cannot go are coalesced, and a keyframe is asked for;
// - the session survives a 5 s block, and once the client reads again it gets a current frame.
// AUD-FIX6 F2: and a 1 MB (4K) keyframe on a throttled link does not turn into a keyframe loop.
// AUD-FIX7 F2: and a client that is slow to acknowledge (285 ms) on a fat link gets the full rate
// without rekeys.
// AUD-FIX10: and a Refresh Rect from the client on an idle stream asks the encoder for a keyframe
// (at most one per second, however many PDUs), which then reaches the client.

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <memory>
#include <vector>

#include "FrameQueuePolicy.h"
#include "RdpConnection.h"
#include "Server.h"
#include "VideoStream.h"

using namespace KRdp;
using namespace std::chrono_literals;

namespace
{
const QString TestUser = QStringLiteral("alice");
const QString Password = QStringLiteral("correct horse");

/**
 * A TCP proxy whose server-to-client direction can be throttled to a byte rate or blocked.
 * The upstream socket's read buffer is small, so a slow or stopped reader pushes back on the
 * server's socket like a slow link does; the client-to-server direction is never held.
 */
class ThrottledProxy : public QObject
{
public:
    explicit ThrottledProxy(quint16 target)
        : m_target(target)
    {
        connect(&m_server, &QTcpServer::newConnection, this, &ThrottledProxy::accept);
        m_tick.setInterval(5);
        connect(&m_tick, &QTimer::timeout, this, &ThrottledProxy::pump);
        m_tick.start();
        m_clock.start();
    }
    bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    quint16 port() const
    {
        return m_server.serverPort();
    }
    /// 0 = unlimited.
    void setRate(qint64 bytesPerSecond)
    {
        m_rate = bytesPerSecond;
        m_tokens = 0;
    }
    void setBlocked(bool blocked)
    {
        m_blocked = blocked;
    }
    qint64 forwardedToClient() const
    {
        return m_forwarded;
    }

private:
    struct Pair {
        QTcpSocket *client = nullptr;
        std::unique_ptr<QTcpSocket> upstream;
    };

    void accept()
    {
        while (auto *client = m_server.nextPendingConnection()) {
            auto pair = std::make_unique<Pair>();
            pair->client = client;
            pair->upstream = std::make_unique<QTcpSocket>();
            pair->upstream->setReadBufferSize(32 * 1024);
            pair->upstream->connectToHost(QHostAddress::LocalHost, m_target);
            pair->upstream->waitForConnected(5000);
            pair->upstream->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 32 * 1024);
            auto *upstream = pair->upstream.get();
            connect(client, &QTcpSocket::readyRead, upstream, [client, upstream] {
                upstream->write(client->readAll());
            });
            connect(client, &QTcpSocket::disconnected, upstream, [upstream] {
                upstream->disconnectFromHost();
            });
            connect(upstream, &QTcpSocket::disconnected, client, [client] {
                client->disconnectFromHost();
            });
            m_pairs.push_back(std::move(pair));
        }
    }

    void pump()
    {
        const qint64 elapsedMs = m_clock.restart();
        if (m_blocked) {
            return;
        }
        if (m_rate > 0) {
            m_tokens = std::min<qint64>(m_tokens + m_rate * elapsedMs / 1000, m_rate / 10 + 1);
        }
        for (const auto &pair : m_pairs) {
            if (!pair->client || pair->client->state() != QAbstractSocket::ConnectedState) {
                continue;
            }
            qint64 budget = m_rate > 0 ? m_tokens : pair->upstream->bytesAvailable();
            if (budget <= 0) {
                continue;
            }
            const QByteArray chunk = pair->upstream->read(budget);
            if (chunk.isEmpty()) {
                continue;
            }
            pair->client->write(chunk);
            m_forwarded += chunk.size();
            if (m_rate > 0) {
                m_tokens -= chunk.size();
            }
        }
    }

    QTcpServer m_server;
    quint16 m_target;
    std::vector<std::unique_ptr<Pair>> m_pairs;
    QTimer m_tick;
    QElapsedTimer m_clock;
    qint64 m_rate = 0;
    qint64 m_tokens = 0;
    bool m_blocked = false;
    qint64 m_forwarded = 0;
};

/// The last 16 bytes of every fed frame: "KRDPSEQ:" + the sequence number (little endian).
QByteArray frameData(quint64 seq, int size)
{
    QByteArray data(size, '\x5a');
    QByteArray tail("KRDPSEQ:");
    for (int i = 0; i < 8; ++i) {
        tail.append(char((seq >> (8 * i)) & 0xff));
    }
    data.replace(size - tail.size(), tail.size(), tail);
    return data;
}
}

class VideoFlowLoopbackTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_certificate;
    QString m_key;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        const QString openssl = QStandardPaths::findExecutable(QStringLiteral("openssl"));
        if (openssl.isEmpty()) {
            QSKIP("openssl is needed to make the test certificate");
        }
        m_certificate = m_dir.filePath(QStringLiteral("server.crt"));
        m_key = m_dir.filePath(QStringLiteral("server.key"));
        QProcess process;
        process.start(openssl,
                      {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"), QStringLiteral("rsa:2048"), QStringLiteral("-nodes"),
                       QStringLiteral("-keyout"), m_key, QStringLiteral("-out"), m_certificate, QStringLiteral("-days"), QStringLiteral("1"),
                       QStringLiteral("-subj"), QStringLiteral("/CN=krdp-test")});
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(process.exitCode(), 0);
    }

    void saturatedLinkKeepsTheSession()
    {
        Server server;
        server.setAddress(QHostAddress::LocalHost);
        server.setPort(0);
        server.setTlsCertificate(m_certificate.toStdString());
        server.setTlsCertificateKey(m_key.toStdString());
        server.setUsers({{TestUser, Password}});

        QPointer<RdpConnection> connection;
        bool closed = false;
        quint64 seq = 0;
        int sinceKeyFrame = 0;
        bool feeding = true;
        const auto feed = [&](bool keyFrame) {
            if (!connection || connection->state() != RdpConnection::State::Streaming || !connection->videoStream()->enabled()) {
                return;
            }
            VideoFrame frame;
            frame.size = QSize(640, 480);
            frame.isKeyFrame = keyFrame;
            // ~2.9 Mbit/s of P-frames at 30 fps; a keyframe is four times as big.
            frame.data = frameData(++seq, keyFrame ? 48000 : 12000);
            frame.presentationTimeStamp = std::chrono::system_clock::now();
            connection->videoStream()->queueFrame(frame);
            sinceKeyFrame = keyFrame ? 0 : sinceKeyFrame + 1;
        };
        int keyFramesRequested = 0;
        connect(&server, &Server::newConnectionCreated, this, [&](RdpConnection *c) {
            connection = c;
            c->videoStream()->setCodecPreference(CodecPreference::Avc420);
            // Context c, not the test object: a queued slot must not outlive the connection
            // (the next test runs in the same event loop).
            connect(c, &RdpConnection::stateChanged, c, [&, c] {
                if (c->state() == RdpConnection::State::Closed) {
                    closed = true;
                }
            });
            // What the encoder does for a request: an IDR of the newest picture at once
            // (KPipeWire re-feeds the last captured frame), even when nothing else changes.
            connect(c->videoStream(), &VideoStream::keyFrameRequested, c, [&](int) {
                ++keyFramesRequested;
                feed(true);
            }, Qt::QueuedConnection);
        });
        QVERIFY(server.start());

        ThrottledProxy proxy(server.serverPort());
        QVERIFY(proxy.listen());

        QTimer feeder;
        feeder.setInterval(33);
        connect(&feeder, &QTimer::timeout, this, [&] {
            if (feeding) {
                feed(sinceKeyFrame >= 60 || seq == 0);
            }
        });
        feeder.start();

        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), m_dir.path());
        environment.insert(QStringLiteral("HOME"), m_dir.path()); // FreeRDP's known-hosts file
        probe.setProcessEnvironment(environment);
        // The password goes over stdin ("-"), never on the command line.
        probe.start(QStringLiteral(KRDPCTL_PROBE),
                    {QStringLiteral("127.0.0.1"), QString::number(proxy.port()), TestUser, QStringLiteral("-"), QStringLiteral("--silent"),
                     QStringLiteral("--no-krdpctl"), QStringLiteral("--gfx"), QStringLiteral("--gfx-count"), QStringLiteral("--timeout"), QStringLiteral("120")});
        QVERIFY(probe.waitForStarted(5000));
        probe.write(Password.toUtf8() + '\n');
        probe.closeWriteChannel();
        const auto cleanup = qScopeGuard([&probe] {
            if (probe.state() != QProcess::NotRunning) {
                probe.terminate();
                if (!probe.waitForFinished(5000)) {
                    probe.kill();
                    probe.waitForFinished(5000);
                }
            }
        });

        QByteArray log;
        quint64 lastSeenSeq = 0;
        const auto readProbe = [&] {
            const QByteArray more = probe.readAllStandardError();
            log += more;
            for (const QByteArray &line : more.split('\n')) {
                const int at = line.indexOf("frame seq ");
                if (at >= 0) {
                    const QList<QByteArray> parts = line.mid(at + 10).split(' ');
                    lastSeenSeq = std::max(lastSeenSeq, parts.value(0).toULongLong());
                }
            }
        };
        int maxInFlight = 0;
        int windowFrames = FrameQueuePolicy::MaxInFlightFrames;
        const auto sample = [&](std::chrono::milliseconds duration) {
            QDeadlineTimer deadline(duration);
            while (!deadline.hasExpired()) {
                QTest::qWait(20);
                readProbe();
                if (connection) {
                    const auto stats = connection->videoStream()->flowStats();
                    maxInFlight = std::max(maxInFlight, stats.inFlight);
                    windowFrames = std::min(windowFrames, stats.windowFrames > 0 ? stats.windowFrames : windowFrames);
                    // AUD-FIX7 F2: the window follows the ack latency, so it can shrink below what
                    // is already out (a send only ever goes into an open window); never beyond the cap.
                    QVERIFY2(stats.inFlight <= FrameQueuePolicy::MaxInFlightFrames,
                             qPrintable(QStringLiteral("%1 in flight, window %2").arg(stats.inFlight).arg(stats.windowFrames)));
                }
                QVERIFY2(!closed, log.constData());
                QVERIFY2(probe.state() == QProcess::Running, log.constData());
            }
        };

        // Connected, streaming, frames arriving and acknowledged.
        QDeadlineTimer connectDeadline(30s);
        while ((lastSeenSeq < 10 || !connection || connection->videoStream()->flowStats().acknowledged < 10) && !connectDeadline.hasExpired()) {
            QTest::qWait(50);
            readProbe();
            if (probe.state() != QProcess::Running) {
                break;
            }
        }
        if (lastSeenSeq == 0 && log.contains("does not support H.264")) {
            QSKIP("this libfreerdp cannot negotiate AVC420 (built WITH_GFX_H264=OFF)");
        }
        QVERIFY2(lastSeenSeq >= 10, log.constData());
        QVERIFY(connection);
        sample(2s);
        if (QTest::currentTestFailed()) return; // the connection may be gone
        const auto before = connection->videoStream()->flowStats();
        qInfo() << "unthrottled:" << before.sent << "sent," << before.acknowledged << "acked, window" << before.windowFrames;

        // Throttled to 0.5 Mbit/s: a sixth of the stream.
        proxy.setRate(64 * 1024);
        sample(8s);
        if (QTest::currentTestFailed()) return; // the connection may be gone
        const auto throttled = connection->videoStream()->flowStats();
        qInfo() << "throttled:" << throttled.sent - before.sent << "sent," << throttled.acknowledged - before.acknowledged << "acked,"
                << throttled.dropped - before.dropped << "coalesced," << throttled.keyFrameRequests - before.keyFrameRequests << "keyframe requests, max in flight"
                << maxInFlight;
        QVERIFY(throttled.acknowledged - before.acknowledged >= 10); // the server kept reading acks
        QVERIFY(throttled.dropped > before.dropped); // what could not go was coalesced
        QVERIFY(throttled.keyFrameRequests > before.keyFrameRequests);

        // Blocked: the client reads nothing for 5 s.
        proxy.setBlocked(true);
        sample(5s);
        if (QTest::currentTestFailed()) return; // the connection may be gone
        // The desktop stops changing just before the client resumes: the newest picture is this.
        feeding = false;
        sample(300ms);
        if (QTest::currentTestFailed()) return; // the connection may be gone
        const quint64 newest = seq;
        const auto blocked = connection->videoStream()->flowStats();
        qInfo() << "blocked:" << blocked.sent - throttled.sent << "sent," << blocked.dropped - throttled.dropped << "coalesced; newest frame" << newest
                << ", client saw" << lastSeenSeq;
        QVERIFY(lastSeenSeq < newest);

        // The client reads again: it gets a current frame, and the session is still there.
        proxy.setBlocked(false);
        proxy.setRate(0);
        QDeadlineTimer current(10s);
        while (lastSeenSeq < newest && !current.hasExpired()) {
            sample(50ms);
            if (QTest::currentTestFailed()) return; // the connection may be gone
        }
        qInfo() << "resumed: client at frame" << lastSeenSeq << "of" << seq << "; max in flight" << maxInFlight << "window" << windowFrames;
        QVERIFY2(lastSeenSeq >= newest, log.right(4000).constData());
        feeding = true;
        sample(2s);
        if (QTest::currentTestFailed()) return; // the connection may be gone
        const auto after = connection->videoStream()->flowStats();
        QVERIFY(after.acknowledged > blocked.acknowledged);
        QVERIFY(maxInFlight <= FrameQueuePolicy::MaxInFlightFrames);
        QVERIFY(after.maxInFlight <= FrameQueuePolicy::MaxInFlightFrames);
        QVERIFY(!closed);
        QCOMPARE(probe.state(), QProcess::Running);
        QVERIFY(keyFramesRequested > 0);
    }

    // AUD-FIX6 F2: a 4K keyframe (1 MB, eight times the old 128 KiB byte floor) on a throttled
    // link. Before, the P-frames behind it were coalesced away and another 1 MB keyframe was
    // requested once a second: the client got one keyframe a second and nothing else (Sol's
    // console host, 265 KB keyframes, 2026-09-28). Now the P-frames wait for its ack, and
    // once the link carries them the client gets the full frame rate, with at most one rekey.
    // AUD-FIX10: FreeRDP_RefreshRect was advertised but never handled. krdp-client 0.5.5 sends a
    // Refresh Rect when its private codec shows nothing on an idle desktop; stock clients send one
    // after minimising or on a repaint. It must reach the encoder as a keyframe request.
    void refreshRectOnAnIdleStreamGetsAKeyframe()
    {
        Server server;
        server.setAddress(QHostAddress::LocalHost);
        server.setPort(0);
        server.setTlsCertificate(m_certificate.toStdString());
        server.setTlsCertificateKey(m_key.toStdString());
        server.setUsers({{TestUser, Password}});

        QPointer<RdpConnection> connection;
        quint64 seq = 0;
        QVector<quint64> keyFrameSeqs;
        const auto feed = [&](bool keyFrame) {
            if (!connection || connection->state() != RdpConnection::State::Streaming || !connection->videoStream()->enabled()) {
                return;
            }
            VideoFrame frame;
            frame.size = QSize(640, 480);
            frame.isKeyFrame = keyFrame;
            frame.data = frameData(++seq, keyFrame ? 48000 : 12000);
            frame.presentationTimeStamp = std::chrono::system_clock::now();
            connection->videoStream()->queueFrame(frame);
            if (keyFrame) keyFrameSeqs.append(seq);
        };
        int keyFramesRequested = 0;
        bool idle = false;
        connect(&server, &Server::newConnectionCreated, this, [&](RdpConnection *c) {
            connection = c;
            c->videoStream()->setCodecPreference(CodecPreference::Avc420);
            // The encoder's answer to a request: an IDR of the newest picture, even when idle.
            connect(c->videoStream(), &VideoStream::keyFrameRequested, c, [&](int) {
                ++keyFramesRequested;
                feed(true);
            }, Qt::QueuedConnection);
        });
        QVERIFY(server.start());

        // 20 frames (a keyframe first), then nothing: an idle desktop.
        QTimer feeder;
        feeder.setInterval(33);
        connect(&feeder, &QTimer::timeout, this, [&] {
            if (seq >= 20) {
                idle = true;
                feeder.stop();
                return;
            }
            feed(seq == 0);
        });
        feeder.start();

        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), m_dir.path());
        environment.insert(QStringLiteral("HOME"), m_dir.path());
        probe.setProcessEnvironment(environment);
        // Three Refresh Rect PDUs at once after 1.5 s without a frame: one keyframe request.
        probe.start(QStringLiteral(KRDPCTL_PROBE),
                    {QStringLiteral("127.0.0.1"), QString::number(server.serverPort()), TestUser, QStringLiteral("-"), QStringLiteral("--silent"),
                     QStringLiteral("--no-krdpctl"), QStringLiteral("--gfx"), QStringLiteral("--gfx-count"), QStringLiteral("--refresh-rect-idle"),
                     QStringLiteral("1500"), QStringLiteral("--refresh-rect-count"), QStringLiteral("3"), QStringLiteral("--timeout"), QStringLiteral("60")});
        QVERIFY(probe.waitForStarted(5000));
        probe.write(Password.toUtf8() + '\n');
        probe.closeWriteChannel();
        const auto cleanup = qScopeGuard([&probe] {
            if (probe.state() != QProcess::NotRunning) {
                probe.terminate();
                if (!probe.waitForFinished(5000)) {
                    probe.kill();
                    probe.waitForFinished(5000);
                }
            }
        });
        QByteArray log;
        QVector<quint64> seen;
        const auto readProbe = [&] {
            const QByteArray more = probe.readAllStandardError();
            log += more;
            for (const QByteArray &line : more.split('\n')) {
                const int at = line.indexOf("frame seq ");
                if (at >= 0) seen.append(line.mid(at + 10).split(' ').value(0).toULongLong());
            }
        };
        QDeadlineTimer connectDeadline(30s);
        while ((!idle || seen.size() < 20) && !connectDeadline.hasExpired() && probe.state() == QProcess::Running) {
            QTest::qWait(50);
            readProbe();
        }
        if (seen.isEmpty() && log.contains("does not support H.264")) {
            QSKIP("this libfreerdp cannot negotiate AVC420 (built WITH_GFX_H264=OFF)");
        }
        QVERIFY2(seen.size() >= 20, log.constData());
        const int requestsWhenIdle = keyFramesRequested;
        const qsizetype seenWhenIdle = seen.size();

        // The probe's Refresh Rect after 1.5 s of silence: a keyframe request, and that keyframe
        // (the only frame fed after the idle start) reaches the client.
        QDeadlineTimer refreshDeadline(10s);
        while (!log.contains("refresh rect sent") && !refreshDeadline.hasExpired() && probe.state() == QProcess::Running) {
            QTest::qWait(50);
            readProbe();
        }
        QVERIFY2(log.contains("refresh rect sent (3)"), log.constData());
        QTRY_VERIFY_WITH_TIMEOUT((readProbe(), seen.size() > seenWhenIdle), 5000);
        QCOMPARE(keyFramesRequested, requestsWhenIdle + 1);
        QVERIFY(keyFrameSeqs.size() >= 2);
        QCOMPARE(seen.last(), keyFrameSeqs.last());
        QCOMPARE(seen.last(), quint64(21));
        // Three PDUs within a second: one request, not three.
        QTest::qWait(1200);
        readProbe();
        QCOMPARE(keyFramesRequested, requestsWhenIdle + 1);
        QCOMPARE(seen.size(), seenWhenIdle + 1);
        // After a second the next Refresh Rect is honoured again (straight on the stream).
        QVERIFY(connection);
        QVERIFY(connection->videoStream()->requestRefresh());
        QTRY_COMPARE_WITH_TIMEOUT(keyFramesRequested, requestsWhenIdle + 2, 2000);
        QVERIFY(!connection->videoStream()->requestRefresh());
    }

    void hugeKeyFrameOnAThrottledLink()
    {
        Server server;
        server.setAddress(QHostAddress::LocalHost);
        server.setPort(0);
        server.setTlsCertificate(m_certificate.toStdString());
        server.setTlsCertificateKey(m_key.toStdString());
        server.setUsers({{TestUser, Password}});

        QPointer<RdpConnection> connection;
        bool closed = false;
        quint64 seq = 0;
        int keyFramesFed = 0;
        const auto feed = [&](bool keyFrame) {
            if (!connection || connection->state() != RdpConnection::State::Streaming || !connection->videoStream()->enabled()) {
                return;
            }
            VideoFrame frame;
            frame.size = QSize(3840, 2160);
            frame.isKeyFrame = keyFrame;
            // A 4K IDR of a busy desktop, then 8 kB P-frames (about 2 Mbit/s at 30 fps).
            frame.data = frameData(++seq, keyFrame ? 1000 * 1000 : 8000);
            frame.presentationTimeStamp = std::chrono::system_clock::now();
            connection->videoStream()->queueFrame(frame);
            keyFramesFed += keyFrame ? 1 : 0;
        };
        connect(&server, &Server::newConnectionCreated, this, [&](RdpConnection *c) {
            connection = c;
            c->videoStream()->setCodecPreference(CodecPreference::Avc420);
            // Context c, not the test object: a queued slot must not outlive the connection
            // (the next test runs in the same event loop).
            connect(c, &RdpConnection::stateChanged, c, [&, c] {
                if (c->state() == RdpConnection::State::Closed) {
                    closed = true;
                }
            });
            // The encoder answers a request with an IDR of the newest picture at once.
            connect(c->videoStream(), &VideoStream::keyFrameRequested, c, [&](int) {
                feed(true);
            }, Qt::QueuedConnection);
        });
        QVERIFY(server.start());

        ThrottledProxy proxy(server.serverPort());
        QVERIFY(proxy.listen());

        QTimer feeder;
        feeder.setInterval(33);
        connect(&feeder, &QTimer::timeout, this, [&] {
            feed(seq == 0);
        });

        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), m_dir.path());
        environment.insert(QStringLiteral("HOME"), m_dir.path());
        probe.setProcessEnvironment(environment);
        probe.start(QStringLiteral(KRDPCTL_PROBE),
                    {QStringLiteral("127.0.0.1"), QString::number(proxy.port()), TestUser, QStringLiteral("-"), QStringLiteral("--silent"),
                     QStringLiteral("--no-krdpctl"), QStringLiteral("--gfx"), QStringLiteral("--gfx-count"), QStringLiteral("--timeout"), QStringLiteral("120")});
        QVERIFY(probe.waitForStarted(5000));
        probe.write(Password.toUtf8() + '\n');
        probe.closeWriteChannel();
        const auto cleanup = qScopeGuard([&probe] {
            if (probe.state() != QProcess::NotRunning) {
                probe.terminate();
                if (!probe.waitForFinished(5000)) {
                    probe.kill();
                    probe.waitForFinished(5000);
                }
            }
        });

        QByteArray log;
        quint64 lastSeenSeq = 0;
        int framesSeen = 0;
        const auto readProbe = [&] {
            const QByteArray more = probe.readAllStandardError();
            log += more;
            for (const QByteArray &line : more.split('\n')) {
                const int at = line.indexOf("frame seq ");
                if (at >= 0) {
                    lastSeenSeq = std::max(lastSeenSeq, line.mid(at + 10).split(' ').value(0).toULongLong());
                    ++framesSeen;
                }
            }
        };
        const auto sample = [&](std::chrono::milliseconds duration) {
            QDeadlineTimer deadline(duration);
            while (!deadline.hasExpired()) {
                QTest::qWait(20);
                readProbe();
                QVERIFY2(!closed, log.constData());
                QVERIFY2(probe.state() == QProcess::Running, log.constData());
            }
        };

        // Wait for the stream to be up before the throttle and the first frame.
        QDeadlineTimer upDeadline(30s);
        while ((!connection || connection->state() != RdpConnection::State::Streaming || !connection->videoStream()->enabled()) && !upDeadline.hasExpired()) {
            QTest::qWait(20);
            readProbe();
            if (probe.state() != QProcess::Running) {
                break;
            }
        }
        QVERIFY2(connection && connection->state() == RdpConnection::State::Streaming, log.constData());
        // 6 Mbit/s: the 1 MB keyframe needs well over a second; the P-frames fit easily.
        proxy.setRate(750 * 1000);
        feeder.start();

        QDeadlineTimer firstDeadline(30s);
        while (lastSeenSeq == 0 && !firstDeadline.hasExpired() && probe.state() == QProcess::Running) {
            QTest::qWait(20);
            readProbe();
        }
        if (lastSeenSeq == 0 && log.contains("does not support H.264")) {
            QSKIP("this libfreerdp cannot negotiate AVC420 (built WITH_GFX_H264=OFF)");
        }
        QVERIFY2(lastSeenSeq >= 1, log.constData());
        const auto afterKey = connection->videoStream()->flowStats();
        qInfo() << "keyframe delivered: window" << afterKey.windowBytes / 1024 << "KiB," << afterKey.dropped << "coalesced," << afterKey.keyFrameRequests
                << "keyframe requests," << afterKey.keyFramesDeferred << "deferred";
        QVERIFY(afterKey.windowBytes >= 2 * 1000 * 1000);

        // Let the backlog behind the keyframe drain, then count what arrives: still throttled,
        // but the link now carries the P-frames at the full rate.
        sample(2s);
        if (QTest::currentTestFailed()) return; // the connection may be gone
        int before = framesSeen;
        const quint64 seqBefore = lastSeenSeq;
        sample(3s);
        if (QTest::currentTestFailed()) return; // the connection may be gone
        const int throttledFrames = framesSeen - before;
        qInfo() << "throttled, after the keyframe:" << throttledFrames << "frames in 3 s; client at" << lastSeenSeq << "of" << seq;
        QVERIFY2(throttledFrames >= 75, log.right(3000).constData()); // >= 25 fps of the fed 30
        QVERIFY(lastSeenSeq > seqBefore);
        QVERIFY(seq - lastSeenSeq <= 10); // current, not a growing backlog

        // Unthrottled: still the full rate.
        proxy.setRate(0);
        sample(1s);
        if (QTest::currentTestFailed()) return; // the connection may be gone
        before = framesSeen;
        sample(2s);
        if (QTest::currentTestFailed()) return; // the connection may be gone
        const int freeFrames = framesSeen - before;
        const auto after = connection->videoStream()->flowStats();
        qInfo() << "unthrottled:" << freeFrames << "frames in 2 s;" << after.dropped << "coalesced," << after.keyFrameRequests << "keyframe requests,"
                << keyFramesFed << "keyframes fed";
        QVERIFY(freeFrames >= 50);
        QVERIFY(after.keyFrameRequests <= 1); // at most one rekey
        QVERIFY(keyFramesFed <= 2);
        QVERIFY(!closed);
        QCOMPARE(probe.state(), QProcess::Running);
    }

    // AUD-FIX7 F2, the Sol :3391 case end to end: a client that acknowledges each frame 285 ms
    // after it arrived (krdpctl-probe --gfx-ack-delay: decode and present time, not the link)
    // on a fat link, 30 fps of 64 KiB P-frames and a 130 KB IDR every 100 frames. The window of
    // four (from the minimum RTT) capped it at 14 fps and rekeyed about once a second; now the
    // window follows the ack latency: the full rate and no rekey.
    void slowAckingClientKeepsFullRate()
    {
        Server server;
        server.setAddress(QHostAddress::LocalHost);
        server.setPort(0);
        server.setTlsCertificate(m_certificate.toStdString());
        server.setTlsCertificateKey(m_key.toStdString());
        server.setUsers({{TestUser, Password}});

        QPointer<RdpConnection> connection;
        bool closed = false;
        quint64 seq = 0;
        int sinceKeyFrame = 0;
        int keyFramesRequested = 0;
        const auto feed = [&](bool keyFrame) {
            if (!connection || connection->state() != RdpConnection::State::Streaming || !connection->videoStream()->enabled()) {
                return;
            }
            VideoFrame frame;
            frame.size = QSize(1920, 1080);
            frame.isKeyFrame = keyFrame;
            frame.data = frameData(++seq, keyFrame ? 130000 : 64 * 1024);
            frame.presentationTimeStamp = std::chrono::system_clock::now();
            connection->videoStream()->queueFrame(frame);
            sinceKeyFrame = keyFrame ? 0 : sinceKeyFrame + 1;
        };
        connect(&server, &Server::newConnectionCreated, this, [&](RdpConnection *c) {
            connection = c;
            c->videoStream()->setCodecPreference(CodecPreference::Avc420);
            connect(c, &RdpConnection::stateChanged, c, [&, c] {
                if (c->state() == RdpConnection::State::Closed) {
                    closed = true;
                }
            });
            connect(c->videoStream(), &VideoStream::keyFrameRequested, c, [&](int) {
                ++keyFramesRequested;
                feed(true);
            }, Qt::QueuedConnection);
        });
        QVERIFY(server.start());

        // The encoder: the frame rate the stream asks for (30 here; less if it throttles).
        QElapsedTimer clock;
        clock.start();
        qint64 nextFrameMs = 0;
        QTimer feeder;
        feeder.setInterval(2);
        feeder.setTimerType(Qt::PreciseTimer);
        connect(&feeder, &QTimer::timeout, this, [&] {
            if (!connection || clock.elapsed() < nextFrameMs) {
                return;
            }
            const int rate = std::clamp<int>(int(connection->videoStream()->requestedFrameRate()), 1, 30);
            nextFrameMs = std::max(nextFrameMs + 1000 / rate, clock.elapsed() - 1000 / rate);
            feed(seq == 0 || sinceKeyFrame + 1 >= 100);
        });
        feeder.start();

        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), m_dir.path());
        environment.insert(QStringLiteral("HOME"), m_dir.path());
        probe.setProcessEnvironment(environment);
        probe.start(QStringLiteral(KRDPCTL_PROBE),
                    {QStringLiteral("127.0.0.1"), QString::number(server.serverPort()), TestUser, QStringLiteral("-"), QStringLiteral("--silent"),
                     QStringLiteral("--no-krdpctl"), QStringLiteral("--gfx"), QStringLiteral("--gfx-count"), QStringLiteral("--gfx-ack-delay"), QStringLiteral("285"),
                     QStringLiteral("--timeout"), QStringLiteral("120")});
        QVERIFY(probe.waitForStarted(5000));
        probe.write(Password.toUtf8() + '\n');
        probe.closeWriteChannel();
        const auto cleanup = qScopeGuard([&probe] {
            if (probe.state() != QProcess::NotRunning) {
                probe.terminate();
                if (!probe.waitForFinished(5000)) {
                    probe.kill();
                    probe.waitForFinished(5000);
                }
            }
        });

        QByteArray log;
        quint64 lastSeenSeq = 0;
        int framesSeen = 0;
        const auto readProbe = [&] {
            const QByteArray more = probe.readAllStandardError();
            log += more;
            for (const QByteArray &line : more.split('\n')) {
                const int at = line.indexOf("frame seq ");
                if (at >= 0) {
                    lastSeenSeq = std::max(lastSeenSeq, line.mid(at + 10).split(' ').value(0).toULongLong());
                    ++framesSeen;
                }
            }
        };
        const auto sample = [&](std::chrono::milliseconds duration) {
            QDeadlineTimer deadline(duration);
            while (!deadline.hasExpired()) {
                QTest::qWait(20);
                readProbe();
                QVERIFY2(!closed, log.constData());
                QVERIFY2(probe.state() == QProcess::Running, log.constData());
            }
        };
        QDeadlineTimer upDeadline(30s);
        while (lastSeenSeq < 10 && !upDeadline.hasExpired() && probe.state() == QProcess::Running) {
            QTest::qWait(20);
            readProbe();
        }
        if (lastSeenSeq == 0 && log.contains("does not support H.264")) {
            QSKIP("this libfreerdp cannot negotiate AVC420 (built WITH_GFX_H264=OFF)");
        }
        QVERIFY2(lastSeenSeq >= 10, log.constData());
        QVERIFY2(log.contains("acknowledging frames 285 ms after their end"), log.constData());
        // Settle: the window learns the ack latency; a throttle from the first seconds lifts.
        sample(12s);
        if (QTest::currentTestFailed()) return; // the connection may be gone
        const auto before = connection->videoStream()->flowStats();
        const int seenBefore = framesSeen;
        const int requestsBefore = keyFramesRequested;
        sample(30s);
        if (QTest::currentTestFailed()) return; // the connection may be gone
        const auto after = connection->videoStream()->flowStats();
        const int delivered = framesSeen - seenBefore;
        qInfo() << "slow acks:" << delivered << "frames in 30 s," << keyFramesRequested - requestsBefore << "keyframe requests,"
                << after.dropped - before.dropped << "coalesced; ack latency" << after.ackLatencyUs / 1000 << "ms, window" << after.windowFrames << "frames"
                << after.windowBytes / 1024 << "KiB, frame rate" << after.frameRate << (after.linkClear ? "(link clear)" : "");
        QVERIFY(after.ackLatencyUs >= 250000);
        QVERIFY(after.windowFrames >= 10);
        QVERIFY2(delivered >= 27 * 30, log.right(3000).constData()); // >= 27 fps of 30
        QVERIFY(keyFramesRequested - requestsBefore <= 1); // at most one rekey (the limit is 1 a minute)
        QCOMPARE(after.dropped, before.dropped);
        QVERIFY(seq - lastSeenSeq <= 16); // current: the window's worth behind, no backlog
        QVERIFY(!closed);
        QCOMPARE(probe.state(), QProcess::Running);
    }
};

QTEST_GUILESS_MAIN(VideoFlowLoopbackTest)

#include "VideoFlowLoopbackTest.moc"
