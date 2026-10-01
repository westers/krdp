// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Manual native fixture, built only with tests. Production PAM/seat/transport/
// controller/launcher run against a private compositor selected by the root-owned
// worker wrapper. In-memory monitor preferences avoid changing an active user's
// files. Never install this binary or use it as physical-console acceptance.
#include "BrokerAuthentication.h"
#include "ConsoleHostController.h"
#include "ConsoleSeatWatcher.h"
#include "ConsoleWorkerLauncher.h"
#include "EncoderSupport.h"
#include "Server.h"
#include "VideoCodecHost.h"
#include <QCoreApplication>
#include <QHostAddress>
#include <QSocketNotifier>
#include <QSysInfo>
#include <QTimer>
#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("farside-owned-rdp-acceptance"));
    if (geteuid() != 0 || QSysInfo::machineHostName().section(QLatin1Char('.'), 0, 0) != QStringLiteral("sol")) return 1;
    const QString root = QStringLiteral("/tmp/farside-t06-rdp-root");
    const auto auth = KRdp::BrokerAuthentication::readFile(QStringLiteral("/etc/farside/authentication.json"), false);
    if (!auth.policy) { qCritical().noquote() << auth.error; return 1; }
    KRdp::Server server;
    server.setAddress(QHostAddress(QStringLiteral("192.168.48.57"))); server.setPort(3397);
    server.setTlsCertificate(QString(root + QStringLiteral("/console.crt")).toStdString());
    server.setTlsCertificateKey(QString(root + QStringLiteral("/console.key")).toStdString());
    if (!KRdp::BrokerAuthentication::apply(server, auth.policy->route(KRdp::BrokerAuthentication::Desktop::Console))) return 1;
    server.setStandardClientMedia(false);
    KRdp::ConsoleSeatWatcher seat;
    KRdp::ConsoleWorkerLauncher launcher(root + QStringLiteral("/worker-wrapper.sh"));
    launcher.setSessionLookup([&seat](const QString &id) { return seat.session(id); });
    KRdp::ConsoleHostController host(&server,
        {[&launcher](const auto &target, const auto &socket, const auto &token, QString *error) { return launcher.launch(target, socket, token, error); },
         [&launcher](const QString &socket, int signal) { launcher.signalWorker(socket, signal); }}, root + QStringLiteral("/sockets"));
    QObject::connect(&launcher, &KRdp::ConsoleWorkerLauncher::workerExited, &host, &KRdp::ConsoleHostController::workerExited, Qt::QueuedConnection);
    QObject::connect(&seat, &KRdp::ConsoleSeatWatcher::sessionsChanged, &host, [&host, &app](const auto &sessions) {
        const auto adapter = KRdp::ConsoleSeat::adapterFor(sessions);
        if (adapter != KRdp::ConsoleSeat::Adapter::PhysicalUser
            || KRdp::ConsoleSeat::activeSessionUid(sessions, adapter) != std::optional<quint32>(1000)) {
            qWarning("Private owned-output acceptance stopped: owner desktop is unavailable or changed to a greeter.");
            app.exit(2);
            return;
        }
        host.setSeatSessions(sessions);
    });
    host.setUserSettingsReader([](quint32 uid) {
        KRdp::BrokerUserSettings::Result result;
        if (uid != 1000) { result.error = QStringLiteral("fixture requires desktop owner UID1000"); return result; }
        auto &p = result.preferences;
        p.monitorMode = QStringLiteral("virtual"); p.virtualMonitorPolicy = QStringLiteral("extend");
        p.virtualMonitorLayout = QStringLiteral("client"); p.virtualMonitorFallbackSize = QSize(1280, 720);
        p.wakeDisplayOnConnect = false; p.standardClientMedia = false;
        return result;
    });
    host.setVideoCodecHost({KRdp::EncoderSupport::probe(), KRdp::CodecPolicy::SoftwareEncoding::Auto, 0});
    if (!server.start()) return 1;
    host.start(); seat.start();
    // Match production shutdown semantics so the launcher stops its worker and
    // its private output guard can restore the saved inventory.
    static int signals[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, signals) != 0) return 1;
    struct sigaction action{};
    action.sa_handler = [](int) { const int saved = errno; const char byte = 1; [[maybe_unused]] const auto sent = ::write(signals[1], &byte, 1); errno = saved; };
    sigemptyset(&action.sa_mask); action.sa_flags = SA_RESTART;
    ::sigaction(SIGINT, &action, nullptr); ::sigaction(SIGTERM, &action, nullptr);
    QSocketNotifier notifier(signals[0], QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, &QCoreApplication::quit);
    QTimer::singleShot(170000, &app, &QCoreApplication::quit);
    return app.exec();
}
