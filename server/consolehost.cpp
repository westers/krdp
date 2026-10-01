#include "FarsideEnv.h"
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
#include <QTimer>

#include <chrono>

#include <RdpConnection.h>
#include <Server.h>

#include "ConsoleHostController.h"
#include "ConsoleSeatWatcher.h"
#include "ConsoleWorkerLauncher.h"
#include "BrokerAuthentication.h"
#include "VaapiDriverMode.h"
#include "HostCertificate.h"
#include "VideoCodecHost.h"

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    Farside::warnLegacyEnvironment();
    application.setApplicationName(QStringLiteral("farside-console-host"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Persistent physical-console RDP host."));
    parser.addHelpOption();
    const QCommandLineOption workerOption(QStringLiteral("worker"), QStringLiteral("Installed farside-console-worker executable."), QStringLiteral("path"));
    const QCommandLineOption authenticationOption(QStringLiteral("authentication-policy"), QStringLiteral("Root-owned shared Console/Virtual authentication policy; restart to reload."), QStringLiteral("path"), QStringLiteral("/etc/farside/authentication.json"));
    const QCommandLineOption certificateOption(QStringLiteral("certificate"), QStringLiteral("TLS certificate."), QStringLiteral("path"));
    const QCommandLineOption keyOption(QStringLiteral("certificate-key"), QStringLiteral("TLS private key."), QStringLiteral("path"));
    const QCommandLineOption addressOption(QStringLiteral("address"), QStringLiteral("Listen address."), QStringLiteral("address"), QStringLiteral("0.0.0.0"));
    const QCommandLineOption portOption(QStringLiteral("port"), QStringLiteral("Listen port."), QStringLiteral("port"), QStringLiteral("3389"));
    const QCommandLineOption runtimeOption(QStringLiteral("runtime-directory"), QStringLiteral("Host-owned worker socket directory."), QStringLiteral("path"), QStringLiteral("/run/farside-console"));
    const QCommandLineOption audioPriorityOption(QStringLiteral("prefer-audio-quality"), QStringLiteral("Default to audio-first congestion steering for the controlling client: true or false (live client overrides allowed)."), QStringLiteral("enabled"), QStringLiteral("false"));
    const QCommandLineOption qualityOption(QStringLiteral("quality"), QStringLiteral("Maximum video quality, 0 to 100."), QStringLiteral("quality"), QStringLiteral("80"));
    const QCommandLineOption adaptiveQualityOption(QStringLiteral("adaptive-quality"), QStringLiteral("Steer video quality under link congestion: true or false."), QStringLiteral("enabled"), QStringLiteral("false"));
    const QCommandLineOption standardMediaOption(QStringLiteral("standard-client-media"), QStringLiteral("Enable standard RDP audio and camera consent: true or false."), QStringLiteral("enabled"), QStringLiteral("true"));
    const QCommandLineOption cameraLoopbackOption(QStringLiteral("camera-loopback-device"), QStringLiteral("V4L2 loopback path in the desktop worker, or none."), QStringLiteral("path"), QStringLiteral("none"));
    const QCommandLineOption softwareEncodingOption(QStringLiteral("software-encoding"), QStringLiteral("SoftwareEncoding for private codecs: auto, never or prefer."), QStringLiteral("mode"), QStringLiteral("auto"));
    const QCommandLineOption av1TilesOption(QStringLiteral("av1-tiles"), QStringLiteral("AV1 tiles for the Farside client: auto, 1, 2, 4, 8 or 16."), QStringLiteral("tiles"), QStringLiteral("auto"));
    const QCommandLineOption vaapiDriverOption(QStringLiteral("vaapi-driver"), QStringLiteral("VaapiDriverMode for the capture workers: auto, off, radeonsi, iHD or i965."), QStringLiteral("mode"), QStringLiteral("auto"));
    parser.addOptions({workerOption, authenticationOption, certificateOption, keyOption, addressOption, portOption, runtimeOption, audioPriorityOption, qualityOption, adaptiveQualityOption, standardMediaOption, cameraLoopbackOption, softwareEncodingOption, av1TilesOption, vaapiDriverOption});
    parser.process(application);

    if (geteuid() != 0) {
        qCritical("farside-console-host must run as root to enter selected logind sessions");
        return 1;
    }
    const auto authentication = KRdp::BrokerAuthentication::readFile(parser.value(authenticationOption), parser.isSet(authenticationOption));
    if (!authentication.policy) {
        qCritical().noquote() << authentication.error;
        return 1;
    }

    bool portOk = false;
    const quint16 port = parser.value(portOption).toUShort(&portOk);
    bool qualityOk = false;
    const int quality = parser.value(qualityOption).toInt(&qualityOk);
    const auto adaptiveValue = parser.value(adaptiveQualityOption);
    const auto audioPriorityValue = parser.value(audioPriorityOption);
    const auto standardMediaValue = parser.value(standardMediaOption);
    const auto cameraLoopback = parser.value(cameraLoopbackOption);
    const auto softwareEncoding = KRdp::parseHostSoftwareEncoding(parser.value(softwareEncodingOption));
    const auto vaapiDriver = KRdp::VaapiDriverMode::normalize(parser.value(vaapiDriverOption));
    if (!portOk || port == 0 || parser.value(workerOption).isEmpty() || parser.value(certificateOption).isEmpty() || parser.value(keyOption).isEmpty()
        || !qualityOk || quality < 0 || quality > 100
        || (adaptiveValue != QLatin1String("true") && adaptiveValue != QLatin1String("false"))
        || (audioPriorityValue != QLatin1String("true") && audioPriorityValue != QLatin1String("false"))
        || (standardMediaValue != QLatin1String("true") && standardMediaValue != QLatin1String("false"))
        || (cameraLoopback != QLatin1String("none") && !cameraLoopback.startsWith(QLatin1String("/dev/")))
        || !softwareEncoding || !vaapiDriver) {
        parser.showHelp(1);
    }
    // AUD-FIX7: the host keeps its own certificate valid (created when missing, renewed when
    // expired or within 30 days of it, a valid one kept), like krdpserver's (AUD-K3).
    const KRdp::ServerCertificate::Paths certificatePaths{parser.value(certificateOption), parser.value(keyOption)};
    if (!KRdp::ensureHostCertificate(certificatePaths, "farside-console-host")) {
        return 1;
    }

    KRdp::Server server;
    server.setAddress(QHostAddress(parser.value(addressOption)));
    server.setPort(port);
    server.setTlsCertificate(std::filesystem::path(parser.value(certificateOption).toStdString()));
    server.setTlsCertificateKey(std::filesystem::path(parser.value(keyOption).toStdString()));
    if (!KRdp::BrokerAuthentication::apply(server, authentication.policy->route(KRdp::BrokerAuthentication::Desktop::Console))) return 1;
    server.setStandardClientMedia(standardMediaValue == QLatin1String("true"));
    if (cameraLoopback != QLatin1String("none")) server.setCameraLoopbackDevice(cameraLoopback);

    KRdp::ConsoleSeatWatcher seat;
    KRdp::ConsoleWorkerLauncher launcher(parser.value(workerOption));
    launcher.setVaapiDriverMode(*vaapiDriver);
    qInfo().noquote() << "Console host worker VaapiDriverMode:" << *vaapiDriver;
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
    host.setAudioPriorityDefault(audioPriorityValue == QLatin1String("true"));
    host.setUserSettingsReader(KRdp::BrokerUserSettings::readUser);
    host.setVideoQualityPolicy(quint8(quality), adaptiveValue == QLatin1String("true"));
    // AUD-FIX7: what `capabilities.video` offers and the controlling connection's codec policy
    // starts from; the worker probes its own encoders and replaces this once it reports.
    KRdp::EncoderSupport::applyProcessOverrides();
    const KRdp::VideoCodecHost videoHost{KRdp::EncoderSupport::probe(), *softwareEncoding,
                                         KRdp::parseHostAv1Tiles(parser.value(av1TilesOption), "farside-console-host")};
    qInfo().noquote() << "Console host video encoders:" << KRdp::EncoderSupport::describe(videoHost.probe) << "- SoftwareEncoding"
                      << KRdp::CodecPolicy::softwareEncodingName(videoHost.mode) << "- AV1 tiles" << KRdp::CodecPolicy::av1TilesName(videoHost.av1Tiles);
    host.setVideoCodecHost(videoHost);
    if (!server.start()) {
        return 1;
    }
    host.start();
    seat.start();
    QTimer certificateRenewal;
    certificateRenewal.setInterval(std::chrono::hours(12));
    QObject::connect(&certificateRenewal, &QTimer::timeout, &application, [certificatePaths] {
        KRdp::ensureHostCertificate(certificatePaths, "farside-console-host");
    });
    certificateRenewal.start();
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
