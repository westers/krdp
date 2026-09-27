// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-S1/S2/S3 against a real KRdp::Server on 127.0.0.1, driven by the real
// libfreerdp client in krdpctl-probe (and by a raw TCP socket for the stalled
// handshakes). The "handler" is this test's slot on controlRecordReceived,
// standing in for SessionController.

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "LayoutControl.h"
#include "RdpConnection.h"
#include "Server.h"

using namespace KRdp;
using namespace std::chrono_literals;

namespace
{
const QString TestUser = QStringLiteral("alice");
const QString Password = QStringLiteral("correct horse");

struct Observed {
    QStringList records;
    int displayInfo = 0;
    bool controlChannelAtDisplayInfo = false;
    bool authenticatedBeforeRecord = true;
};
}

class RdpPreAuthLoopbackTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QString m_certificate;
    QString m_key;

    std::unique_ptr<Server> startServer(std::chrono::milliseconds handshakeTimeout, Observed &observed, std::chrono::milliseconds replyDelay = 0ms)
    {
        auto server = std::make_unique<Server>();
        server->setAddress(QHostAddress::LocalHost);
        server->setPort(0);
        server->setTlsCertificate(m_certificate.toStdString());
        server->setTlsCertificateKey(m_key.toStdString());
        // AUD-S3: an entry without a password comes first and must not keep
        // the real user out.
        server->setUsers({{QStringLiteral("blank"), QString()}, {TestUser, Password}});
        server->setHandshakeTimeout(handshakeTimeout);
        connect(server.get(), &Server::newConnectionCreated, this, [&observed, replyDelay](RdpConnection *connection) {
            QPointer<RdpConnection> guard(connection);
            connect(connection, &RdpConnection::clientDisplayInfoReceived, connection, [&observed, guard]() {
                ++observed.displayInfo;
                observed.controlChannelAtDisplayInfo = guard && guard->hasControlChannel();
            }, Qt::QueuedConnection);
            connect(connection, &RdpConnection::controlRecordReceived, connection, [&observed, guard, replyDelay](const QJsonObject &record) {
                const QString type = record.value(QLatin1String("type")).toString();
                observed.records.append(type);
                observed.authenticatedBeforeRecord = observed.authenticatedBeforeRecord && observed.displayInfo > 0;
                if (type == QLatin1String("query")) {
                    QTimer::singleShot(replyDelay, guard, [guard]() {
                        guard->sendControlRecord(LayoutControl::layoutRecord(LayoutControl::Layout{}));
                    });
                }
            }, Qt::QueuedConnection);
        });
        if (!server->start()) {
            return nullptr;
        }
        return server;
    }

    int runProbe(quint16 port, const QString &password, QByteArray *output = nullptr)
    {
        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), m_dir.path());
        probe.setProcessEnvironment(environment);
        probe.start(QStringLiteral(KRDPCTL_PROBE),
                    {QStringLiteral("127.0.0.1"), QString::number(port), TestUser, password, QStringLiteral("--query"), QStringLiteral("--timeout"), QStringLiteral("10")});
        if (!probe.waitForStarted(5000)) {
            return -1;
        }
        // The server lives on this thread: keep its event loop turning.
        QDeadlineTimer deadline(30s);
        while (probe.state() != QProcess::NotRunning && !deadline.hasExpired()) {
            QTest::qWait(50);
        }
        if (probe.state() != QProcess::NotRunning) {
            probe.kill();
            probe.waitForFinished();
            return -3;
        }
        if (output) {
            *output = probe.readAllStandardOutput() + probe.readAllStandardError();
        }
        return probe.exitStatus() == QProcess::NormalExit ? probe.exitCode() : -2;
    }

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

    void configuredUsersSkipEmptyEntries()
    {
        Server server;
        server.setUsers({{QStringLiteral("blank"), QString()}, {QString(), QStringLiteral("x")}, {TestUser, Password}});
        QVERIFY(server.matchesConfiguredUser(TestUser, Password));
        QVERIFY(!server.matchesConfiguredUser(QStringLiteral("blank"), QString()));
        QVERIFY(!server.matchesConfiguredUser(QString(), QStringLiteral("x")));
        QVERIFY(!server.matchesConfiguredUser(TestUser, QStringLiteral("wrong")));
        QVERIFY(!server.matchesConfiguredUser(TestUser, QString()));
    }

    void authenticatedClientReachesTheHandler()
    {
        Observed observed;
        auto server = startServer(15s, observed);
        QVERIFY(server);
        QByteArray output;
        QCOMPARE(runProbe(server->serverPort(), Password, &output), 0);
        QVERIFY2(output.contains("\"layout\""), output.constData());
        QCOMPARE(observed.records, QStringList{QStringLiteral("query")});
        QCOMPARE(observed.displayInfo, 1);
        QVERIFY(observed.controlChannelAtDisplayInfo);
        QVERIFY(observed.authenticatedBeforeRecord);
    }

    void wrongPasswordNeverReachesAnything()
    {
        Observed observed;
        auto server = startServer(15s, observed);
        QVERIFY(server);
        QVERIFY(runProbe(server->serverPort(), QStringLiteral("wrong")) != 0);
        QTest::qWait(200); // let any queued signal land
        QVERIFY(observed.records.isEmpty());
        QCOMPARE(observed.displayInfo, 0);
    }

    void handshakeTimeoutSparesAuthenticatedClients()
    {
        Observed observed;
        // The reply comes well after the handshake timeout: an authenticated
        // connection must not be cut by it.
        auto server = startServer(500ms, observed, 1500ms);
        QVERIFY(server);
        QCOMPARE(runProbe(server->serverPort(), Password), 0);
        QCOMPARE(observed.records, QStringList{QStringLiteral("query")});
    }

    void stalledHandshakeIsClosed_data()
    {
        QTest::addColumn<QByteArray>("sent");
        QTest::newRow("silent") << QByteArray();
        // TPKT + X.224 Connection Request + RDP_NEG_REQ asking for TLS: the
        // server answers and then waits for a ClientHello that never comes,
        // blocked inside freerdp_tls_accept().
        QTest::newRow("tls") << QByteArray::fromHex("030000130ee000000000000100080001000000");
    }

    void stalledHandshakeIsClosed()
    {
        QFETCH(QByteArray, sent);
        Observed observed;
        auto server = startServer(500ms, observed);
        QVERIFY(server);
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, server->serverPort());
        QVERIFY(socket.waitForConnected(5000));
        if (!sent.isEmpty()) {
            socket.write(sent);
            QVERIFY(socket.waitForBytesWritten(5000));
        }
        QElapsedTimer elapsed;
        elapsed.start();
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::UnconnectedState, 10000);
        QVERIFY2(elapsed.elapsed() >= 300, "closed before the timeout");
        QCOMPARE(observed.displayInfo, 0);
    }
};

QTEST_GUILESS_MAIN(RdpPreAuthLoopbackTest)
#include "RdpPreAuthLoopbackTest.moc"
