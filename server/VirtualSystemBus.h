// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace KRdp::VirtualSystemBus
{
/** OPT-075: system-bus access for Virtual desktops. Pure policy and rule construction, no process or file side effects
 * except the explicit drop-in reader. The root service entry resolves a Plan, the launcher starts xdg-dbus-proxy
 * outside the sandbox as the session user and binds only the proxy socket. See
 * docs/superpowers/specs/2026-10-16-virtual-system-bus-design.md. `full` is never a default anywhere.
 */
enum class Mode { None, Filtered, Full };

struct Key {
    const char *key; // virtual-session.conf scope key
    const char *suffix; // FARSIDE_VIRTUAL_<suffix>
    const char *fallback; // shipped default
};
inline const QList<Key> &keyTable()
{
    static const QList<Key> table{{"VirtualSystemBus", "SYSTEM_BUS", "filtered"}, {"SystemBusNetwork", "SYSTEM_BUS_NETWORK", "true"},
                                  {"SystemBusPower", "SYSTEM_BUS_POWER", "true"}, {"SystemBusPackages", "SYSTEM_BUS_PACKAGES", "false"},
                                  {"SystemBusPolkit", "SYSTEM_BUS_POLKIT", "false"}, {"SystemBusBluetooth", "SYSTEM_BUS_BLUETOOTH", "false"},
                                  {"SystemBusPrinting", "SYSTEM_BUS_PRINTING", "false"}, {"SystemBusExtra", "SYSTEM_BUS_EXTRA", ""}};
    return table;
}
inline QString environmentName(const QString &key)
{
    for (const auto &entry : keyTable())
        if (key == QLatin1String(entry.key)) return QStringLiteral("FARSIDE_VIRTUAL_") + QLatin1String(entry.suffix);
    return {};
}

constexpr int MaximumExtraNames = 16;
constexpr int MaximumNameBytes = 255;
constexpr int MaximumRules = 200;
inline const QString SystemSocket = QStringLiteral("/run/dbus/system_bus_socket");
inline const QString ProxyProgram = QStringLiteral("/usr/bin/xdg-dbus-proxy");

inline QString modeName(Mode mode) { return mode == Mode::None ? QStringLiteral("none") : mode == Mode::Filtered ? QStringLiteral("filtered") : QStringLiteral("full"); }
inline std::optional<Mode> parseMode(const QString &text)
{
    if (text == QLatin1String("none")) return Mode::None;
    if (text == QLatin1String("filtered")) return Mode::Filtered;
    if (text == QLatin1String("full")) return Mode::Full;
    return {};
}

/** Names the free-form allow-list may never contain, or cover with a wildcard: the bus itself and the service managers. */
inline bool deniedName(const QString &name)
{
    static const QStringList denied{QStringLiteral("org.freedesktop.DBus"), QStringLiteral("org.freedesktop.systemd1"), QStringLiteral("org.freedesktop.machine1")};
    const bool wildcard = name.endsWith(QLatin1String(".*"));
    const QString base = wildcard ? name.chopped(2) : name;
    for (const auto &item : denied) {
        if (base == item || base.startsWith(item + QLatin1Char('.'))) return true;
        if (wildcard && item.startsWith(base + QLatin1Char('.'))) return true; // a wildcard that would cover a denied name
    }
    return false;
}
inline bool validBusName(const QString &name)
{
    static const QRegularExpression grammar(QStringLiteral("\\A[A-Za-z_][A-Za-z0-9_-]*(\\.[A-Za-z_][A-Za-z0-9_-]*)+(\\.\\*)?\\z"));
    return name.toUtf8().size() <= MaximumNameBytes && grammar.match(name).hasMatch() && !deniedName(name);
}
/** Canonical comma-joined form of the extra allow-list, or nothing when any name is invalid (the whole value is refused). */
inline std::optional<QString> normalizeExtra(const QString &value)
{
    for (const auto c : value)
        if (c.unicode() < 32 || c.unicode() == 127) return {};
    if (value.trimmed().isEmpty()) return QString();
    QStringList names;
    for (const auto &part : value.split(QLatin1Char(','))) {
        const auto name = part.trimmed();
        if (!validBusName(name)) return {};
        if (!names.contains(name)) names.append(name);
    }
    if (names.size() > MaximumExtraNames) return {};
    return names.join(QLatin1Char(','));
}
/** One key of the virtual-session scope: canonical text or nothing. Booleans accept only exact true/false. */
inline std::optional<QString> normalizeValue(const QString &key, const QString &value)
{
    if (key == QLatin1String("VirtualSystemBus")) return parseMode(value) ? std::optional<QString>(value) : std::nullopt;
    if (key == QLatin1String("SystemBusExtra")) return normalizeExtra(value);
    if (environmentName(key).isEmpty()) return {};
    if (value == QLatin1String("true") || value == QLatin1String("false")) return value;
    return {};
}
inline bool isBooleanKey(const QString &key) { return !environmentName(key).isEmpty() && key != QLatin1String("VirtualSystemBus") && key != QLatin1String("SystemBusExtra"); }

struct Settings {
    Mode mode = Mode::Filtered;
    bool network = true;
    bool power = true;
    bool packages = false;
    bool polkit = false;
    bool bluetooth = false;
    bool printing = false;
    QStringList extra;
    bool operator==(const Settings &) const = default;
};
/** Applies assignments (settings key -> canonical value) over a base. A key that does not validate refuses all of them. */
inline std::optional<Settings> apply(Settings base, const QMap<QString, QString> &values, QString *error = nullptr)
{
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const auto value = normalizeValue(it.key(), it.value());
        if (!value) {
            if (error) *error = QStringLiteral("invalid value for %1").arg(it.key());
            return {};
        }
        const bool on = *value == QLatin1String("true");
        if (it.key() == QLatin1String("VirtualSystemBus")) base.mode = *parseMode(*value);
        else if (it.key() == QLatin1String("SystemBusNetwork")) base.network = on;
        else if (it.key() == QLatin1String("SystemBusPower")) base.power = on;
        else if (it.key() == QLatin1String("SystemBusPackages")) base.packages = on;
        else if (it.key() == QLatin1String("SystemBusPolkit")) base.polkit = on;
        else if (it.key() == QLatin1String("SystemBusBluetooth")) base.bluetooth = on;
        else if (it.key() == QLatin1String("SystemBusPrinting")) base.printing = on;
        else if (it.key() == QLatin1String("SystemBusExtra")) base.extra = value->isEmpty() ? QStringList() : value->split(QLatin1Char(','));
    }
    if (error) error->clear();
    return base;
}
/** Shipped defaults (what the unit file sets): filtered, network and power on, nothing else. */
inline Settings defaults() { return Settings{}; }

