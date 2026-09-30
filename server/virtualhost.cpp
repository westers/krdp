#include "FarsideEnv.h"
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionHostController.h"
#include "VirtualSessionLaunchPlan.h"
#include "VirtualHostTls.h"
#include "HostCertificate.h"
#include "VideoCodecHost.h"
#include <QCoreApplication>
#include <QTimer>
#include <chrono>
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
    Farside::warnLegacyEnvironment();
    application.setApplicationName(QStringLiteral("farside-virtual-host"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Persistent virtual-desktop RDP broker; does not attach to the physical console."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("certificate"), QStringLiteral("Absolute TLS certificate path."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("certificate-key"), QStringLiteral("Absolute TLS private-key path."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("address"), QStringLiteral("Numeric listen address."), QStringLiteral("address"), QStringLiteral("0.0.0.0")});
    parser.addOption({QStringLiteral("port"), QStringLiteral("Listen port, independent of the physical-console listener."), QStringLiteral("port"), QStringLiteral("3395")});
    parser.addOption({QStringLiteral("quality"), QStringLiteral("Maximum video quality, 0 to 100."), QStringLiteral("quality"), QStringLiteral("80")});
    parser.addOption({QStringLiteral("adaptive-quality"), QStringLiteral("Steer video quality under link congestion: true or false."), QStringLiteral("enabled"), QStringLiteral("false")});
    parser.addOption({QStringLiteral("prefer-audio-quality"), QStringLiteral("Default to audio-first congestion steering for the attached client: true or false (live client overrides allowed)."), QStringLiteral("enabled"), QStringLiteral("false")});
    parser.addOption({QStringLiteral("standard-client-media"), QStringLiteral("Enable standard RDP audio and camera consent: true or false."), QStringLiteral("enabled"), QStringLiteral("true")});
    parser.addOption({QStringLiteral("camera-loopback-device"), QStringLiteral("V4L2 loopback path in the desktop worker, or none."), QStringLiteral("path"), QStringLiteral("none")});
    parser.addOption({QStringLiteral("software-encoding"), QStringLiteral("SoftwareEncoding for private codecs: auto, never or prefer."), QStringLiteral("mode"), QStringLiteral("auto")});
    parser.addOption({QStringLiteral("av1-tiles"), QStringLiteral("AV1 tiles for the Farside client: auto, 1, 2, 4, 8 or 16."), QStringLiteral("tiles"), QStringLiteral("auto")});
    parser.process(application);
    if (getuid() || geteuid()) { qCritical("Virtual host requires an explicit root service invocation"); return 1; }
    bool validPort = false;
    const auto port = parser.value(QStringLiteral("port")).toUShort(&validPort);
    bool validQuality = false;
    const int quality = parser.value(QStringLiteral("quality")).toInt(&validQuality);
    const auto adaptiveValue = parser.value(QStringLiteral("adaptive-quality"));
    const auto audioPriorityValue = parser.value(QStringLiteral("prefer-audio-quality"));
    const auto standardMediaValue = parser.value(QStringLiteral("standard-client-media"));
    const auto cameraLoopback = parser.value(QStringLiteral("camera-loopback-device"));
    const QHostAddress address(parser.value(QStringLiteral("address")));
    const auto certificate = parser.value(QStringLiteral("certificate")), key = parser.value(QStringLiteral("certificate-key"));
    const auto readableFile = [](const QString &path) {
        return KRdp::VirtualSessionLaunchPlan::absoluteCleanPath(path) && QFileInfo(path).isFile() && QFileInfo(path).isReadable();
    };
    const auto softwareEncoding = KRdp::parseHostSoftwareEncoding(parser.value(QStringLiteral("software-encoding")));
    if (!parser.positionalArguments().isEmpty() || !validPort || !port || !validQuality || quality < 0 || quality > 100
        || (adaptiveValue != QLatin1String("true") && adaptiveValue != QLatin1String("false")) || address.isNull() || !softwareEncoding
        || (audioPriorityValue != QLatin1String("true") && audioPriorityValue != QLatin1String("false"))
        || (standardMediaValue != QLatin1String("true") && standardMediaValue != QLatin1String("false"))
        || (cameraLoopback != QLatin1String("none") && !cameraLoopback.startsWith(QLatin1String("/dev/")))
        || !KRdp::VirtualSessionLaunchPlan::absoluteCleanPath(certificate) || !KRdp::VirtualSessionLaunchPlan::absoluteCleanPath(key)) {
        qCritical("Virtual host requires valid address, port, software encoding and absolute TLS paths"); return 1;
    }
    // AUD-FIX7: the host keeps its own certificate valid (created when missing, renewed when
    // expired or within 30 days of it, a valid one kept), like krdpserver's (AUD-K3).
    const KRdp::ServerCertificate::Paths certificatePaths{certificate, key};
    if (!KRdp::ensureHostCertificate(certificatePaths, "farside-virtual-host")) return 1;
    if (!readableFile(certificate) || !readableFile(key)) {
        qCritical("Virtual host requires readable absolute TLS files"); return 1;
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
    server.setStandardClientMedia(standardMediaValue == QLatin1String("true"));
    if (cameraLoopback != QLatin1String("none")) server.setCameraLoopbackDevice(cameraLoopback);
    KRdp::VirtualSessionHostController host(&server, {});
    host.setCameraLoopbackDevice(server.cameraLoopbackDevice());
    host.setVideoQualityPolicy(quint8(quality), adaptiveValue == QLatin1String("true"));
    host.setAudioPriorityDefault(audioPriorityValue == QLatin1String("true"));
    host.setUserSettingsReader(KRdp::BrokerUserSettings::readUser);
    // User preferences (including this field) are one validated transaction.
    host.setStockClientPolicy([](quint32) { return KRdp::VirtualStockClient::Policy::AttachOrCreate; });
    // AUD-FIX7: what `capabilities.video` offers and each connection's codec policy starts from;
    // a desktop's worker probes its own encoders and replaces this estimate once it reports.
    KRdp::EncoderSupport::applyProcessOverrides();
    const KRdp::VideoCodecHost videoHost{KRdp::EncoderSupport::probe(), *softwareEncoding,
                                         KRdp::parseHostAv1Tiles(parser.value(QStringLiteral("av1-tiles")), "farside-virtual-host")};
    qInfo().noquote() << "Virtual host video encoders:" << KRdp::EncoderSupport::describe(videoHost.probe) << "- SoftwareEncoding"
                      << KRdp::CodecPolicy::softwareEncodingName(videoHost.mode) << "- AV1 tiles" << KRdp::CodecPolicy::av1TilesName(videoHost.av1Tiles);
    host.setVideoCodecHost(videoHost);
    if (!host.recover(*journal, &error) || !host.enableIndependentCreates(*journal)) {
        qCritical().noquote() << "Virtual host recovery refused:" << error; return 1;
    }
    // Never expose a listener with partially imported or unavailable journal state.
    // Individual failed desktop intents remain listed; they are not replacements.
    if (!server.start()) return 1;
    QTimer certificateRenewal;
    certificateRenewal.setInterval(std::chrono::hours(12));
    QObject::connect(&certificateRenewal, &QTimer::timeout, &application, [certificatePaths] {
        KRdp::ensureHostCertificate(certificatePaths, "farside-virtual-host");
    });
    certificateRenewal.start();
    return application.exec();
}
