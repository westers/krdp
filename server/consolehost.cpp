// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <csignal>
#include <filesystem>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>

#include <RdpConnection.h>
#include <Server.h>

#include "ConsoleHostController.h"
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
    parser.addOptions({workerOption, certificateOption, keyOption, addressOption, portOption, runtimeOption});
    parser.process(application);

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

    KRdp::ConsoleWorkerLauncher launcher(parser.value(workerOption));
    KRdp::ConsoleHostController host(&server,
                                     [&launcher](const auto &target, const auto &socketName, const auto &token, QString *error) {
                                         return launcher.launch(target, socketName, token, error);
                                     },
                                     parser.value(runtimeOption));
    if (!server.start()) {
        return 1;
    }
    host.start();
    std::signal(SIGINT, [](int) { QCoreApplication::quit(); });
    std::signal(SIGTERM, [](int) { QCoreApplication::quit(); });
    return application.exec();
}
