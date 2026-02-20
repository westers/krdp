// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <csignal>
#include <filesystem>

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QRegularExpression>

#include <KAboutData>
#include <KCrash>
#include <KSharedConfig>

#include <qt6keychain/keychain.h>

#include "Server.h"
#include "SessionController.h"
#include "krdp_version.h"
#include "krdpserversettings.h"

using namespace Qt::StringLiterals;

namespace
{
QString normalizedVaapiDriverMode(QString mode)
{
    mode = mode.trimmed();
    if (mode.isEmpty() || mode.compare(u"auto"_s, Qt::CaseInsensitive) == 0) {
        return u"auto"_s;
    }
    if (mode.compare(u"off"_s, Qt::CaseInsensitive) == 0 || mode.compare(u"disabled"_s, Qt::CaseInsensitive) == 0) {
        return u"off"_s;
    }
    if (mode.compare(u"radeonsi"_s, Qt::CaseInsensitive) == 0) {
        return u"radeonsi"_s;
    }
    if (mode.compare(u"ihd"_s, Qt::CaseInsensitive) == 0) {
        return u"iHD"_s;
    }
    if (mode.compare(u"i965"_s, Qt::CaseInsensitive) == 0) {
        return u"i965"_s;
    }

    qWarning() << "Unknown VaapiDriverMode value" << mode << "falling back to auto";
    return u"auto"_s;
}

void applyVaapiDriverMode(const QString &mode)
{
    const auto normalizedMode = normalizedVaapiDriverMode(mode);
    if (normalizedMode == u"auto"_s) {
        qunsetenv("KRDP_FORCE_VAAPI_DRIVER");
        qunsetenv("KRDP_AUTO_VAAPI_DRIVER");
        return;
    }
    if (normalizedMode == u"off"_s) {
        qunsetenv("KRDP_FORCE_VAAPI_DRIVER");
        qputenv("KRDP_AUTO_VAAPI_DRIVER", "0");
        return;
    }

    qunsetenv("KRDP_AUTO_VAAPI_DRIVER");
    qputenv("KRDP_FORCE_VAAPI_DRIVER", normalizedMode.toLatin1());
}

QString envValueOrUnset(const char *name)
{
    const auto value = qgetenv(name);
    return value.isEmpty() ? u"unset"_s : QString::fromLatin1(value);
}
}

