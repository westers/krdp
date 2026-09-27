// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// KRDPCTL v2 against a real KRdp::Server on 127.0.0.1 with the production virtual-session
// broker transport (VirtualSessionTransport) behind it, driven by krdpctl-probe (the real
// libfreerdp client): `capabilities` comes first and only to a client that opened KRDPCTL,
// every reply echoes its requestId, a client without KRDPCTL gets no custom data at all,
// and a refused login is reported with ERRINFO_SERVER_INSUFFICIENT_PRIVILEGES.

#include <QDeadlineTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "RdpConnection.h"
#include "Server.h"
#include "VirtualSessionControl.h"
#include "VirtualSessionSupervisor.h"
#include "VirtualSessionTransport.h"
#include "VirtualStockClient.h"

using namespace KRdp;
using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace
{
const QString TestUser = u"alice"_s;
const QString Password = u"correct horse"_s;

struct Seen {
    int connections = 0;
    bool reachedStreaming = false;
    bool controlChannel = false;
    bool authenticated = false;
};
}

class KrdpctlV2LoopbackTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_certificate;
    QString m_key;
    std::unique_ptr<VirtualSessionSupervisor> m_supervisor;
    std::unique_ptr<VirtualSessionControl> m_control;
    std::vector<std::unique_ptr<VirtualSessionTransport>> m_transports;
    quint64 m_sequence = 0;
    quint64 m_nextClient = 0;
    /** Called once the client authenticated (on the main thread). */
    std::function<void(RdpConnection *)> m_onAuthenticated;
    /** Every MS-RDPEDISP layout the server received. */
    QList<QList<VideoMonitor>> m_displayLayouts;

    std::unique_ptr<Server> startServer(Seen &seen)
    {
        auto server = std::make_unique<Server>();
        server->setAddress(QHostAddress::LocalHost);
        server->setPort(0);
        server->setTlsCertificate(m_certificate.toStdString());
        server->setTlsCertificateKey(m_key.toStdString());
        server->setUsers({{TestUser, Password}});
        connect(server.get(), &Server::newConnectionCreated, this, [this, &seen](RdpConnection *connection) {
            ++seen.connections;
            // The broker's own transport: it sends `capabilities` and answers every record.
            m_transports.push_back(std::make_unique<VirtualSessionTransport>(++m_nextClient, connection, *m_control, VirtualSessionTransport::Resolve{}, m_sequence));
            QPointer<RdpConnection> guard(connection);
            connect(connection, &RdpConnection::clientDisplayInfoReceived, this, [this, &seen, guard] {
                if (!guard) return;
                seen.authenticated = guard->isAuthenticated();
                seen.controlChannel = guard->hasControlChannel();
                if (m_onAuthenticated) m_onAuthenticated(guard);
            }, Qt::QueuedConnection);
            connect(connection, &RdpConnection::displayLayoutRequested, this, [this](const QList<VideoMonitor> &monitors) {
                m_displayLayouts.append(monitors);
            }, Qt::QueuedConnection);
            connect(connection, &RdpConnection::stateChanged, this, [&seen](RdpConnection::State state) {
                if (state == RdpConnection::State::Streaming) seen.reachedStreaming = true;
            }, Qt::QueuedConnection);
        });
        if (!server->start()) return nullptr;
        return server;
    }

    QString writeJson(const QString &name, const QJsonObject &object)
    {
        const QString path = m_dir.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return {};
        file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
        return path;
    }

    int runProbe(quint16 port, const QString &password, const QStringList &options, QByteArray *out, QByteArray *err)
    {
        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"XDG_RUNTIME_DIR"_s, m_dir.path());
        probe.setProcessEnvironment(environment);
        probe.start(QStringLiteral(KRDPCTL_PROBE), QStringList{u"127.0.0.1"_s, QString::number(port), TestUser, password} + options);
        if (!probe.waitForStarted(5000)) return -1;
        QDeadlineTimer deadline(40s);
        while (probe.state() != QProcess::NotRunning && !deadline.hasExpired()) QTest::qWait(50);
        if (probe.state() != QProcess::NotRunning) {
            probe.kill();
            probe.waitForFinished();
            return -3;
        }
        *out = probe.readAllStandardOutput();
        *err = probe.readAllStandardError();
        return probe.exitStatus() == QProcess::NormalExit ? probe.exitCode() : -2;
    }

    static QList<QJsonObject> replies(const QByteArray &out)
    {
        QList<QJsonObject> records;
        for (const QByteArray &line : out.split('\n')) {
            if (!line.startsWith("reply: ")) continue;
            records.append(QJsonDocument::fromJson(line.mid(7)).object());
        }
        return records;
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
        m_supervisor = std::make_unique<VirtualSessionSupervisor>([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> { return std::nullopt; });
        m_control = std::make_unique<VirtualSessionControl>(*m_supervisor, VirtualSessionControl::Release{});
    }
    void cleanup()
    {
        m_transports.clear();
        m_onAuthenticated = {};
        m_displayLayouts.clear();
    }

    void capabilitiesFirstThenRepliesEchoRequestIds()
    {
        Seen seen;
        auto server = startServer(seen);
        QVERIFY(server);
        const QString list = writeJson(u"list.json"_s, {{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"list-1"_s},
                                                      {u"requestId"_s, u"list-1"_s}, {u"action"_s, u"list"_s}});
        const QString bogus = writeJson(u"bogus.json"_s, {{u"type"_s, u"bogus"_s}, {u"v"_s, 1}, {u"requestId"_s, u"r9"_s}});
        const QString mismatch = writeJson(u"mismatch.json"_s, {{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"a"_s},
                                                              {u"requestId"_s, u"b"_s}, {u"action"_s, u"list"_s}});
        QByteArray out, err;
        const int code = runProbe(server->serverPort(), Password,
                                  {u"--silent"_s, u"--raw"_s, list, u"--raw"_s, bogus, u"--raw"_s, mismatch, u"--timeout"_s, u"6"_s}, &out, &err);
        QVERIFY2(code == 0, err.constData());
        QVERIFY(seen.authenticated && seen.controlChannel);
        const auto records = replies(out);
        QVERIFY2(records.size() >= 4, out.constData());
        QCOMPARE(records.first().value(u"type"_s).toString(), u"capabilities"_s);
        QCOMPARE(records.first().value(u"protocol"_s).toInt(), 2);
        QCOMPARE(records.first().value(u"host"_s).toString(), u"virtual"_s);
        QVERIFY(!records.first().contains(u"requestId"_s));
        QCOMPARE(std::count_if(records.cbegin(), records.cend(), [](const auto &r) { return r.value(u"type"_s) == u"capabilities"_s; }), 1);
        const auto find = [&records](const QString &requestId) {
            for (const auto &record : records)
                if (record.value(u"requestId"_s).toString() == requestId) return record;
            return QJsonObject{};
        };
        // The broker's typed reply (refused: no PAM identity here) and its generic error both echo.
        QCOMPARE(find(u"list-1"_s).value(u"type"_s).toString(), u"virtual-session"_s);
        QCOMPARE(find(u"list-1"_s).value(u"id"_s).toString(), u"list-1"_s);
        QCOMPARE(find(u"r9"_s).value(u"type"_s).toString(), u"error"_s);
        // requestId != id: refused, uncorrelated.
        bool refused = false;
        for (const auto &record : records)
            refused = refused || (record.value(u"message"_s) == u"invalid requestId"_s && !record.contains(u"requestId"_s));
        QVERIFY2(refused, out.constData());
    }

    void clientWithoutKrdpctlGetsNoCustomData()
    {
        Seen seen;
        auto server = startServer(seen);
        QVERIFY(server);
        QByteArray out, err;
        runProbe(server->serverPort(), Password, {u"--silent"_s, u"--no-krdpctl"_s, u"--gfx"_s, u"--timeout"_s, u"6"_s}, &out, &err);
        QTRY_VERIFY(seen.authenticated);
        QVERIFY(!seen.controlChannel);
        QVERIFY2(!out.contains("capabilities") && !out.contains("reply:"), out.constData());
        QVERIFY(!err.contains("KRDPCTL connected"));
        // The ordinary RDP path is untouched: with an H.264-capable libfreerdp the session
        // streams. A distribution FreeRDP built WITH_GFX_H264=OFF (hal's) cannot negotiate
        // AVC420, which the server refuses before streaming - not something KRDPCTL decides.
        if (err.contains("0x0000112f") || err.contains("GRAPHICS_SUBSYSTEM_FAILED")) {
            qWarning("the probe's libfreerdp has no H.264: streaming not verified on this host");
        } else {
            QVERIFY2(seen.reachedStreaming, err.constData());
        }
    }

    // AUD-D4: the broker's refusals of a stock client reach a real libfreerdp
    // client as the standard Set Error Info code it prints.
    void stockClientRefusalIsAStandardErrorInfo_data()
    {
        QTest::addColumn<int>("refusal");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("policy") << int(VirtualStockClient::Refusal::Policy) << QByteArray("0x00000007");
        QTest::newRow("no free slot") << int(VirtualStockClient::Refusal::NoFreeSlot) << QByteArray("0x00000408");
        QTest::newRow("displaced") << int(VirtualStockClient::Refusal::Displaced) << QByteArray("0x00000005");
    }
    void stockClientRefusalIsAStandardErrorInfo()
    {
        QFETCH(int, refusal);
        QFETCH(QByteArray, expected);
        Seen seen;
        auto server = startServer(seen);
        QVERIFY(server);
        m_onAuthenticated = [refusal](RdpConnection *connection) {
            connection->closeWithErrorInfo(VirtualStockClient::errorInfo(VirtualStockClient::Refusal(refusal)));
        };
        QByteArray out, err;
        runProbe(server->serverPort(), Password, {u"--silent"_s, u"--no-krdpctl"_s, u"--timeout"_s, u"6"_s}, &out, &err);
        QVERIFY(seen.authenticated);
        QVERIFY2(err.contains("server ended the session") && err.contains(expected), err.constData());
    }

    // MS-RDPEDISP: a stock client's window resize reaches the broker (the
    // transport turns it into the worker resize; VirtualSessionTransportTest).
    void displayControlLayoutReachesTheBroker()
    {
        Seen seen;
        auto server = startServer(seen);
        QVERIFY(server);
        QByteArray out, err;
        runProbe(server->serverPort(), Password, {u"--silent"_s, u"--no-krdpctl"_s, u"--disp"_s, u"1600x900"_s, u"--timeout"_s, u"6"_s}, &out, &err);
        QVERIFY2(err.contains("DISP caps: MaxNumMonitors 16"), err.constData());
        QVERIFY2(err.contains("DISP layout 1600x900 sent (0)"), err.constData());
        QTRY_COMPARE(m_displayLayouts.size(), 1);
        QCOMPARE(m_displayLayouts.first().size(), 1);
        QCOMPARE(m_displayLayouts.first().first().geometry, QRect(0, 0, 1600, 900));
        QVERIFY(m_displayLayouts.first().first().primary);
        QVERIFY(!out.contains("capabilities")); // still no custom data for a stock client
    }

    void refusedLoginIsReportedWithAStandardCode()
    {
        Seen seen;
        auto server = startServer(seen);
        QVERIFY(server);
        QByteArray out, err;
        const int code = runProbe(server->serverPort(), u"wrong"_s, {u"--silent"_s, u"--timeout"_s, u"6"_s}, &out, &err);
        QVERIFY(code != 0);
        QVERIFY(!seen.authenticated);
        // ERRINFO_SERVER_INSUFFICIENT_PRIVILEGES, as a connect failure or a server-ended session.
        QVERIFY2(err.contains("0x00000009") || err.contains("insufficient access privileges") || err.contains("INSUFFICIENT_PRIVILEGES"), err.constData());
        QVERIFY(!out.contains("capabilities"));
    }
};

QTEST_GUILESS_MAIN(KrdpctlV2LoopbackTest)
#include "KrdpctlV2LoopbackTest.moc"
