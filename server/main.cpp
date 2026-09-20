// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <csignal>
#include <filesystem>

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>

#include <KAboutData>
#include <KCrash>
#include <KSharedConfig>
#include <KConfigWatcher>

#include <qt6keychain/keychain.h>

#include "PhysicalOutputGuard.h"
#include "RdpConnection.h"
#include "Server.h"
#include "SessionController.h"
#include "VideoCodecSupport.h"
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

QString normalizedMonitorMode(QString mode)
{
    mode = mode.trimmed();
    if (mode.isEmpty() || mode.compare(u"workspace"_s, Qt::CaseInsensitive) == 0) {
        return u"workspace"_s;
    }
    if (mode.compare(u"primary"_s, Qt::CaseInsensitive) == 0) {
        return u"primary"_s;
    }
    if (mode.compare(u"specific"_s, Qt::CaseInsensitive) == 0) {
        return u"specific"_s;
    }
    if (mode.compare(u"multi"_s, Qt::CaseInsensitive) == 0) {
        return u"multi"_s;
    }
    if (mode.compare(u"virtual"_s, Qt::CaseInsensitive) == 0) {
        return u"virtual"_s;
    }
    qWarning() << "Unknown MonitorMode value" << mode << "falling back to workspace";
    return u"workspace"_s;
}

KRdp::CodecPreference codecPreferenceFrom(const QString &value)
{
    if (const auto parsed = KRdp::VideoCodecSupport::parseCodecPreference(value)) {
        return *parsed;
    }
    qWarning() << "Unknown Codec value" << value << "(auto|avc420|avc444); using auto";
    return KRdp::CodecPreference::Auto;
}

// OPT-045b (design §10 A10.2): the whole set is refused together on an invalid value, same rule as
// ChromaPolicy::isValid() / KPipeWire's own setChromaPolicy() - never "fix up just the bad field",
// which could silently turn a deliberately unusual but valid set into something else.
KRdp::ChromaPolicy chromaPolicyFrom(const ServerConfig *config)
{
    const KRdp::ChromaPolicy policy{config->avc444MotionGapMs(), config->avc444RestMs(), config->avc444MaxGapMs()};
    if (policy.isValid()) {
        return policy;
    }
    qWarning() << "Invalid Avc444MotionGapMs/Avc444RestMs/Avc444MaxGapMs" << policy.motionGapMs << policy.restMs << policy.maxGapMs
               << "(need each in [16,5000] and motionGap <= rest <= maxGap); using the defaults 100/150/1500";
    return KRdp::ChromaPolicy{};
}

std::optional<int> configuredMonitorIndex(const ServerConfig *config)
{
    const auto mode = normalizedMonitorMode(config->monitorMode());
    // `virtual` captures no physical monitor at all, and the workspace is what
    // it falls back to when it cannot be used (portal session).
    if (mode == u"workspace"_s || mode == u"virtual"_s) {
        return std::nullopt;
    }

    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return std::nullopt;
    }

    if (mode == u"primary"_s || mode == u"multi"_s) {
        // `multi` streams every monitor on its own surface; this is only the
        // target it falls back to when that turns out not to be usable.
        return SessionController::primaryScreenIndex();
    }

    const auto index = config->monitorIndex();
    if (index < 0) {
        qWarning() << "Configured monitor index is negative, falling back to workspace mode";
        return std::nullopt;
    }
    return index;
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
        {u"restore-outputs"_s, u"Re-enable the physical outputs a crashed virtual-monitor session left disabled, then exit."_s},
        {u"quality"_s, u"Encoding quality of the stream, from 0 (lowest) to 100 (highest)"_s, u"quality"_s},
#ifdef WITH_PLASMA_SESSION
        {u"plasma"_s, u"Use Plasma protocols instead of XDP"_s},
