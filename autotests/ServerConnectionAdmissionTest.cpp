// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// REVIEW-S1 against a real KRdp::Server on 127.0.0.1: the caps on connections
// that have not authenticated (closed at accept, before TLS), the per-source
// failed-login backoff, and that a password does not outlive its check.
// Raw TCP sockets stand in for stalled handshakes; the real libfreerdp client
// in krdpctl-probe makes the logins. Several loopback source addresses
// (127.0.0.2, ...) stand in for several clients.

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <freerdp/settings.h>

#include "LayoutControl.h"
#include "RdpConnection.h"
#include "Server.h"

using namespace KRdp;
using namespace std::chrono_literals;

namespace
{
const QString TestUser = QStringLiteral("alice");
const QString Password = QStringLiteral("correct horse");
}

class ServerConnectionAdmissionTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QString m_certificate;
    QString m_key;
    int m_displayInfo = 0;
    bool m_passwordClearedAtAuthentication = false;
    bool m_passwordSeenInSettings = false;

    std::unique_ptr<Server> startServer(const ConnectionAdmission::Limits &limits, std::chrono::milliseconds replyDelay = 0ms)
    {
        auto server = std::make_unique<Server>();
        server->setAddress(QHostAddress::LocalHost);
        server->setPort(0);
        server->setTlsCertificate(m_certificate.toStdString());
        server->setTlsCertificateKey(m_key.toStdString());
        server->setUsers({{TestUser, Password}});
        server->setHandshakeTimeout(30s);
        server->setAdmissionLimits(limits);
        m_displayInfo = 0;
        connect(server.get(), &Server::newConnectionCreated, this, [this, replyDelay](RdpConnection *connection) {
            QPointer<RdpConnection> guard(connection);
            connect(connection, &RdpConnection::clientDisplayInfoReceived, connection, [this, guard]() {
                ++m_displayInfo;
                if (guard && guard->freerdpContext()) {
                    const char *stored = freerdp_settings_get_string(guard->freerdpContext()->settings, FreeRDP_Password);
                    m_passwordSeenInSettings = true;
                    m_passwordClearedAtAuthentication = !stored || !*stored;
                }
            }, Qt::QueuedConnection);
            connect(connection, &RdpConnection::controlRecordReceived, connection, [guard, replyDelay](const QJsonObject &record) {
                if (record.value(QLatin1String("type")).toString() == QLatin1String("query")) {
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

    void startProbe(QProcess &probe, quint16 port, const QString &password)
    {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), m_dir.path());
        probe.setProcessEnvironment(environment);
        probe.start(QStringLiteral(KRDPCTL_PROBE),
                    {QStringLiteral("127.0.0.1"), QString::number(port), TestUser, password, QStringLiteral("--query"), QStringLiteral("--timeout"), QStringLiteral("10")});
    }

    int runProbe(quint16 port, const QString &password)
    {
        QProcess probe;
        startProbe(probe, port, password);
        if (!probe.waitForStarted(5000)) {
            return -1;
        }
        QDeadlineTimer deadline(30s);
        while (probe.state() != QProcess::NotRunning && !deadline.hasExpired()) {
            QTest::qWait(50);
        }
        if (probe.state() != QProcess::NotRunning) {
            probe.kill();
            probe.waitForFinished();
            return -3;
        }
        return probe.exitStatus() == QProcess::NormalExit ? probe.exitCode() : -2;
    }

    /** Connects from @p source; the connection is "admitted" when the server keeps it open. */
    std::unique_ptr<QTcpSocket> connectFrom(const QHostAddress &source, quint16 port)
    {
        auto socket = std::make_unique<QTcpSocket>();
        socket->bind(source);
        socket->connectToHost(QHostAddress::LocalHost, port);
        if (!socket->waitForConnected(5000)) {
            return nullptr;
        }
        return socket;
    }
    static bool refused(QTcpSocket &socket)
    {
        QDeadlineTimer deadline(3s);
        while (socket.state() == QAbstractSocket::ConnectedState && !deadline.hasExpired()) {
            QTest::qWait(20);
            socket.waitForReadyRead(1);
        }
        return socket.state() != QAbstractSocket::ConnectedState;
    }
    static bool staysOpen(QTcpSocket &socket, int ms = 600)
    {
        QTest::qWait(ms);
        socket.waitForReadyRead(1);
        return socket.state() == QAbstractSocket::ConnectedState;
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

    void defaultLimits()
    {
        Server server;
        const auto limits = server.admissionLimits();
        QCOMPARE(limits.maxUnauthenticated, 16);
        QCOMPARE(limits.maxUnauthenticatedPerSource, 4);
        QCOMPARE(limits.failuresBeforeBackoff, 3);
        QCOMPARE(limits.backoffStart, 2s);
        QCOMPARE(limits.backoffMax, 60s);
    }

    void perSourceCapClosesTheExtraConnectionAtAccept()
    {
        ConnectionAdmission::Limits limits;
        limits.maxUnauthenticatedPerSource = 2;
        auto server = startServer(limits);
        QVERIFY(server);
        const QHostAddress source(QStringLiteral("127.0.0.1"));
        auto a = connectFrom(source, server->serverPort());
        auto b = connectFrom(source, server->serverPort());
        QVERIFY(a && b);
        QTRY_COMPARE(server->unauthenticatedConnections(), 2);
        auto c = connectFrom(source, server->serverPort());
        QVERIFY(c);
        QVERIFY2(refused(*c), "the third connection from one address was not refused");
        QVERIFY(staysOpen(*a));
        QVERIFY(staysOpen(*b));
        QCOMPARE(server->unauthenticatedConnections(), 2);

        // A slot frees when one of the two goes away.
        a->disconnectFromHost();
        QTRY_COMPARE(server->unauthenticatedConnections(), 1);
        auto d = connectFrom(source, server->serverPort());
        QVERIFY(d);
        QVERIFY(staysOpen(*d));
    }

    void totalCapAppliesAcrossSources()
    {
        ConnectionAdmission::Limits limits;
        limits.maxUnauthenticated = 3;
        limits.maxUnauthenticatedPerSource = 2;
        auto server = startServer(limits);
        QVERIFY(server);
        auto a = connectFrom(QHostAddress(QStringLiteral("127.0.0.2")), server->serverPort());
        auto b = connectFrom(QHostAddress(QStringLiteral("127.0.0.2")), server->serverPort());
        auto c = connectFrom(QHostAddress(QStringLiteral("127.0.0.3")), server->serverPort());
        QVERIFY(a && b && c);
        QTRY_COMPARE(server->unauthenticatedConnections(), 3);
        auto d = connectFrom(QHostAddress(QStringLiteral("127.0.0.4")), server->serverPort());
        QVERIFY(d);
        QVERIFY2(refused(*d), "a fourth unauthenticated connection was not refused");
        QVERIFY(staysOpen(*a));
        QVERIFY(staysOpen(*c));
    }

    void authenticatedSessionsDoNotCountAgainstTheCap()
    {
        ConnectionAdmission::Limits limits;
        limits.maxUnauthenticated = 1;
        limits.maxUnauthenticatedPerSource = 1;
        // The reply comes late, so the authenticated connection stays open while we look.
        auto server = startServer(limits, 3000ms);
        QVERIFY(server);
        QProcess probe;
        startProbe(probe, server->serverPort(), Password);
        QVERIFY(probe.waitForStarted(5000));
        QTRY_COMPARE_WITH_TIMEOUT(m_displayInfo, 1, 20000);
        QTRY_COMPARE(server->unauthenticatedConnections(), 0);
        auto stalled = connectFrom(QHostAddress(QStringLiteral("127.0.0.1")), server->serverPort());
        QVERIFY(stalled);
        QVERIFY2(staysOpen(*stalled), "an authenticated session counted against the unauthenticated cap");
        QCOMPARE(server->unauthenticatedConnections(), 1);
        probe.kill();
        probe.waitForFinished();
    }

    void failedLoginsBackOffAndThenRecover()
    {
        ConnectionAdmission::Limits limits; // 3 failures, 2 s first delay
        auto server = startServer(limits);
        QVERIFY(server);
        const QHostAddress loopback(QStringLiteral("127.0.0.1"));
        QVERIFY(runProbe(server->serverPort(), QStringLiteral("wrong1")) != 0);
        QVERIFY(runProbe(server->serverPort(), QStringLiteral("wrong2")) != 0);
        {
            auto ok = connectFrom(loopback, server->serverPort()); // two failures: still welcome
            QVERIFY(ok);
            QVERIFY(staysOpen(*ok, 300));
        }
        QVERIFY(runProbe(server->serverPort(), QStringLiteral("wrong3")) != 0);
        QElapsedTimer sinceThird;
        sinceThird.start();
        {
            auto blocked = connectFrom(loopback, server->serverPort());
            QVERIFY(blocked);
            QVERIFY2(refused(*blocked), "a connection was not refused right after the third failed login");
        }
        // The delay is 2 s from the third failure.
        QTest::qWait(int(qMax<qint64>(0, 2300 - sinceThird.elapsed())));
        auto again = connectFrom(loopback, server->serverPort());
        QVERIFY(again);
        QVERIFY2(staysOpen(*again, 300), "still refused after the 2 s delay");
        // The right password gets in and clears the record.
        QCOMPARE(runProbe(server->serverPort(), Password), 0);
    }

    void successForgetsEarlierFailures()
    {
        ConnectionAdmission::Limits limits;
        auto server = startServer(limits);
        QVERIFY(server);
        QVERIFY(runProbe(server->serverPort(), QStringLiteral("wrong1")) != 0);
        QVERIFY(runProbe(server->serverPort(), QStringLiteral("wrong2")) != 0);
        QCOMPARE(runProbe(server->serverPort(), Password), 0);
        // Two more failures are only two: no backoff.
        QVERIFY(runProbe(server->serverPort(), QStringLiteral("wrong3")) != 0);
        QVERIFY(runProbe(server->serverPort(), QStringLiteral("wrong4")) != 0);
        auto ok = connectFrom(QHostAddress(QStringLiteral("127.0.0.1")), server->serverPort());
        QVERIFY(ok);
        QVERIFY(staysOpen(*ok, 300));
    }

    void otherSourcesAreNotAffectedByOneSourcesFailures()
    {
        ConnectionAdmission::Limits limits;
        limits.failuresBeforeBackoff = 1;
        auto server = startServer(limits);
        QVERIFY(server);
        QVERIFY(runProbe(server->serverPort(), QStringLiteral("wrong")) != 0); // from 127.0.0.1
        auto blocked = connectFrom(QHostAddress(QStringLiteral("127.0.0.1")), server->serverPort());
        QVERIFY(blocked);
        QVERIFY(refused(*blocked));
        auto other = connectFrom(QHostAddress(QStringLiteral("127.0.0.9")), server->serverPort());
        QVERIFY(other);
        QVERIFY(staysOpen(*other, 300));
    }

    void passwordIsClearedFromTheSettingsOnceChecked()
    {
        m_passwordSeenInSettings = false;
        m_passwordClearedAtAuthentication = false;
        auto server = startServer({});
        QVERIFY(server);
        QCOMPARE(runProbe(server->serverPort(), Password), 0);
        QTRY_VERIFY(m_passwordSeenInSettings);
        QVERIFY2(m_passwordClearedAtAuthentication, "FreeRDP still holds the password after authentication");
    }
};

QTEST_GUILESS_MAIN(ServerConnectionAdmissionTest)
#include "ServerConnectionAdmissionTest.moc"
