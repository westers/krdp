// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerHostRuntimeReader.h"
#include "VirtualSessionLaunchPlan.h"
#include "VirtualSessionProcessIdentity.h"
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
namespace KRdp::BrokerHostRuntime {
namespace {
struct Exec {
    QString path; QStringList argv; bool ignore = false;
    quint64 startReal = 0, startMonotonic = 0, endReal = 0, endMonotonic = 0;
    quint32 pid = 0; int code = 0, status = 0;
};
}
}
Q_DECLARE_METATYPE(KRdp::BrokerHostRuntime::EnvironmentFile)
Q_DECLARE_METATYPE(QList<KRdp::BrokerHostRuntime::EnvironmentFile>)
Q_DECLARE_METATYPE(KRdp::BrokerHostRuntime::Exec)
Q_DECLARE_METATYPE(QList<KRdp::BrokerHostRuntime::Exec>)
namespace KRdp::BrokerHostRuntime {
QDBusArgument &operator<<(QDBusArgument &a, const EnvironmentFile &v) { a.beginStructure(); a << v.path << v.optional; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a, EnvironmentFile &v) { a.beginStructure(); a >> v.path >> v.optional; a.endStructure(); return a; }
namespace {
QDBusArgument &operator<<(QDBusArgument &a, const Exec &v)
{ a.beginStructure(); a << v.path << v.argv << v.ignore << v.startReal << v.startMonotonic << v.endReal << v.endMonotonic << v.pid << v.code << v.status; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a, Exec &v)
{ a.beginStructure(); a >> v.path >> v.argv >> v.ignore >> v.startReal >> v.startMonotonic >> v.endReal >> v.endMonotonic >> v.pid >> v.code >> v.status; a.endStructure(); return a; }
struct Fd {
    int fd = -1;
    explicit Fd(int value) : fd(value) {}
    ~Fd() { if (fd >= 0) ::close(fd); }
};
bool same(const struct stat &a, const struct stat &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
QString stamp(const struct stat &value)
{
    return QString::number(quint64(value.st_dev)) + u":"_s + QString::number(quint64(value.st_ino))
        + u":"_s + QString::number(value.st_size) + u":"_s + QString::number(value.st_mtim.tv_sec)
        + u":"_s + QString::number(value.st_mtim.tv_nsec) + u":"_s + QString::number(value.st_ctim.tv_sec)
        + u":"_s + QString::number(value.st_ctim.tv_nsec);
}
bool parents(const QString &path)
{
    if (!VirtualSessionLaunchPlan::absoluteCleanPath(path)) return false;
    const auto parent = QFileInfo(path).path();
    const auto resolved = QFileInfo(parent).canonicalFilePath();
    if (resolved.isEmpty()) return false;
    for (const auto &start : {parent, resolved}) {
        auto current = start;
        while (true) {
            struct stat state{};
            if (::lstat(QFile::encodeName(current).constData(), &state) || state.st_uid
                || (!S_ISLNK(state.st_mode) && (!S_ISDIR(state.st_mode) || (state.st_mode & 0022)))) return false;
            if (current == u"/") break;
            const auto next = QFileInfo(current).path(); if (next == current) return false; current = next;
        }
    }
    return true;
}
File safeRead(const QString &path)
{
    if (!parents(path)) return {false, {}, u"unsafe-file"_s};
    struct stat entry{}, opened{}, after{}, reference{}, finalEntry{};
    if (::lstat(QFile::encodeName(path).constData(), &entry)) return errno == ENOENT ? File{} : File{false, {}, u"unsafe-file"_s};
    const auto canonical = QFileInfo(path).canonicalFilePath();
    if (entry.st_uid || canonical.isEmpty() || !parents(canonical)) return {true, {}, u"unsafe-file"_s};
    const Fd fd(::open(QFile::encodeName(canonical).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.fd < 0 || ::fstat(fd.fd, &opened) || !S_ISREG(opened.st_mode) || opened.st_uid || opened.st_nlink != 1
        || (opened.st_mode & 0022) || opened.st_size < 0 || opened.st_size > BrokerHostSettings::MaximumBytes) return {true, {}, u"unsafe-file"_s};
    QFile file; if (!file.open(fd.fd, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) return {true, {}, u"unsafe-file"_s};
    auto bytes = file.read(BrokerHostSettings::MaximumBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() != opened.st_size || ::fstat(fd.fd, &after) || !same(opened, after)
        || ::stat(QFile::encodeName(path).constData(), &reference) || !same(opened, reference)
        || ::lstat(QFile::encodeName(path).constData(), &finalEntry) || !same(entry, finalEntry) || !parents(path)) return {true, {}, u"stale"_s};
    return {true, bytes, {}, stamp(entry) + u"/"_s + stamp(opened)};
}
QByteArray procRead(int directory, const char *name)
{
    const Fd fd(::openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    QFile file;
    if (fd.fd < 0 || !file.open(fd.fd, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) return {};
    const auto bytes = file.read(MaximumBytes + 1);
    return file.error() == QFileDevice::NoError && bytes.size() <= MaximumBytes ? bytes : QByteArray();
}
bool alive(int fd)
{
    pollfd value{fd, POLLIN, 0}; int status;
    do { status = ::poll(&value, 1, 0); } while (status < 0 && errno == EINTR);
    return status == 0 && !value.revents;
}
QByteArray uidLine(const QByteArray &status)
{
    for (const auto &line : status.split('\n')) if (line.startsWith("Uid:")) return line;
    return {};
}
Process realProcess(const Unit &unit, const Contract &contract)
{
    Process result; result.pid = unit.pid;
    if (unit.pid <= 1 || unit.pid > 2147483647) return result;
    const Fd pinned(int(::syscall(SYS_pidfd_open, unit.pid, 0)));
    if (pinned.fd < 0 || !alive(pinned.fd)) return result;
    const auto identity = virtualPidfdIdentity(pinned.fd), ticks = virtualProcessStartTime(pid_t(unit.pid));
    if (!identity || !ticks) return result;
    const auto path = u"/proc/"_s + QString::number(unit.pid);
    const Fd directory(::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.fd < 0) return result;
    const auto status = procRead(directory.fd, "status"), cgroup = procRead(directory.fd, "cgroup"), raw = procRead(directory.fd, "cmdline");
    if (status.isEmpty() || raw.isEmpty() || !raw.endsWith('\0') || !cgroup.startsWith("0::") || cgroup.count('\n') != 1 || !cgroup.endsWith('\n')) return result;
    const auto uid = uidLine(status);
    const bool root = uid.startsWith("Uid:") && uid.mid(4).simplified().split(' ') == QList<QByteArray>{"0", "0", "0", "0"};
    const auto executable = path + u"/exe"_s;
    struct stat live{}, installed{};
    const bool executableMatches = root && parents(contract.broker)
        && !::stat(QFile::encodeName(executable).constData(), &live) && !::stat(QFile::encodeName(contract.broker).constData(), &installed)
        && S_ISREG(live.st_mode) && !live.st_uid && !(live.st_mode & 0022) && S_ISREG(installed.st_mode) && !installed.st_uid
        && !(installed.st_mode & 0022) && live.st_dev == installed.st_dev && live.st_ino == installed.st_ino;
    auto pieces = raw.split('\0'); pieces.removeLast();
    QStringList argv;
    if (pieces.size() > 128) return result;
    for (const auto &piece : pieces) {
        const auto value = QString::fromUtf8(piece);
        if (value.toUtf8() != piece || value.size() > 4096) return result;
        argv.append(value);
    }
    const auto after = virtualProcessStartTime(pid_t(unit.pid));
    if (!after || *after != *ticks || procRead(directory.fd, "cmdline") != raw || uidLine(procRead(directory.fd, "status")) != uid
        || procRead(directory.fd, "cgroup") != cgroup || !alive(pinned.fd)) return result;
    result.startTicks = *ticks; result.pidIdentity = *identity; result.root = root; result.executableMatches = executableMatches;
    result.alive = true; result.controlGroup = QString::fromUtf8(cgroup.mid(3, cgroup.size() - 4)); result.argv = argv;
    if (executableMatches) result.executableStamp = stamp(live);
    return result;
}
template<class T> bool typed(const QVariantMap &values, const QString &name, T &target)
{
    const auto value = values.value(name);
    if (value.metaType() != QMetaType::fromType<T>()) return false;
    target = value.value<T>(); return true;
}
template<class T> bool array(const QVariantMap &values, const QString &name, const QString &signature, T &target)
{
    const auto value = values.value(name);
    if (value.metaType() == QMetaType::fromType<T>()) { target = value.value<T>(); return true; }
    if (value.metaType() != QMetaType::fromType<QDBusArgument>() || value.value<QDBusArgument>().currentSignature() != signature) return false;
    target = qdbus_cast<T>(value); return true;
}
class BusReader {
public:
    const QDBusConnection &bus;
    QString owner, failure;
    QElapsedTimer clock;
    explicit BusReader(const QDBusConnection &connection) : bus(connection) { clock.start(); }
    QDBusMessage call(const QString &service, const QString &path, const QString &interface, const QString &method, const QVariantList &args = {}) {
        auto message = QDBusMessage::createMethodCall(service, path, interface, method);
        message.setArguments(args); message.setAutoStartService(false);
        const auto remaining = 15000 - clock.elapsed();
        if (remaining <= 0) { failure = u"unavailable"_s; return {}; }
        auto reply = bus.call(message, QDBus::Block, int(qMin<qint64>(remaining, 1500)));
        if (reply.type() == QDBusMessage::ErrorMessage) failure = reply.errorName() == u"org.freedesktop.DBus.Error.AccessDenied" ? u"denied"_s : u"unavailable"_s;
        return reply;
    }
    QString managerOwner() {
        const auto reply = call(u"org.freedesktop.DBus"_s, u"/org/freedesktop/DBus"_s, u"org.freedesktop.DBus"_s, u"GetNameOwner"_s, {u"org.freedesktop.systemd1"_s});
        QDBusReply<QString> value(reply); return value.isValid() ? value.value() : QString();
    }
    bool trustedOwner(bool requirePidOne) {
        owner = managerOwner(); if (owner.isEmpty() || !owner.startsWith(u':')) return false;
        const auto user = call(u"org.freedesktop.DBus"_s, u"/org/freedesktop/DBus"_s, u"org.freedesktop.DBus"_s, u"GetConnectionUnixUser"_s, {owner});
        QDBusReply<quint32> uid(user); if (!uid.isValid() || uid.value()) { failure = u"denied"_s; return false; }
        if (requirePidOne) {
            QDBusReply<quint32> pid(call(u"org.freedesktop.DBus"_s, u"/org/freedesktop/DBus"_s, u"org.freedesktop.DBus"_s, u"GetConnectionUnixProcessID"_s, {owner}));
            if (!pid.isValid() || pid.value() != 1) { failure = u"denied"_s; return false; }
        }
        return true;
    }
    bool read(Scope scope, Unit &unit) {
        const auto loaded = call(owner, u"/org/freedesktop/systemd1"_s, u"org.freedesktop.systemd1.Manager"_s, u"LoadUnit"_s, {unitName(scope)});
        if (loaded.type() == QDBusMessage::ErrorMessage && loaded.errorName() == u"org.freedesktop.systemd1.NoSuchUnit") { failure.clear(); unit.id = unitName(scope); unit.loadState = u"not-found"_s; return true; }
        QDBusReply<QDBusObjectPath> object(loaded);
        if (!object.isValid() || !QRegularExpression(u"\\A/org/freedesktop/systemd1/unit/[A-Za-z0-9_]+\\z"_s).match(object.value().path()).hasMatch()) { if (failure.isEmpty()) failure = u"malformed"_s; return false; }
        const auto path = object.value().path();
        const auto get = [&](const QString &interface) {
            return QDBusReply<QVariantMap>(call(owner, path, u"org.freedesktop.DBus.Properties"_s, u"GetAll"_s, {interface}));
        };
        const auto common = get(u"org.freedesktop.systemd1.Unit"_s);
        if (!common.isValid()) return false;
        const auto u = common.value();
        if (!typed(u, u"Id"_s, unit.id) || unit.id != unitName(scope) || !typed(u, u"LoadState"_s, unit.loadState)
            || !typed(u, u"ActiveState"_s, unit.activeState) || !typed(u, u"SubState"_s, unit.subState)
            || !typed(u, u"FragmentPath"_s, unit.fragment) || !typed(u, u"NeedDaemonReload"_s, unit.needsReload)
            || !array(u, u"DropInPaths"_s, u"as"_s, unit.dropIns)) { failure = u"malformed"_s; return false; }
        for (const auto &value : {unit.loadState, unit.activeState, unit.subState})
            if (!QRegularExpression(u"\\A[a-z-]{1,32}\\z"_s).match(value).hasMatch()) { failure = u"malformed"_s; return false; }
        if (unit.loadState == u"not-found") return true;
        const auto details = get(u"org.freedesktop.systemd1.Service"_s);
        if (!details.isValid()) return false;
        const auto s = details.value(); QList<Exec> commands;
        if (!typed(s, u"MainPID"_s, unit.pid) || !typed(s, u"ExecMainStartTimestampMonotonic"_s, unit.started)
            || !typed(s, u"User"_s, unit.user) || !typed(s, u"Type"_s, unit.type) || !typed(s, u"ControlGroup"_s, unit.controlGroup)
            || !typed(s, u"PAMName"_s, unit.pamName) || !array(s, u"Environment"_s, u"as"_s, unit.environment)
            || !array(s, u"PassEnvironment"_s, u"as"_s, unit.passEnvironment) || !array(s, u"UnsetEnvironment"_s, u"as"_s, unit.unsetEnvironment)
            || !array(s, u"EnvironmentFiles"_s, u"a(sb)"_s, unit.files) || !array(s, u"ExecStart"_s, u"a(sasbttttuii)"_s, commands)) { failure = u"malformed"_s; return false; }
        if (unit.pid > 2147483647 || commands.size() > 4 || unit.files.size() > 32 || unit.dropIns.size() > 64
            || unit.fragment.size() > 4096 || unit.controlGroup.size() > 4096 || unit.user.size() > 256 || unit.pamName.size() > 256) { failure = u"malformed"_s; return false; }
        for (const auto &command : commands) unit.commands.append({command.path, command.argv, command.ignore});
        return true;
    }
};
}
QJsonObject inspect(Scope scope, const QVariantMap &stored, const QString &revision, const QDBusConnection &bus, const ReaderDependencies &dependencies)
{
    const auto contract = installedContract(scope);
    const auto fail = [&](const QString &reason) { return summarize(scope, {}, {}, {}, stored, revision, contract, reason); };
    if (::getuid() || ::geteuid()) return fail(u"denied"_s);
    if (unitName(scope).isEmpty() || !bus.isConnected()) return fail(u"unavailable"_s);
    qDBusRegisterMetaType<EnvironmentFile>(); qDBusRegisterMetaType<QList<EnvironmentFile>>();
    qDBusRegisterMetaType<Exec>(); qDBusRegisterMetaType<QList<Exec>>();
    BusReader reader(bus);
    if (!reader.trustedOwner(dependencies.requirePidOne)) return fail(reader.failure.isEmpty() ? u"unavailable"_s : reader.failure);
    Unit unit;
    if (!reader.read(scope, unit)) return fail(reader.failure.isEmpty() ? u"malformed"_s : reader.failure);
    const auto read = dependencies.readFile ? dependencies.readFile : safeRead;
    QList<File> files;
    for (const auto &file : unit.files) files.append(read(file.path));
    const auto projection = project(scope, unit, files, contract);
    const auto probe = dependencies.readProcess ? dependencies.readProcess : realProcess;
    const bool active = unit.activeState == u"active" && unit.subState == u"running";
    const auto process = active ? probe(unit, contract) : Process();
    Unit after;
    if (!reader.read(scope, after) || unit != after || reader.managerOwner() != reader.owner) return fail(u"stale"_s);
    for (int i = 0; i < files.size(); ++i) {
        const auto fresh = read(unit.files[i].path), previous = files[i];
        if (fresh.exists != previous.exists || fresh.bytes != previous.bytes || fresh.error != previous.error
            || fresh.identity != previous.identity) return fail(u"stale"_s);
    }
    if (active && probe(unit, contract) != process) return fail(u"stale"_s);
    Unit final;
    if (!reader.read(scope, final) || unit != final || reader.managerOwner() != reader.owner) return fail(u"stale"_s);
    auto value = summarize(scope, unit, projection, process, stored, revision, contract);
    return validPublic(scope, value) ? value : fail(u"malformed"_s);
}
}
