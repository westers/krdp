// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// FIX-CURSOR end to end on the broker side: a fake capture worker sends Cursor records over the
// real worker socket to a ConsoleWorkerEndpoint; the broker's handling (CursorTracker::apply on
// the connection's KRdp::Cursor, as VirtualSessionTransport and ConsoleHostController do) turns
// them into RDP pointer updates for a real libfreerdp client (krdpctl-probe --log-pointer) on a
// real KRdp::Server on 127.0.0.1. Checked: a new shape is PointerNew + PointerCached, a shape
// seen before is only PointerCached, hidden is PointerSystem(null), the default arrow
// PointerSystem(default), a 96+ px shape PointerLarge, and a shape sent before the connection
// streams arrives once it does.

#include <QLocalSocket>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "ConsoleWorkerEndpoint.h"
#include "ConsoleWorkerOutbox.h"
#include "CursorTracker.h"
#include "RdpConnection.h"
#include "Server.h"
#include "VideoStream.h"

using namespace KRdp;
using Shape = ConsoleWorkerWire::CursorShape;

namespace
{
const QString TestUser = QStringLiteral("alice");
const QString Password = QStringLiteral("correct horse");

Shape bitmap(int size, QColor color, QPoint hotspot)
{
    QImage image(size, size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.fillRect(0, 0, std::max(1, size / 3), size, color);
    painter.end();
    return CursorTracker::fromImage(image, hotspot);
}
}

class CursorLoopbackTest : public QObject
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

    void workerCursorBecomesRdpPointerUpdates()
    {
        // The broker's endpoint and a fake worker on its socket.
        ConsoleWorkerEndpoint endpoint;
        const QByteArray token(24, 't');
        QVERIFY(endpoint.listen(m_dir.filePath(QStringLiteral("worker.sock")), {ConsoleSeat::Adapter::VirtualUser, QStringLiteral("v1"), 1000}, token));
        QLocalSocket worker;
        worker.connectToServer(endpoint.socketName());
        QVERIFY(worker.waitForConnected(1000));
        ConsoleWorkerOutbox outbox([&worker](const QByteArray &bytes) {
            worker.write(bytes);
            worker.flush();
        });
        outbox.hello({QStringLiteral("v1"), 1000, token});
        outbox.ready();
        QTRY_VERIFY(endpoint.ready());

        Server server;
        server.setAddress(QHostAddress::LocalHost);
        server.setPort(0);
        server.setTlsCertificate(m_certificate.toStdString());
        server.setTlsCertificateKey(m_key.toStdString());
        server.setUsers({{TestUser, Password}});
        QPointer<RdpConnection> connection;
        // Before the connection streams (it was only just created): sent as soon as it does.
        const Shape arrow = bitmap(32, Qt::black, {0, 0});
        bool appliedEarly = false;
        connect(&server, &Server::newConnectionCreated, this, [&](RdpConnection *c) {
            connection = c;
            c->videoStream()->setCodecPreference(CodecPreference::Avc420);
            appliedEarly = c->state() != RdpConnection::State::Streaming;
            CursorTracker::apply(*c->cursor(), arrow);
        });
        // The broker: every Cursor record the worker sends becomes the connection's pointer.
        int applied = 0;
        connect(&endpoint, &ConsoleWorkerEndpoint::cursorShapeReceived, this, [&](const Shape &shape) {
            if (connection) {
                CursorTracker::apply(*connection->cursor(), shape);
                ++applied;
            }
        });
        QVERIFY(server.start());

        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), m_dir.path());
        environment.insert(QStringLiteral("HOME"), m_dir.path());
        probe.setProcessEnvironment(environment);
        probe.start(QStringLiteral(KRDPCTL_PROBE),
                    {QStringLiteral("127.0.0.1"), QString::number(server.serverPort()), TestUser, QStringLiteral("-"), QStringLiteral("--silent"),
                     QStringLiteral("--no-krdpctl"), QStringLiteral("--gfx"), QStringLiteral("--gfx-count"), QStringLiteral("--log-pointer"),
                     QStringLiteral("--timeout"), QStringLiteral("60")});
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
        QStringList pointerLines;
        const auto readProbe = [&] {
            const QByteArray more = probe.readAllStandardError();
            log += more;
            for (const QByteArray &line : more.split('\n')) {
                const int at = line.indexOf("pointer: ");
                if (at >= 0) pointerLines << QString::fromUtf8(line.mid(at + 9)).trimmed();
            }
        };
        const auto dump = qScopeGuard([&] {
            if (QTest::currentTestFailed()) qWarning().noquote() << "probe log:\n" << QString::fromUtf8(log.right(6000));
        });
        QTRY_VERIFY_WITH_TIMEOUT(connection, 20000);
        QVERIFY(appliedEarly);
        const auto waitFor = [&](const QStringList &expected) {
            QTRY_VERIFY_WITH_TIMEOUT((readProbe(), pointerLines.size() >= expected.size()), 20000);
            QTest::qWait(200); // nothing more than expected may follow
            readProbe();
            QCOMPARE(pointerLines, expected);
            pointerLines.clear();
        };
        QTRY_VERIFY_WITH_TIMEOUT(connection && connection->state() == RdpConnection::State::Streaming, 30000);
        const QString arrowNew = QStringLiteral("new 32x32 hot 0,0 bpp 32 cache 0 xor %1").arg(32 * 32 * 4);
        waitFor({arrowNew, QStringLiteral("cached 0")});

        // The I-beam over a text field, from the worker: a new shape in the next cache slot.
        const Shape ibeam = bitmap(24, Qt::blue, {12, 11});
        outbox.cursor(ibeam);
        QTRY_COMPARE(applied, 1);
        waitFor({QStringLiteral("new 24x24 hot 12,11 bpp 32 cache 1 xor %1").arg(24 * 24 * 4), QStringLiteral("cached 1")});
        // Back to the arrow: from the cache, no bitmap again.
        outbox.cursor(arrow);
        waitFor({QStringLiteral("cached 0")});
        // A fullscreen video hides it; the arrow comes back from the cache.
        Shape hidden;
        hidden.type = Shape::Type::Hidden;
        outbox.cursor(hidden);
        waitFor({QStringLiteral("system null")});
        outbox.cursor(arrow);
        waitFor({QStringLiteral("cached 0")});
        // The default arrow, then a resize cursor (new) from a 2x desktop: 96+ px is PointerLarge.
        outbox.cursor(Shape{});
        waitFor({QStringLiteral("system default")});
        const Shape resize = bitmap(128, Qt::red, {64, 64});
        outbox.cursor(resize);
        waitFor({QStringLiteral("large 128x128 hot 64,64 bpp 32 cache 2 xor %1").arg(128 * 128 * 4), QStringLiteral("cached 2")});
        QVERIFY2(!log.contains("disconnected") && probe.state() == QProcess::Running, "the client dropped the session");
    }
};

QTEST_MAIN(CursorLoopbackTest)

#include "CursorLoopbackTest.moc"
