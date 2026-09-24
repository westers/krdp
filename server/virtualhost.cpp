// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionHostController.h"
#include "VirtualSessionLaunchPlan.h"
#include "VirtualHostTls.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QSocketNotifier>
#include <filesystem>
#include <csignal>
#include <sys/signalfd.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    // Block termination before Qt/FreeRDP start threads; consume it on the event
    // loop rather than invoking Qt inside an async POSIX signal handler.
    sigset_t terminationMask;
    sigemptyset(&terminationMask); sigaddset(&terminationMask, SIGTERM); sigaddset(&terminationMask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &terminationMask, nullptr)) return 1;
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("krdp-virtual-host"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Persistent virtual-desktop RDP broker; does not attach to the physical console."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("certificate"), QStringLiteral("Absolute TLS certificate path."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("certificate-key"), QStringLiteral("Absolute TLS private-key path."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("address"), QStringLiteral("Numeric listen address."), QStringLiteral("address"), QStringLiteral("0.0.0.0")});
    parser.addOption({QStringLiteral("port"), QStringLiteral("Listen port, independent of the physical-console listener."), QStringLiteral("port"), QStringLiteral("3395")});
    parser.addOption({QStringLiteral("experimental-initial-layout"), QStringLiteral("Enable selected-screen virtual desktop creation for isolated testing.")});
    parser.process(application);
    if (getuid() || geteuid()) { qCritical("Virtual host requires an explicit root service invocation"); return 1; }
    bool validPort = false;
    const auto port = parser.value(QStringLiteral("port")).toUShort(&validPort);
    const QHostAddress address(parser.value(QStringLiteral("address")));
    const auto certificate = parser.value(QStringLiteral("certificate")), key = parser.value(QStringLiteral("certificate-key"));
    const auto readableFile = [](const QString &path) {
        return KRdp::VirtualSessionLaunchPlan::absoluteCleanPath(path) && QFileInfo(path).isFile() && QFileInfo(path).isReadable();
    };
    if (!parser.positionalArguments().isEmpty() || !validPort || !port || address.isNull()
        || !readableFile(certificate) || !readableFile(key)) {
        qCritical("Virtual host requires valid address, port and readable absolute TLS files"); return 1;
    }
    if (!KRdp::validVirtualHostTls(certificate, key)) {
        qCritical("Virtual host requires a matching PEM certificate and unencrypted private key"); return 1;
    }
    const int fd = signalfd(-1, &terminationMask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd < 0) { qCritical("Cannot establish termination signal handling"); return 1; }
    QFile signalFile;
    if (!signalFile.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { close(fd); return 1; }
    QSocketNotifier notifier(fd, QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &application, [&] {
        signalfd_siginfo signal{};
        if (read(fd, &signal, sizeof(signal)) == sizeof(signal)) application.quit();
    });
    QString error;
    auto journal = KRdp::VirtualSessionJournal::open(&error);
    if (!journal) { qCritical().noquote() << error; return 1; }
    // Destruction order: detach hosts/transports, then server, then journal lease.
    KRdp::Server server;
    server.setAddress(address); server.setPort(port);
    server.setTlsCertificate(std::filesystem::path(certificate.toStdString()));
    server.setTlsCertificateKey(std::filesystem::path(key.toStdString()));
    server.setUsePAMAuthentication(true); server.setAllowAnyPAMUser(true);
    KRdp::VirtualSessionHostController host(&server, {});
    if (!host.recover(*journal, &error) || !host.enableIndependentCreates(*journal, {}, {}, parser.isSet(QStringLiteral("experimental-initial-layout")))) {
        qCritical().noquote() << "Virtual host recovery refused:" << error; return 1;
    }
    // Never expose a listener with partially imported or unavailable journal state.
    // Individual failed desktop intents remain listed; they are not replacements.
    if (!server.start()) return 1;
    return application.exec();
}
