// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX F4: the console broker's refusal of another account on an unlocked
// desktop reaches a real libfreerdp client (krdpctl-probe, as a stock client)
// as ERRINFO_SERVER_INSUFFICIENT_PRIVILEGES (0x9). On Sol the default refusal
// set the error info and closed without sending it first, so the client
// processed Deactivate All and reported 0xC LOGOFF_BY_USER instead.
//
// A real Server on 127.0.0.1 and the production ConsoleHostController with its
// default refusal. The uid resolver is injected (no PAM here), and Streaming is
// signalled right after authentication because a libfreerdp without H.264
// (hal's) never reaches it on its own; admission runs on that signal.

#include <QDeadlineTimer>
#include <QHostAddress>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <freerdp/error.h>

#include "ConsoleHostController.h"
#include "RdpConnection.h"
#include "Server.h"

using namespace KRdp;
using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace
{
const QString TestUser = u"intruder"_s;
const QString Password = u"correct horse"_s;
constexpr quint32 DesktopOwner = 1000;
constexpr quint32 OtherAccount = 1001;

ConsoleSeat::Session unlockedDesktop(quint32 uid)
{
    ConsoleSeat::Session session;
    session.id = u"3"_s;
    session.uid = uid;
    session.seat = u"seat0"_s;
    session.type = u"wayland"_s;
    session.sessionClass = u"user"_s;
    session.state = u"active"_s;
    session.active = true;
    session.locked = false;
    return session;
}
}

class ConsoleRefusalLoopbackTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_certificate;
    QString m_key;

    int runProbe(quint16 port, QByteArray *err)
    {
        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"XDG_RUNTIME_DIR"_s, m_dir.path());
        probe.setProcessEnvironment(environment);
        // A stock client: no KRDPCTL.
        probe.start(QStringLiteral(KRDPCTL_PROBE),
                    {u"127.0.0.1"_s, QString::number(port), TestUser, Password, u"--silent"_s, u"--no-krdpctl"_s, u"--timeout"_s, u"8"_s});
        if (!probe.waitForStarted(5000)) return -1;
        QDeadlineTimer deadline(40s);
        while (probe.state() != QProcess::NotRunning && !deadline.hasExpired()) QTest::qWait(50);
        if (probe.state() != QProcess::NotRunning) {
            probe.kill();
            probe.waitForFinished();
            return -3;
        }
        *err = probe.readAllStandardError();
        return probe.exitStatus() == QProcess::NormalExit ? probe.exitCode() : -2;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        const QString openssl = QStandardPaths::findExecutable(u"openssl"_s);
        if (openssl.isEmpty()) QSKIP("openssl is needed to make the test certificate");
        m_certificate = m_dir.filePath(u"server.crt"_s);
        m_key = m_dir.filePath(u"server.key"_s);
        QProcess process;
        process.start(openssl, {u"req"_s, u"-x509"_s, u"-newkey"_s, u"rsa:2048"_s, u"-nodes"_s, u"-keyout"_s, m_key, u"-out"_s, m_certificate,
                                u"-days"_s, u"1"_s, u"-subj"_s, u"/CN=krdp-test"_s});
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(process.exitCode(), 0);
    }

    void otherAccountOnUnlockedDesktopGetsInsufficientPrivileges()
    {
        QCOMPARE(quint32(ERRINFO_SERVER_INSUFFICIENT_PRIVILEGES), quint32(0x9));

        Server server;
        server.setAddress(QHostAddress::LocalHost);
        server.setPort(0);
        server.setTlsCertificate(m_certificate.toStdString());
        server.setTlsCertificateKey(m_key.toStdString());
        server.setUsers({{TestUser, Password}});

        QTemporaryDir runtime;
        // No worker launcher: this is about admission, not capture.
        ConsoleHostController host(&server, {}, runtime.path());
        host.setUidResolver([](RdpConnection *) -> std::optional<quint32> {
            return OtherAccount;
        });
        // Default refusal (no setRefuse): the production path under test.
        host.setSeatSessions({unlockedDesktop(DesktopOwner)});

        bool authenticated = false;
        connect(&server, &Server::newConnectionCreated, this, [&authenticated](RdpConnection *connection) {
            QPointer<RdpConnection> guard(connection);
            connect(connection, &RdpConnection::clientDisplayInfoReceived, connection, [guard, &authenticated] {
                if (!guard) return;
                authenticated = guard->isAuthenticated();
                Q_EMIT guard->stateChanged(RdpConnection::State::Streaming);
            }, Qt::QueuedConnection);
        });
        QVERIFY(server.start());

        QByteArray err;
        runProbe(server.serverPort(), &err);
        QVERIFY2(authenticated, err.constData());
        QVERIFY2(err.contains("server ended the session"), err.constData());
        QVERIFY2(err.contains("(0x00000009)"), err.constData());
        QVERIFY2(!err.contains("(0x0000000c)"), err.constData());
    }
};

QTEST_GUILESS_MAIN(ConsoleRefusalLoopbackTest)

#include "ConsoleRefusalLoopbackTest.moc"
