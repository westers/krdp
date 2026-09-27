// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

// Pure rules shared by krdpserver (server/main.cpp) and the KCM, so both
// sides agree on what a setting means (WS-K of the 2026-09-27 audit plan).

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTimeZone>

#include <optional>

namespace KRdp::ServerSettings
{
// --- AUD-K4: listening port -------------------------------------------------

constexpr int kMinPort = 1;
constexpr int kMaxPort = 65535;

inline bool isValidListenPort(qint64 port)
{
    return port >= kMinPort && port <= kMaxPort;
}

// Strict: decimal digits only (no sign, no spaces), 1..65535.
inline std::optional<quint16> parseListenPort(const QString &text)
{
    if (text.isEmpty() || text.size() > 5) {
        return std::nullopt;
    }
    for (const QChar c : text) {
        if (c < u'0' || c > u'9') {
            return std::nullopt;
        }
    }
    const int value = text.toInt();
    if (!isValidListenPort(value)) {
        return std::nullopt;
    }
    return quint16(value);
}

// --- AUD-K5: capture backend -------------------------------------------------

enum class Backend {
    Portal,
    Plasma,
};

inline QString backendName(Backend backend)
{
    return backend == Backend::Plasma ? QStringLiteral("plasma") : QStringLiteral("portal");
}

// `multi` (one surface per monitor) and `virtual` (compositor-created outputs)
// only exist on the Plasma screencast backend; the portal session cannot do
// either.
inline bool monitorModeNeedsPlasma(const QString &mode)
{
    const auto m = mode.trimmed();
    return m.compare(QLatin1String("multi"), Qt::CaseInsensitive) == 0 || m.compare(QLatin1String("virtual"), Qt::CaseInsensitive) == 0;
}

struct BackendChoice {
    Backend backend = Backend::Portal;
    // Non-empty when the server picked Plasma on its own (for the log).
    QString automaticReason;
};

// --plasma always wins. Otherwise the server picks Plasma by itself when the
// configured MonitorMode needs it and Plasma support is built in, so the stock
// unit (which passes no flag) serves `multi`/`virtual` without a drop-in.
inline BackendChoice chooseBackend(bool plasmaFlag, const QString &monitorMode, bool plasmaBuilt)
{
    if (plasmaFlag && plasmaBuilt) {
        return {Backend::Plasma, {}};
    }
    if (plasmaBuilt && monitorModeNeedsPlasma(monitorMode)) {
        return {Backend::Plasma, QStringLiteral("MonitorMode=%1 needs the Plasma backend").arg(monitorMode.trimmed())};
    }
    return {Backend::Portal, {}};
}

// --- AUD-K6: restart detection -------------------------------------------------

// The settings krdpserver reads only at startup. Everything else in
// krdpserverrc is applied live by the running server.
struct StartupSettings {
    int listenPort = 3389;
    QString listenAddress;
    bool autogenerateCertificates = true;
    QString certificate;
    QString certificateKey;
    QStringList users;
    bool systemUserEnabled = false;

    bool operator==(const StartupSettings &) const = default;
};

// What a running server records about the configuration it started with.
struct LoadedState {
    qint64 pid = 0;
    QDateTime loadedAt;
    Backend backend = Backend::Portal;
    StartupSettings settings;
};

// One state file per krdpserverrc path, so test instances with their own
// XDG_CONFIG_HOME never collide with the live service.
inline QString configFileKey(const QString &configFilePath)
{
    return QString::fromLatin1(QCryptographicHash::hash(configFilePath.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

inline QString runtimeDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/krdpserver");
}

inline QString loadedStatePath(const QString &configFilePath)
{
    return runtimeDirectory() + QStringLiteral("/loaded-%1.json").arg(configFileKey(configFilePath));
}

// Written by the KCM after a password was stored: passwords are read from the
// keychain at startup only, so a newer stamp than LoadedState::loadedAt means
// the running server still has the old one.
inline QString credentialsStampPath(const QString &configFilePath)
{
    return runtimeDirectory() + QStringLiteral("/credentials-%1.stamp").arg(configFileKey(configFilePath));
}

inline QByteArray serializeLoadedState(const LoadedState &state)
{
    const auto &s = state.settings;
    const QJsonObject settings{
        {QStringLiteral("ListenPort"), s.listenPort},
        {QStringLiteral("ListenAddress"), s.listenAddress},
        {QStringLiteral("AutogenerateCertificates"), s.autogenerateCertificates},
        {QStringLiteral("Certificate"), s.certificate},
        {QStringLiteral("CertificateKey"), s.certificateKey},
        {QStringLiteral("Users"), QJsonArray::fromStringList(s.users)},
        {QStringLiteral("SystemUserEnabled"), s.systemUserEnabled},
    };
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("v"), 1},
                             {QStringLiteral("pid"), state.pid},
                             {QStringLiteral("loadedAt"), state.loadedAt.toMSecsSinceEpoch()},
                             {QStringLiteral("backend"), backendName(state.backend)},
                             {QStringLiteral("settings"), settings},
                         })
        .toJson(QJsonDocument::Compact);
}