/** Per-user override file values by environment name (FARSIDE_VIRTUAL_SYSTEM_BUS...), refusing any other name. */
inline std::optional<QMap<QString, QString>> overrideValues(const QMap<QString, QString> &environment, QString *error = nullptr)
{
    QMap<QString, QString> result;
    for (auto it = environment.cbegin(); it != environment.cend(); ++it) {
        QString key;
        for (const auto &entry : keyTable())
            if (it.key() == QStringLiteral("FARSIDE_VIRTUAL_") + QLatin1String(entry.suffix)) key = QLatin1String(entry.key);
        if (key.isEmpty()) {
            if (error) *error = QStringLiteral("unexpected key %1").arg(it.key().left(64));
            return {};
        }
        result.insert(key, it.value());
    }
    if (error) error->clear();
    return result;
}

/** What the launcher and the proxy get. */
struct Plan {
    Mode mode = Mode::None;
    QStringList proxyRules; // filtered only: --see/--talk/--call/--broadcast arguments, never --filter
    bool polkitAgent = false;
    bool cupsSocket = false;
    QString note; // why the effective mode is below the requested one, empty otherwise
};

namespace detail
{
inline void pathRules(QStringList &rules, const QString &name, const QString &methodAndPath)
{
    rules.append(QStringLiteral("--call=%1=%2").arg(name, methodAndPath));
}
inline void readRules(QStringList &rules, const QString &name, const QString &basePath, const QStringList &methods)
{
    rules.append(QStringLiteral("--see=") + name);
    for (const auto &method : methods) {
        pathRules(rules, name, method + QLatin1Char('@') + basePath);
        pathRules(rules, name, method + QStringLiteral("@") + basePath + QStringLiteral("/*"));
    }
    rules.append(QStringLiteral("--broadcast=%1=@%2").arg(name, basePath));
    rules.append(QStringLiteral("--broadcast=%1=@%2/*").arg(name, basePath));
}
}

