// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-D2: devices against a real KRdp::Server on 127.0.0.1, driven by the real
// libfreerdp client in krdpctl-probe with its standard audio channels
// (--media: RDPSND with the silent `fake` backend, AUDIN from the probe's
// built-in paced-silence device).
//
// - A stock client (no KRDPCTL) gets playback and its microphone through
//   standard negotiation alone under StandardClientMedia.
// - Our own client switches devices with KRDPCTL `device` records; every
//   request is answered once, with its requestId.
//
// The server's PipeWire endpoints run against a private daemon (the
// virtual-session graph config: no devices, no policy), never the desktop's.
// This test's slots stand in for SessionController and use the same
// PhysicalDeviceControl helpers it does. Exits 77 (skip) without openssl or a
// private PipeWire daemon.

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <unistd.h>

#include "LayoutControl.h"
#include "PhysicalDeviceControl.h"
#include "RdpConnection.h"
#include "Server.h"

using namespace KRdp;
using namespace std::chrono_literals;

namespace
{
const QString TestUser = QStringLiteral("alice");
const QString Password = QStringLiteral("correct horse");

QJsonObject device(const QString &requestId, const QString &name, const QString &action)
{
    return {{QStringLiteral("type"), QStringLiteral("device")}, {QStringLiteral("v"), 1}, {QStringLiteral("requestId"), requestId},
            {QStringLiteral("device"), name}, {QStringLiteral("action"), action}};
}
}

class DeviceLoopbackTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QString m_certificate;
    QString m_key;
    std::unique_ptr<QTemporaryDir> m_runtime;
    QProcess m_daemon;
    QPointer<RdpConnection> m_connection;

    std::unique_ptr<Server> startServer(bool standardClientMedia)
    {
        auto server = std::make_unique<Server>();
        server->setAddress(QHostAddress::LocalHost);
        server->setPort(0);
        server->setTlsCertificate(m_certificate.toStdString());
        server->setTlsCertificateKey(m_key.toStdString());
        server->setUsers({{TestUser, Password}});
        server->setStandardClientMedia(standardClientMedia);
        connect(server.get(), &Server::newConnectionCreated, this, [this](RdpConnection *connection) {
            m_connection = connection;
            QPointer<RdpConnection> guard(connection);
            // SessionController::onClientDisplayInfo(): capabilities to a KRDPCTL
            // client, the configured build (and StandardClientMedia) for a stock one.
            connect(connection, &RdpConnection::clientDisplayInfoReceived, connection, [guard]() {
                if (!guard) return;
                if (guard->hasControlChannel()) {
                    LayoutControl::ChannelCapabilities capabilities;
                    capabilities.host = QStringLiteral("physical");
                    capabilities.devices = PhysicalDeviceControl::Capabilities;
                    guard->sendControlRecord(LayoutControl::capabilitiesRecord(capabilities));
                } else if (PhysicalDeviceControl::wantsStandardConsent(false, false)) {
                    guard->applyStandardConsent();
                }
            }, Qt::QueuedConnection);
            // SessionController::onControlRecord() / onControlDevice().
            connect(connection, &RdpConnection::controlRecordReceived, connection, [guard](const QJsonObject &incoming) {
                if (!guard) return;
                QJsonObject record = incoming;
                const auto requestId = LayoutControl::takeRequestId(record);
                if (requestId.invalid) {
                    guard->sendControlRecord(LayoutControl::invalidRequestIdRecord());
                } else if (const auto refused = PhysicalDeviceControl::request(*guard, record, requestId.value)) {
                    guard->sendControlRecord(*refused);
                }
            }, Qt::QueuedConnection);
            connect(connection, &RdpConnection::deviceState, connection,
                    [guard](MediaDevice device, const DeviceStatus &status, const QString &requestId) {
                if (guard && guard->hasControlChannel()) {
                    guard->sendControlRecord(PhysicalDeviceControl::stateRecord(device, status, requestId));
                }
            }, Qt::QueuedConnection);
        });
        if (!server->start()) {
            return nullptr;
        }
        return server;
    }

    void startProbe(QProcess &probe, quint16 port, const QStringList &options)
    {
        probe.setProcessEnvironment(QProcessEnvironment::systemEnvironment()); // the private runtime dir
        probe.start(QStringLiteral(KRDPCTL_PROBE), QStringList{QStringLiteral("127.0.0.1"), QString::number(port), TestUser, Password} + options);
    }

    /** Wait for \a probe while turning the server's event loop (it lives on this thread). */
    QByteArray finish(QProcess &probe, std::chrono::milliseconds limit)
    {
        QDeadlineTimer deadline(limit);
        while (probe.state() != QProcess::NotRunning && !deadline.hasExpired()) {
            QTest::qWait(50);
        }
        if (probe.state() != QProcess::NotRunning) {
            probe.kill();
            probe.waitForFinished();
        }
        const QByteArray errors = probe.readAllStandardError();
        if (qEnvironmentVariableIsSet("KRDP_TEST_PROBE_LOG")) qInfo().noquote() << "probe stderr:" << errors;
        return probe.readAllStandardOutput();
    }

    QString writeRecord(const QString &name, const QJsonObject &record)
    {
        const QString path = m_dir.filePath(name + QStringLiteral(".json"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        file.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
        return path;
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

        // A private PipeWire graph for this process and its children.
        const QString parent = QStringLiteral("/run/user/%1").arg(getuid());
        if (!QFileInfo(parent).isDir() || QFileInfo(parent).ownerId() != getuid()) {
            QSKIP("no owned logind runtime directory");
        }
        m_runtime = std::make_unique<QTemporaryDir>(parent + QStringLiteral("/krdp-device-loopback-XXXXXX"));
        QVERIFY(m_runtime->isValid());
        qputenv("PIPEWIRE_RUNTIME_DIR", m_runtime->path().toUtf8());
        qputenv("XDG_RUNTIME_DIR", m_runtime->path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "pipewire-0");
        qputenv("PULSE_RUNTIME_PATH", (m_runtime->path() + QStringLiteral("/pulse")).toUtf8());
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PIPEWIRE_CONFIG_DIR"), QStringLiteral(KRDP_AUDIO_CONFIG_DIR));
        env.insert(QStringLiteral("PIPEWIRE_CONFIG_NAME"), QStringLiteral("virtual-session-pipewire.conf"));
        m_daemon.setProcessEnvironment(env);
        m_daemon.setStandardOutputFile(m_runtime->path() + QStringLiteral("/daemon.log"));
        m_daemon.setStandardErrorFile(m_runtime->path() + QStringLiteral("/daemon.log"), QIODevice::Append);
        m_daemon.start(QStringLiteral(KRDP_PIPEWIRE_EXECUTABLE), QStringList{});
        if (!m_daemon.waitForStarted(3000)) {
            QSKIP("the private PipeWire daemon did not start");
        }
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(m_runtime->path() + QStringLiteral("/pipewire-0")) || m_daemon.state() != QProcess::Running, 10000);
        if (m_daemon.state() != QProcess::Running) {
            QSKIP("the private PipeWire daemon exited");
        }
    }

    void cleanupTestCase()
    {
        if (m_daemon.state() != QProcess::NotRunning) {
            m_daemon.terminate();
            if (!m_daemon.waitForFinished(3000)) m_daemon.kill();
        }
    }

    void stockClientGetsStandardMedia()
    {
        auto server = startServer(true);
        QVERIFY(server);
        QProcess probe;
        startProbe(probe, server->serverPort(), {QStringLiteral("--silent"), QStringLiteral("--no-krdpctl"), QStringLiteral("--media"),
                                                 QStringLiteral("--timeout"), QStringLiteral("30")});
        QVERIFY(probe.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(m_connection, 15000);
        const auto state = [this](MediaDevice device) {
            return m_connection ? m_connection->deviceStatus(device).state : DeviceStatus::State::Off;
        };
        // Standard negotiation is the consent: RDPSND activated, AUDIN accepted.
        QTRY_COMPARE_WITH_TIMEOUT(state(MediaDevice::Playback), DeviceStatus::State::On, 20000);
        QTRY_COMPARE_WITH_TIMEOUT(state(MediaDevice::Microphone), DeviceStatus::State::On, 20000);
        // No RDPECAM add-in: the enumerator is refused and closed after the 5 s deadline.
        QTRY_COMPARE_WITH_TIMEOUT(state(MediaDevice::Camera), DeviceStatus::State::Error, 20000);
        QVERIFY(m_connection && !m_connection->hasControlChannel());
        QVERIFY(m_connection && !m_connection->isOwnClient()); // never an SNDC_CLOSE for it
        probe.kill();
        finish(probe, 5s);
    }

    void standardClientMediaOffLeavesStockClientsWithout()
    {
        auto server = startServer(false);
        QVERIFY(server);
        QProcess probe;
        startProbe(probe, server->serverPort(), {QStringLiteral("--silent"), QStringLiteral("--no-krdpctl"), QStringLiteral("--media"),
                                                 QStringLiteral("--timeout"), QStringLiteral("30")});
        QVERIFY(probe.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(m_connection && m_connection->isAuthenticated(), 15000);
        QTest::qWait(2000);
        for (const auto device : {MediaDevice::Playback, MediaDevice::Microphone, MediaDevice::Camera}) {
            QVERIFY(m_connection);
            QCOMPARE(m_connection->deviceStatus(device).state, DeviceStatus::State::Off);
        }
        probe.kill();
        finish(probe, 5s);
    }

    void ownClientSwitchesDevicesAndGetsEachAnswerOnce()
    {
        auto server = startServer(true);
        QVERIFY(server);
        struct Step {
            QString requestId;
            QJsonObject record;
            QString expectType;
            QString expectState; // `device` state, or the error code
        };
        auto invalid = device(QStringLiteral("bad1"), QStringLiteral("microphone"), QStringLiteral("on"));
        invalid.insert(QStringLiteral("extra"), true);
        const QList<Step> steps{
            {QStringLiteral("m1"), device(QStringLiteral("m1"), QStringLiteral("microphone"), QStringLiteral("on")), QStringLiteral("device"), QStringLiteral("on")},
            {QStringLiteral("p1"), device(QStringLiteral("p1"), QStringLiteral("playback"), QStringLiteral("on")), QStringLiteral("device"), QStringLiteral("on")},
            {QStringLiteral("q1"), device(QStringLiteral("q1"), QStringLiteral("microphone"), QStringLiteral("query")), QStringLiteral("device"), QStringLiteral("on")},
            {QStringLiteral("x1"), device(QStringLiteral("x1"), QStringLiteral("microphone"), QStringLiteral("reselect")), QStringLiteral("error"), QStringLiteral("unsupported")},
            {QStringLiteral("bad1"), invalid, QStringLiteral("error"), QStringLiteral("invalid")},
            {QStringLiteral("m2"), device(QStringLiteral("m2"), QStringLiteral("microphone"), QStringLiteral("off")), QStringLiteral("device"), QStringLiteral("off")},
            {QStringLiteral("p2"), device(QStringLiteral("p2"), QStringLiteral("playback"), QStringLiteral("off")), QStringLiteral("device"), QStringLiteral("off")},
            {QStringLiteral("m3"), device(QStringLiteral("m3"), QStringLiteral("microphone"), QStringLiteral("on")), QStringLiteral("device"), QStringLiteral("on")},
            {QStringLiteral("m4"), device(QStringLiteral("m4"), QStringLiteral("microphone"), QStringLiteral("off")), QStringLiteral("device"), QStringLiteral("off")},
            // Playback again after SNDC_CLOSE: the format is selected again.
            {QStringLiteral("p3"), device(QStringLiteral("p3"), QStringLiteral("playback"), QStringLiteral("on")), QStringLiteral("device"), QStringLiteral("on")},
            {QStringLiteral("c1"), device(QStringLiteral("c1"), QStringLiteral("camera"), QStringLiteral("query")), QStringLiteral("device"), QStringLiteral("off")},
        };
        QStringList options{QStringLiteral("--silent"), QStringLiteral("--media"), QStringLiteral("--raw-gap"), QStringLiteral("1500"),
                            QStringLiteral("--timeout"), QStringLiteral("24")};
        for (const auto &step : steps) {
            const QString path = writeRecord(step.requestId, step.record);
            QVERIFY(!path.isEmpty());
            options << QStringLiteral("--raw") << path;
        }
        QProcess probe;
        startProbe(probe, server->serverPort(), options);
        QVERIFY(probe.waitForStarted(5000));
        const QByteArray output = finish(probe, 60s);

        QHash<QString, QList<QJsonObject>> replies;
        bool capabilities = false;
        for (const QByteArray &line : output.split('\n')) {
            if (!line.startsWith("reply: ")) continue;
            const QJsonObject reply = QJsonDocument::fromJson(line.mid(7)).object();
            if (reply.value(QStringLiteral("type")) == QLatin1String("capabilities")) {
                capabilities = reply.value(QStringLiteral("devices")).toObject().value(QStringLiteral("camera")).toObject().value(QStringLiteral("reselect")).toBool();
                continue;
            }
            replies[reply.value(QStringLiteral("requestId")).toString()].append(reply);
        }
        QVERIFY2(capabilities, output.constData());
        for (const auto &step : steps) {
            const auto answers = replies.value(step.requestId);
            QVERIFY2(answers.size() == 1, qPrintable(QStringLiteral("%1: %2 answers\n%3").arg(step.requestId).arg(answers.size()).arg(QString::fromUtf8(output))));
            const auto &answer = answers.first();
            QCOMPARE(answer.value(QStringLiteral("type")).toString(), step.expectType);
            QCOMPARE(answer.value(step.expectType == QLatin1String("error") ? QStringLiteral("code") : QStringLiteral("state")).toString(), step.expectState);
        }
        QVERIFY(m_connection.isNull() || m_connection->isOwnClient());
    }
};

QTEST_GUILESS_MAIN(DeviceLoopbackTest)
#include "DeviceLoopbackTest.moc"