#endif
    });
    about.setupCommandLine(&parser);
    parser.process(application);
    about.processCommandLine(&parser);

    if (parser.isSet(u"restore-outputs"_s)) {
        return PhysicalOutputGuard::restoreFromStateFile() ? 0 : 1;
    }

    signal(SIGINT, [](int) {
        QCoreApplication::exit(0);
    });

    signal(SIGTERM, [](int) {
        QCoreApplication::exit(0);
    });

    auto config = ServerConfig::self();
    const auto vaapiDriverMode = normalizedVaapiDriverMode(config->vaapiDriverMode());
    applyVaapiDriverMode(vaapiDriverMode);
    // Resolve LIBVA_DRIVER_NAME once at startup (was previously done lazily on
    // the first connection behind a once-per-process guard).
    KRdp::selectVaapiDriver();

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

    // Log Qt screen list for diagnostics (order may differ from Plasma display settings).
    const auto screens = QGuiApplication::screens();
    const auto primaryScreen = QGuiApplication::primaryScreen();
    for (int i = 0; i < screens.size(); ++i) {
        const auto *s = screens.at(i);
        qInfo() << "Qt screen" << i << s->name() << s->geometry()
                << (s == primaryScreen ? "(Qt primary)" : "");
    }
    qInfo() << "KWin primary output:" << SessionController::kwinPrimaryOutputName();

    SessionController controller(&server, parser.isSet(u"plasma"_s) ? SessionController::SessionType::Plasma : SessionController::SessionType::Portal);
    // A crash with MonitorMode=virtual/replace leaves the state file behind;
    // a clean session deletes it. Nothing to do in the common case.
    PhysicalOutputGuard::restoreFromStateFile();
    QString streamTarget = u"workspace-default"_s;
    const bool monitorPinnedByCli = parser.isSet(u"monitor"_s) || parser.isSet(u"virtual-monitor"_s);
    const bool qualityPinnedByCli = parser.isSet(u"quality"_s);
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
        std::optional<int> monitorIndex;
        // --monitor pins a single output, so it wins over MonitorMode=multi/virtual.
        const bool virtualRequested = !parser.isSet(u"monitor"_s) && normalizedMonitorMode(config->monitorMode()) == u"virtual"_s;
        const bool multiRequested = !parser.isSet(u"monitor"_s) && !virtualRequested && normalizedMonitorMode(config->monitorMode()) == u"multi"_s;
        if (parser.isSet(u"monitor"_s)) {
            monitorIndex = parser.value(u"monitor"_s).toInt();
        } else {
            monitorIndex = configuredMonitorIndex(config);
        }

        if (virtualRequested && !parser.isSet(u"plasma"_s)) {
            qWarning() << "MonitorMode=virtual needs --plasma (the portal session cannot create outputs); using workspace";
        }
        controller.setVirtualPolicy(SessionController::parseVirtualPolicy(config->virtualMonitorPolicy()));
        controller.setVirtualLayout(SessionController::parseVirtualLayout(config->virtualMonitorLayout()));
        controller.setVirtualFallbackSize(SessionController::parseSize(config->virtualMonitorFallbackSize()).value_or(QSize(1920, 1080)));
        controller.setVirtualMode(virtualRequested && parser.isSet(u"plasma"_s));

        // The index goes first so that a multi -> workspace/specific switch
        // rebuilds the single session on the target it is about to use,
        // instead of building it on the old one and re-creating the capture
        // stream a moment later.
        controller.setMonitorIndex(monitorIndex);
        controller.setMultiMonitorEnabled(multiRequested);
        if (controller.virtualMode()) {
            streamTarget = u"virtual:%1 (%2)"_s.arg(SessionController::layoutName(controller.virtualLayout()), SessionController::policyName(controller.virtualPolicy()));
        } else if (controller.multiMonitorEnabled()) {
            streamTarget = u"multi:%1"_s.arg(controller.multiMonitorCount());
        } else if (multiRequested) {
            // With no resolvable primary there is no index to fall back to, so
            // the stream is the whole workspace.
            streamTarget = monitorIndex.has_value() ? u"specific:%1 (multi fallback)"_s.arg(*monitorIndex) : u"workspace (multi fallback)"_s;
        } else if (monitorIndex.has_value()) {
            streamTarget = u"monitor:%1"_s.arg(*monitorIndex);
        }
    }
    const auto quality = parserValueWithDefault(u"quality", config->quality());
    controller.setQuality(quality);
    controller.setAdaptiveQuality(config->adaptiveQuality());
    controller.setCodecPreference(codecPreferenceFrom(config->codec()));
    controller.setChromaPolicyDefaults(chromaPolicyFrom(config));
    controller.setWakeDisplayOnConnect(config->wakeDisplayOnConnect());

    auto runtimeConfig = KSharedConfig::openConfig(QStringLiteral("krdpserverrc"));
    // Also logged on every reload: KConfig's --notify broadcasts by file NAME,
    // so a second instance started with its own XDG_CONFIG_HOME makes every
    // other krdpserver reload too. Saying which file this process actually
    // read is what tells a reload of one's own config from that cross-talk.
    const QString runtimeConfigPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/krdpserverrc");
    // Applies persisted-config changes (quality, monitor target, wake, VAAPI).
    // It must NOT force a display refresh: a quality-slider write should never
    // touch the stream. setMonitorIndex() self-guards and only re-creates the
    // stream when the resolved target actually changed.
    const bool plasmaSession = parser.isSet(u"plasma"_s);
    auto applyRuntimeConfig = [config, &controller, monitorPinnedByCli, qualityPinnedByCli, plasmaSession, listenPort = server.port(), runtimeConfigPath]() {
        // KConfigSkeleton::read() only re-applies the in-memory KConfig cache
        // to the skeleton's items; it does NOT reload the file from disk (see
        // KCoreConfigSkeleton::read() vs ::load() docs). That happened to work
        // when KConfigWatcher had just reparsed the same KSharedConfig object
        // for us, but left the QFileSystemWatcher-triggered path below (a
        // plain file edit, or a write without --notify) reading stale values
        // forever. load() always reads from disk, so it covers both triggers.
        config->load();

        if (!qualityPinnedByCli) {
            controller.setQuality(config->quality());
        }

        if (!monitorPinnedByCli) {
            // Both self-guard on an unchanged value, so a quality-slider write
            // does not rebuild anything. Same order as at startup: the index
            // has to be current before multi mode is switched off, or the
            // single session it rebuilds targets the previous monitor.
            controller.setMonitorIndex(configuredMonitorIndex(config));
            const bool virtualRequested = normalizedMonitorMode(config->monitorMode()) == u"virtual"_s && plasmaSession;
            controller.setVirtualPolicy(SessionController::parseVirtualPolicy(config->virtualMonitorPolicy()));
            controller.setVirtualLayout(SessionController::parseVirtualLayout(config->virtualMonitorLayout()));
            controller.setVirtualFallbackSize(SessionController::parseSize(config->virtualMonitorFallbackSize()).value_or(QSize(1920, 1080)));
            controller.setVirtualMode(virtualRequested);
            controller.setMultiMonitorEnabled(!virtualRequested && normalizedMonitorMode(config->monitorMode()) == u"multi"_s);
        }

        controller.setAdaptiveQuality(config->adaptiveQuality());
        controller.setCodecPreference(codecPreferenceFrom(config->codec()));
        const auto chromaPolicy = chromaPolicyFrom(config);
        controller.setChromaPolicyDefaults(chromaPolicy);
        controller.setWakeDisplayOnConnect(config->wakeDisplayOnConnect());
        applyVaapiDriverMode(config->vaapiDriverMode());
        KRdp::selectVaapiDriver();

        // The KRDP logging category (used inside libKRdp) is not exported for
        // consumers, so this uses the same plain, always-on qInfo() the rest
        // of this file's "Applied runtime ..." lines use.
        qInfo() << "Runtime config applied: quality" << config->quality() << "adaptive" << config->adaptiveQuality() << "monitorMode" << config->monitorMode()
                << "monitorIndex" << config->monitorIndex() << "virtualPolicy" << config->virtualMonitorPolicy() << "virtualLayout" << config->virtualMonitorLayout()
                << "wakeDisplay" << config->wakeDisplayOnConnect() << "vaapiMode" << config->vaapiDriverMode() << "port" << listenPort << "from" << runtimeConfigPath
                << "codec" << config->codec() << "chroma" << QStringLiteral("%1/%2/%3").arg(chromaPolicy.motionGapMs).arg(chromaPolicy.restMs).arg(chromaPolicy.maxGapMs);
    };

    // Re-creates the capture stream for a new display topology (resolution or
    // output-set change). Kept separate from applyRuntimeConfig so config
    // reloads and screen events do not share a code path.
    auto refreshDisplayTopology = [config, &controller, monitorPinnedByCli]() {
        if (!monitorPinnedByCli) {
            // In primary/specific mode the resolved index can shift when outputs
            // re-enumerate; re-resolve before refreshing.
            controller.setMonitorIndex(configuredMonitorIndex(config));
        }
        controller.refreshDisplayConfiguration();
    };

    QTimer configDebounceTimer(&application);
    configDebounceTimer.setSingleShot(true);
    configDebounceTimer.setInterval(100);
    QObject::connect(&configDebounceTimer, &QTimer::timeout, &application, [applyRuntimeConfig]() {
        applyRuntimeConfig();
    });

    auto configWatcher = KConfigWatcher::create(runtimeConfig);
    QObject::connect(configWatcher.get(), &KConfigWatcher::configChanged, &application, [&configDebounceTimer](const KConfigGroup &group, const QByteArrayList &) {
        if (group.name() != QLatin1StringView("General")) {
            return;
        }
        configDebounceTimer.start();
    });

    auto configFileWatcher = std::make_unique<QFileSystemWatcher>(&application);
    if (!runtimeConfigPath.isEmpty()) {
        configFileWatcher->addPath(runtimeConfigPath);
    }
    QObject::connect(configFileWatcher.get(), &QFileSystemWatcher::fileChanged, &application, [&configDebounceTimer, configFileWatcher = configFileWatcher.get()](const QString &path) {
        if (QFileInfo::exists(path) && !configFileWatcher->files().contains(path)) {
            configFileWatcher->addPath(path);
        }
        configDebounceTimer.start();
    });

    QTimer displayRefreshDebounceTimer(&application);
    displayRefreshDebounceTimer.setSingleShot(true);
    displayRefreshDebounceTimer.setInterval(100);
    QObject::connect(&displayRefreshDebounceTimer, &QTimer::timeout, &application, [refreshDisplayTopology]() {
        refreshDisplayTopology();
    });

    auto scheduleDisplayRefresh = [&displayRefreshDebounceTimer]() {
        displayRefreshDebounceTimer.start();
    };

    auto connectScreenSignals = [&application, scheduleDisplayRefresh](QScreen *screen) {
        if (!screen || screen->property("_krdpDisplaySignalsConnected").toBool()) {
            return;
        }
        screen->setProperty("_krdpDisplaySignalsConnected", true);

        QObject::connect(screen, &QScreen::geometryChanged, &application, [scheduleDisplayRefresh](const QRect &) {
            scheduleDisplayRefresh();
        });
        QObject::connect(screen, &QScreen::virtualGeometryChanged, &application, [scheduleDisplayRefresh](const QRect &) {
            scheduleDisplayRefresh();
        });
        QObject::connect(screen, &QScreen::availableGeometryChanged, &application, [scheduleDisplayRefresh](const QRect &) {
            scheduleDisplayRefresh();
        });
    };

    for (auto *screen : QGuiApplication::screens()) {
        connectScreenSignals(screen);
    }

    QObject::connect(&application, &QGuiApplication::screenAdded, &application, [connectScreenSignals, scheduleDisplayRefresh](QScreen *screen) {
        connectScreenSignals(screen);
        scheduleDisplayRefresh();
    });
    QObject::connect(&application, &QGuiApplication::screenRemoved, &application, [scheduleDisplayRefresh](QScreen *) {
        scheduleDisplayRefresh();
    });
    QObject::connect(&application, &QGuiApplication::primaryScreenChanged, &application, [scheduleDisplayRefresh](QScreen *) {
        scheduleDisplayRefresh();
    });