/** The proxy rules for the enabled services, in a stable order. */
inline QStringList proxyRules(const Settings &settings)
{
    QStringList rules;
    const QStringList common{QStringLiteral("org.freedesktop.DBus.Properties.Get"), QStringLiteral("org.freedesktop.DBus.Properties.GetAll"),
                             QStringLiteral("org.freedesktop.DBus.Introspectable.Introspect")};
    if (settings.network) {
        detail::readRules(rules, QStringLiteral("org.freedesktop.NetworkManager"), QStringLiteral("/org/freedesktop/NetworkManager"),
                          common + QStringList{QStringLiteral("org.freedesktop.NetworkManager.GetDevices"), QStringLiteral("org.freedesktop.NetworkManager.GetAllDevices"),
                                               QStringLiteral("org.freedesktop.NetworkManager.GetDeviceByIpIface"),
                                               QStringLiteral("org.freedesktop.NetworkManager.Settings.ListConnections"),
                                               QStringLiteral("org.freedesktop.NetworkManager.Settings.GetConnectionByUuid"),
                                               QStringLiteral("org.freedesktop.NetworkManager.Settings.Connection.GetSettings"),
                                               QStringLiteral("org.freedesktop.NetworkManager.Device.Wireless.GetAllAccessPoints"),
                                               QStringLiteral("org.freedesktop.NetworkManager.Device.Wireless.GetAccessPoints")});
    }
    if (settings.power) {
        detail::readRules(rules, QStringLiteral("org.freedesktop.UPower"), QStringLiteral("/org/freedesktop/UPower"),
                          common + QStringList{QStringLiteral("org.freedesktop.UPower.EnumerateDevices"), QStringLiteral("org.freedesktop.UPower.GetDisplayDevice"),
                                               QStringLiteral("org.freedesktop.UPower.GetCriticalAction")});
        QStringList login;
        for (const auto *name : {"GetSession", "GetSessionByPID", "GetSeat", "GetUser", "GetUserByPID", "ListSessions", "ListSeats", "ListUsers", "CanPowerOff", "CanReboot",
                                 "CanSuspend", "CanHibernate", "CanHybridSleep", "CanSuspendThenHibernate"})
            login.append(QStringLiteral("org.freedesktop.login1.Manager.") + QLatin1String(name));
        detail::readRules(rules, QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"), common + login);
    }
    if (settings.packages) rules.append({QStringLiteral("--talk=org.freedesktop.PackageKit"), QStringLiteral("--talk=org.freedesktop.fwupd")});
    if (settings.polkit) rules.append(QStringLiteral("--talk=org.freedesktop.PolicyKit1"));
    if (settings.bluetooth) rules.append(QStringLiteral("--talk=org.bluez"));
    if (settings.printing) rules.append(QStringLiteral("--talk=org.opensuse.CupsPkHelper.Mechanism"));
    for (const auto &name : settings.extra) rules.append(QStringLiteral("--talk=") + name);
    return rules;
}

/** The single grammar for one proxy argument. The launcher repeats it (same pattern) before the proxy command line is built. */
inline bool validProxyArgument(const QString &argument)
{
    static const QRegularExpression grammar(QStringLiteral("\\A(--(see|talk)=[A-Za-z0-9_.*-]+|--(call|broadcast)=[A-Za-z0-9_.*-]+=[A-Za-z0-9_.*@/-]+)\\z"));
    return argument.size() <= 512 && grammar.match(argument).hasMatch();
}

/** The effective plan. Filtered degrades to none without the proxy or the real system socket; full needs the socket. */
inline Plan plan(const Settings &settings, bool proxyInstalled, bool systemSocketPresent)
{
    Plan result;
    if (settings.mode == Mode::None) return result;
    if (!systemSocketPresent) {
        result.note = QStringLiteral("%1 requested, but %2 does not exist; this desktop has no system bus").arg(modeName(settings.mode), SystemSocket);
        return result;
    }
    if (settings.mode == Mode::Filtered) {
        if (!proxyInstalled) {
            result.note = QStringLiteral("filtered requested, but xdg-dbus-proxy is not installed; this desktop has no system bus");
            return result;
        }
        result.proxyRules = proxyRules(settings);
    }
    result.mode = settings.mode;
    // These start or bind something besides the bus, so they apply in full mode too.
    result.polkitAgent = settings.polkit;
    result.cupsSocket = settings.printing;
    return result;
}

/** Per-user drop-in: /etc/farside/virtual-session.d/<login>.conf. */
inline bool validLogin(const QString &login)
{
    static const QRegularExpression grammar(QStringLiteral("\\A[a-z_][a-z0-9_.-]{0,31}\\z"));
    return grammar.match(login).hasMatch();
}
struct UserOverride {
    enum class State { Absent, Applied, Invalid } state = State::Absent;
    Settings settings; // the global values when Absent; none-mode when Invalid
    QString error;
};
/** `readFile` returns the file's bytes after checking it is safe (root-owned, regular, not writable by others, bounded); the production
 * reader is `readTrustedFile`. A file that exists but fails any check, or has any invalid value or unexpected key, yields none-mode. */
struct FileRead {
    bool exists = false;
    QByteArray bytes;
    QString error; // non-empty: unsafe or unreadable
};
inline FileRead readTrustedFile(const QString &directory, const QString &name, uid_t owner = 0)
{
    FileRead result;
    const QString path = directory + QLatin1Char('/') + name;
    struct stat directoryStat {};
    if (lstat(QFile::encodeName(directory).constData(), &directoryStat)) return result; // no directory: no overrides
    if (!S_ISDIR(directoryStat.st_mode) || directoryStat.st_uid != owner || (directoryStat.st_mode & 0022)) {
        result.exists = true;
        result.error = QStringLiteral("override directory is not trusted");
        return result;
    }
    struct stat fileStat {};
    if (lstat(QFile::encodeName(path).constData(), &fileStat)) return result;
    result.exists = true;
    if (!S_ISREG(fileStat.st_mode) || fileStat.st_uid != owner || (fileStat.st_mode & 0022) || fileStat.st_nlink != 1 || fileStat.st_size > 65536) {
        result.error = QStringLiteral("override file is not trusted");
        return result;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("override file is unreadable");
        return result;
    }
    result.bytes = file.read(65537);
    if (result.bytes.size() > 65536) result.error = QStringLiteral("override file is oversized");
    return result;
}
template<typename ParseEnvironment>
inline UserOverride resolveUser(const Settings &global, const QString &login, const FileRead &file, ParseEnvironment parseEnvironment)
{
    UserOverride result;
    result.settings = global;
    if (!validLogin(login) || !file.exists) return result;
    const auto invalid = [&](const QString &why) {
        result.state = UserOverride::State::Invalid;
        result.error = why;
        result.settings = Settings{};
        result.settings.mode = Mode::None;
        return result;
    };
    if (!file.error.isEmpty()) return invalid(file.error);
    QString error;
    const auto environment = parseEnvironment(file.bytes, &error);
    if (!error.isEmpty()) return invalid(error);
    const auto values = overrideValues(environment, &error);
    if (!values) return invalid(error);
    const auto merged = apply(global, *values, &error);
    if (!merged) return invalid(error);
    result.state = UserOverride::State::Applied;
    result.settings = *merged;
    return result;
}
} // namespace KRdp::VirtualSystemBus