inline std::optional<LoadedState> parseLoadedState(const QByteArray &data)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const auto root = document.object();
    const auto settings = root.value(QStringLiteral("settings")).toObject();
    if (root.value(QStringLiteral("v")).toInt() != 1 || settings.isEmpty()) {
        return std::nullopt;
    }
    LoadedState state;
    state.pid = root.value(QStringLiteral("pid")).toInteger();
    state.loadedAt = QDateTime::fromMSecsSinceEpoch(root.value(QStringLiteral("loadedAt")).toInteger(), QTimeZone::UTC);
    state.backend = root.value(QStringLiteral("backend")).toString() == QLatin1String("plasma") ? Backend::Plasma : Backend::Portal;
    auto &s = state.settings;
    s.listenPort = settings.value(QStringLiteral("ListenPort")).toInt();
    s.listenAddress = settings.value(QStringLiteral("ListenAddress")).toString();
    s.autogenerateCertificates = settings.value(QStringLiteral("AutogenerateCertificates")).toBool();
    s.certificate = settings.value(QStringLiteral("Certificate")).toString();
    s.certificateKey = settings.value(QStringLiteral("CertificateKey")).toString();
    const auto users = settings.value(QStringLiteral("Users")).toArray();
    for (const auto &user : users) {
        s.users.append(user.toString());
    }
    s.systemUserEnabled = settings.value(QStringLiteral("SystemUserEnabled")).toBool();
    return state;
}

// Names of the krdpserverrc keys (plus the two pseudo-keys "Passwords" and
// "Backend") whose saved value differs from what the running server loaded.
// Empty means no restart is needed. A server that has not recorded its state
// (not running, or an older binary) yields nothing.
inline QStringList restartReasons(const std::optional<LoadedState> &loaded,
                                  const StartupSettings &saved,
                                  const QString &savedMonitorMode,
                                  const std::optional<QDateTime> &credentialsChangedAt)
{
    QStringList reasons;
    if (!loaded) {
        return reasons;
    }
    const auto &l = loaded->settings;
    if (l.listenPort != saved.listenPort) {
        reasons << QStringLiteral("ListenPort");
    }
    if (l.listenAddress != saved.listenAddress) {
        reasons << QStringLiteral("ListenAddress");
    }
    if (l.autogenerateCertificates != saved.autogenerateCertificates) {
        reasons << QStringLiteral("AutogenerateCertificates");
    }
    // With autogeneration on the server uses its own managed files and
    // ignores these two keys.
    if (!saved.autogenerateCertificates && (l.certificate != saved.certificate || l.certificateKey != saved.certificateKey)) {
        reasons << QStringLiteral("Certificate");
    }
    if (l.users != saved.users) {
        reasons << QStringLiteral("Users");
    }
    if (l.systemUserEnabled != saved.systemUserEnabled) {
        reasons << QStringLiteral("SystemUserEnabled");
    }
    if (credentialsChangedAt && credentialsChangedAt->isValid() && *credentialsChangedAt > loaded->loadedAt) {
        reasons << QStringLiteral("Passwords");
    }
    if (monitorModeNeedsPlasma(savedMonitorMode) && loaded->backend != Backend::Plasma) {
        reasons << QStringLiteral("Backend");
    }
    return reasons;
}
}