int main(int argc, char **argv)
{
    QApplication application{argc, argv};
    application.setApplicationName(u"krdp-server"_s);
    application.setApplicationDisplayName(u"KRDP Server"_s);
    // Ensure Wayland privilege checks resolve to the installed desktop file.
    application.setDesktopFileName(u"org.kde.krdpserver"_s);

    KAboutData about(u"krdp-server"_s, u"KRDP Server"_s, QStringLiteral(KRdp_VERSION_STRING));
    KAboutData::setApplicationData(about);

    KCrash::initialize();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"An RDP server that exposes the current desktop session over the RDP protocol.\nNote that a valid TLS certificate and key is needed. If not provided, a temporary certificate will be generated."_s);
    parser.addOptions({
        {{u"u"_s, u"username"_s}, u"The username to use for login"_s, u"username"_s},
        {{u"p"_s, u"password"_s}, u"The password to use for login. Requires username to be passed as well."_s, u"password"_s},
        {u"address"_s, u"The address to listen on for connections. Defaults to 0.0.0.0"_s, u"address"_s},
        {u"port"_s, u"The port to use for connections. Defaults to 3389."_s, u"port"_s, u"3389"_s},
        {u"certificate"_s, u"The TLS certificate file to use."_s, u"certificate"_s, u"server.crt"_s},
        {u"certificate-key"_s, u"The TLS certificate key to use."_s, u"certificate-key"_s, u"server.key"_s},
        {u"monitor"_s, u"The index of the monitor to use when streaming."_s, u"monitor"_s, u"-1"_s},
        {u"virtual-monitor"_s,
         u"Creates a new virtual output to connect to (WIDTHxHEIGHT@SCALE, e.g. 1920x1080@1). Incompatible with --monitor."_s,
         u"data"_s,
         u"1920x1080@1"_s},
        {u"quality"_s, u"Encoding quality of the stream, from 0 (lowest) to 100 (highest)"_s, u"quality"_s},
#ifdef WITH_PLASMA_SESSION
        {u"plasma"_s, u"Use Plasma protocols instead of XDP"_s},
#endif
    });
    about.setupCommandLine(&parser);
    parser.process(application);
    about.processCommandLine(&parser);

    signal(SIGINT, [](int) {
        QCoreApplication::exit(0);
    });

    signal(SIGTERM, [](int) {
        QCoreApplication::exit(0);
    });

    auto config = ServerConfig::self();
    const auto vaapiDriverMode = normalizedVaapiDriverMode(config->vaapiDriverMode());
    applyVaapiDriverMode(vaapiDriverMode);

    auto parserValueWithDefault = [&parser](QAnyStringView option, auto defaultValue) {
        auto optionString = option.toString();
        if (parser.isSet(optionString)) {
            return QVariant(parser.value(optionString)).value<decltype(defaultValue)>();
        } else {
            return defaultValue;
        }
    };

    QHostAddress address = QHostAddress::Any;
    if (parser.isSet(u"address"_s)) {
        address = QHostAddress(parser.value(u"address"_s));
    }
    auto port = parserValueWithDefault(u"port", config->listenPort());
    auto certificate = std::filesystem::path(parserValueWithDefault(u"certificate", config->certificate()).toStdString());
    auto certificateKey = std::filesystem::path(parserValueWithDefault(u"certificate-key", config->certificateKey()).toStdString());

    KRdp::Server server(nullptr);

    server.setAddress(address);
    server.setPort(port);

    server.setTlsCertificate(certificate);
    server.setTlsCertificateKey(certificateKey);

    // Use parsed username/pw if set
    if (parser.isSet(u"username"_s)) {
        KRdp::User user;
        user.name = parser.value(u"username"_s);
        user.password = parser.value(u"password"_s);
        server.addUser(user);
    }
    // Otherwise use KCM username list
    else {
        server.setUsePAMAuthentication(config->systemUserEnabled());

        const auto users = config->users();
        for (const auto &userName : users) {
            const auto readJob = new QKeychain::ReadPasswordJob(QLatin1StringView("KRDP"));
            readJob->setKey(QLatin1StringView(userName.toLatin1()));
            QObject::connect(readJob, &QKeychain::ReadPasswordJob::finished, &server, [userName, readJob, &server]() {
                KRdp::User user;
                if (readJob->error() != QKeychain::Error::NoError) {
                    qWarning() << "requestPassword: Failed to read password of " << userName << " because of error: " << readJob->error();
                    return;
                }
                user.name = userName;
                user.password = readJob->textData();
                server.addUser(user);
            });
            readJob->start();
        }
        if (users.isEmpty() && !server.usePAMAuthentication()) {
            qWarning() << "No users configured for login. Either pass a username/password or configure users using kcm_krdp.";
            return -1;
        }
    }

    SessionController controller(&server, parser.isSet(u"plasma"_s) ? SessionController::SessionType::Plasma : SessionController::SessionType::Portal);
    QString streamTarget = u"workspace-default"_s;
    if (parser.isSet(u"virtual-monitor"_s)) {
        const QString vmData = parser.value(u"virtual-monitor"_s);
        const QRegularExpression rx(uR"((\d+)x(\d+)@([\d.]+))"_s);
        const auto match = rx.match(vmData);
        if (!match.hasMatch()) {
            qWarning() << "failed to parse" << vmData << ".  Should be WIDTHxHEIGHT@SCALE";
            return 1;
        }
        controller.setVirtualMonitor({vmData, {match.capturedView(1).toInt(), match.capturedView(2).toInt()}, match.capturedView(3).toDouble()});
        streamTarget = u"virtual:%1"_s.arg(vmData);
    } else {
        controller.setMonitorIndex(parser.isSet(u"monitor"_s) ? std::optional(parser.value(u"monitor"_s).toInt()) : std::nullopt);
        if (parser.isSet(u"monitor"_s)) {
            streamTarget = u"monitor:%1"_s.arg(parser.value(u"monitor"_s));
        }
    }
    const auto quality = parserValueWithDefault(u"quality", config->quality());
    controller.setQuality(quality);

    const bool experimentalAvc444 = qEnvironmentVariableIntValue("KRDP_EXPERIMENTAL_AVC444") > 0;
    const bool experimentalAvc444v2 = qEnvironmentVariableIntValue("KRDP_EXPERIMENTAL_AVC444V2") > 0;
#ifdef WITH_PLASMA_SESSION
    const auto sessionType = parser.isSet(u"plasma"_s) ? u"plasma"_s : u"portal"_s;
#else
    const auto sessionType = u"portal"_s;
#endif
    qInfo().noquote() << QStringLiteral("KRDP startup summary: session=%1 stream=%2 port=%3 quality=%4 vaapiMode=%5 KRDP_FORCE_VAAPI_DRIVER=%6 KRDP_AUTO_VAAPI_DRIVER=%7 expAvc444=%8 expAvc444v2=%9")
                             .arg(sessionType,
                                  streamTarget,
                                  QString::number(port),
                                  QString::number(quality),
                                  vaapiDriverMode,
                                  envValueOrUnset("KRDP_FORCE_VAAPI_DRIVER"),
                                  envValueOrUnset("KRDP_AUTO_VAAPI_DRIVER"),
                                  experimentalAvc444 ? u"1"_s : u"0"_s,
                                  experimentalAvc444v2 ? u"1"_s : u"0"_s);

    if (!server.start()) {
        return -1;
    }

    return application.exec();
}