#ifdef WITH_PLASMA_SESSION
    const auto sessionType = parser.isSet(u"plasma"_s) ? u"plasma"_s : u"portal"_s;
#else
    const auto sessionType = u"portal"_s;
#endif
    const auto startupChromaPolicy = controller.chromaPolicyDefaults();
    const auto startupChromaText = QStringLiteral("%1/%2/%3").arg(startupChromaPolicy.motionGapMs).arg(startupChromaPolicy.restMs).arg(startupChromaPolicy.maxGapMs);
    qInfo().noquote() << QStringLiteral("KRDP startup summary: session=%1 stream=%2 port=%3 quality=%4 vaapiMode=%5 KRDP_FORCE_VAAPI_DRIVER=%6 KRDP_AUTO_VAAPI_DRIVER=%7 wakeDisplay=%8 adaptive=%9 codec=%10 chroma=%11")
                             .arg(sessionType,
                                  streamTarget,
                                  QString::number(port),
                                  QString::number(quality),
                                  vaapiDriverMode,
                                  envValueOrUnset("KRDP_FORCE_VAAPI_DRIVER"),
                                  envValueOrUnset("KRDP_AUTO_VAAPI_DRIVER"),
                                  config->wakeDisplayOnConnect() ? u"1"_s : u"0"_s,
                                  config->adaptiveQuality() ? u"1"_s : u"0"_s,
                                  QLatin1String(KRdp::VideoCodecSupport::preferenceName(controller.codecPreference())),
                                  startupChromaText);

    if (!server.start()) {
        return -1;
    }

    return application.exec();
}
