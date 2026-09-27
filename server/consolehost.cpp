// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <cerrno>
#include <csignal>
#include <filesystem>
#include <sys/socket.h>
#include <unistd.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSocketNotifier>

#include <RdpConnection.h>
#include <Server.h>

#include "ConsoleHostController.h"
#include "ConsoleSeatWatcher.h"
#include "ConsoleWorkerLauncher.h"

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("krdp-console-host"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Persistent physical-console RDP host."));
    parser.addHelpOption();
    const QCommandLineOption workerOption(QStringLiteral("worker"), QStringLiteral("Installed krdp-console-worker executable."), QStringLiteral("path"));
    const QCommandLineOption certificateOption(QStringLiteral("certificate"), QStringLiteral("TLS certificate."), QStringLiteral("path"));
    const QCommandLineOption keyOption(QStringLiteral("certificate-key"), QStringLiteral("TLS private key."), QStringLiteral("path"));
    const QCommandLineOption addressOption(QStringLiteral("address"), QStringLiteral("Listen address."), QStringLiteral("address"), QStringLiteral("0.0.0.0"));
    const QCommandLineOption portOption(QStringLiteral("port"), QStringLiteral("Listen port."), QStringLiteral("port"), QStringLiteral("3389"));
    const QCommandLineOption runtimeOption(QStringLiteral("runtime-directory"), QStringLiteral("Host-owned worker socket directory."), QStringLiteral("path"), QStringLiteral("/run/krdp-console"));
    const QCommandLineOption audioPriorityOption(QStringLiteral("prefer-audio-quality"), QStringLiteral("Default to audio-first congestion steering for the controlling client (live client overrides allowed)."));
    parser.addOptions({workerOption, certificateOption, keyOption, addressOption, portOption, runtimeOption, audioPriorityOption});
    parser.process(application);

    if (geteuid() != 0) {
        qCritical("krdp-console-host must run as root to enter selected logind sessions");
        return 1;
    }

    bool portOk = false;
    const quint16 port = parser.value(portOption).toUShort(&portOk);
    if (!portOk || port == 0 || parser.value(workerOption).isEmpty() || parser.value(certificateOption).isEmpty() || parser.value(keyOption).isEmpty()) {
        parser.showHelp(1);
    }

    KRdp::Server server;
    server.setAddress(QHostAddress(parser.value(addressOption)));
    server.setPort(port);
    server.setTlsCertificate(std::filesystem::path(parser.value(certificateOption).toStdString()));
    server.setTlsCertificateKey(std::filesystem::path(parser.value(keyOption).toStdString()));
    server.setUsePAMAuthentication(true);
    server.setAllowAnyPAMUser(true);

    KRdp::ConsoleSeatWatcher seat;
    KRdp::ConsoleWorkerLauncher launcher(parser.value(workerOption));
    launcher.setSessionLookup([&seat](const QString &id) {
        return seat.session(id);
    });
    KRdp::ConsoleHostController host(&server,
                                     {[&launcher](const auto &target, const auto &socketName, const auto &token, QString *error) {
                                          return launcher.launch(target, socketName, token, error);
                                      },
                                      [&launcher](const QString &socketName, int signal) {
                                          launcher.signalWorker(socketName, signal);
                                      }},
                                     parser.value(runtimeOption));
    QObject::connect(&launcher, &KRdp::ConsoleWorkerLauncher::workerExited,
                     &host, &KRdp::ConsoleHostController::workerExited, Qt::QueuedConnection);
    QObject::connect(&seat, &KRdp::ConsoleSeatWatcher::sessionsChanged, &host, &KRdp::ConsoleHostController::setSeatSessions);
    host.setAudioPriorityDefault(parser.isSet(audioPriorityOption));
    if (!server.start()) {
        return 1;
    }
    host.start();
    seat.start();
    // Self-pipe: QCoreApplication::quit() is not async-signal-safe. On quit
    // the launcher's destructor sends SIGTERM to the worker, which restores
    // the outputs it changed before it exits.
    static int signalPipe[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, signalPipe) == 0) {
        struct sigaction action{};
        action.sa_handler = [](int) {
            const int saved = errno;
            const char byte = 1;
            [[maybe_unused]] const auto written = ::write(signalPipe[1], &byte, 1);
            errno = saved;
        };
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        ::sigaction(SIGINT, &action, nullptr);
        ::sigaction(SIGTERM, &action, nullptr);
        auto *notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, &application);
        QObject::connect(notifier, &QSocketNotifier::activated, &application, [] {
            QCoreApplication::quit();
        });
    }
    return application.exec();
}
