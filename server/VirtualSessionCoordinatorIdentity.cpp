// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionCoordinatorIdentity.h"
#include "VirtualSessionProcessIdentity.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDeadlineTimer>
#include <QUuid>
#include <QVariantMap>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace KRdp {
namespace {
const QString unitName = QStringLiteral("krdp-maintenance-validate.service");
const QString unitPath = QStringLiteral("/org/freedesktop/systemd1/unit/krdp_2dmaintenance_2dvalidate_2eservice");
const QString managerName = QStringLiteral("org.freedesktop.systemd1");
const QString daemon = QStringLiteral("org.freedesktop.DBus");
QString boot() {
    QFile f(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const auto bytes = f.read(128);
    const auto value = QString::fromLatin1(bytes).trimmed();
    return !QUuid(value).isNull() && QUuid(value).toString(QUuid::WithoutBraces) == value ? value : QString();
}
bool root() {
    uid_t real, effective, saved;
    return !getresuid(&real, &effective, &saved) && !real && !effective && !saved;
}
bool same(int first, int second) {
    struct stat a{}, b{};
    return first >= 0 && second >= 0 && !fstat(first, &a) && !fstat(second, &b)
        && S_ISREG(a.st_mode) && S_ISREG(b.st_mode) && a.st_nlink && b.st_nlink
        && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
}
struct VirtualSessionCoordinatorIdentity::Data {
    explicit Data(const QDBusConnection &connection) : bus(connection) {}
    ~Data() { if (pidfd >= 0) ::close(pidfd); if (exe >= 0) ::close(exe); }
    QDBusConnection bus;
    QString executable, bootId, guid, owner, invocation;
    uid_t managerUid = 0;
    pid_t managerPid = 1, process = 0;
    bool fixture = false;
    int pidfd = -1, exe = -1;
    quint64 ticks = 0, inode = 0;

    bool local() const {
        if (getpid() != process || (!fixture && !root()) || boot() != bootId
            || virtualProcessStartTime(process) != std::optional<quint64>(ticks)
            || virtualPidfdIdentity(pidfd) != std::optional<quint64>(inode)) return false;
        struct pollfd p{pidfd, POLLIN, 0};
        if (::poll(&p, 1, 0) != 0) return false;
        const int named = ::open(QFile::encodeName(executable).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        const int current = ::open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        struct stat st{};
        const bool ok = same(exe, named) && same(exe, current) && !fstat(exe, &st)
            && (fixture || (st.st_uid == 0 && !(st.st_mode & 0022)));
        if (named >= 0) ::close(named);
        if (current >= 0) ::close(current);
        return ok;
    }
    QDBusMessage call(QDeadlineTimer &deadline, const QString &destination, const QString &path,
                      const QString &interface, const QString &method, const QList<QVariant> &args = {}) const {
        if (!bus.isConnected() || deadline.hasExpired()) return {};
        const auto remaining = deadline.remainingTime();
        if (remaining <= 0) return {};
        auto message = QDBusMessage::createMethodCall(destination, path, interface, method);
        message.setAutoStartService(false); message.setArguments(args);
        const auto reply = bus.call(message, QDBus::Block, int(remaining));
        // Qt's public reply service() is empty, not an independently checked
        // sender. Rely on call correlation and pinned unique destinations.
        if (!bus.isConnected() || deadline.hasExpired() || reply.type() != QDBusMessage::ReplyMessage) return {};
        return reply;
    }
    QString string(QDeadlineTimer &deadline, const QString &method, const QList<QVariant> &args = {}) const {
        const auto reply = call(deadline, daemon, QStringLiteral("/org/freedesktop/DBus"), daemon, method, args);
        return reply.signature() == QStringLiteral("s") && reply.arguments().size() == 1
            && reply.arguments()[0].metaType().id() == QMetaType::QString ? reply.arguments()[0].toString() : QString();
    }
    bool credential(QDeadlineTimer &deadline, const QString &method, quint32 expected) const {
        const auto reply = call(deadline, daemon, QStringLiteral("/org/freedesktop/DBus"), daemon, method, {owner});
        return reply.signature() == QStringLiteral("u") && reply.arguments().size() == 1
            && reply.arguments()[0].metaType().id() == QMetaType::UInt && reply.arguments()[0].toUInt() == expected;
    }
    bool endpoint(QDeadlineTimer &deadline) const {
        return string(deadline, QStringLiteral("GetId")) == guid
            && string(deadline, QStringLiteral("GetNameOwner"), {managerName}) == owner
            && credential(deadline, QStringLiteral("GetConnectionUnixUser"), managerUid)
            && credential(deadline, QStringLiteral("GetConnectionUnixProcessID"), quint32(managerPid))
            && bus.isConnected() && !deadline.hasExpired();
    }
    std::optional<QVariantMap> properties(QDeadlineTimer &deadline, const QString &interface) const {
        const auto reply = call(deadline, owner, unitPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"), {interface});
        if (reply.signature() != QStringLiteral("a{sv}") || reply.arguments().size() != 1 || !endpoint(deadline)) return {};
        const QDBusReply<QVariantMap> values(reply);
        return values.isValid() ? std::optional(values.value()) : std::nullopt;
    }
    QString snapshot(QDeadlineTimer &deadline) const {
        if (!local() || !endpoint(deadline)) return {};
        const auto pathReply = call(deadline, owner, QStringLiteral("/org/freedesktop/systemd1"), managerName + QStringLiteral(".Manager"),
                                    QStringLiteral("GetUnitByPID"), {quint32(process)});
        const QDBusReply<QDBusObjectPath> path(pathReply);
        if (pathReply.signature() != QStringLiteral("o") || pathReply.arguments().size() != 1
            || !path.isValid() || path.value().path() != unitPath || !endpoint(deadline)) return {};
        const auto unit = properties(deadline, managerName + QStringLiteral(".Unit"));
        const auto service = properties(deadline, managerName + QStringLiteral(".Service"));
        if (!unit || !service) return {};
        const auto textIs = [](const QVariantMap &map, const char *key, const QString &value) {
            const auto v = map.value(QString::fromLatin1(key));
            return v.metaType().id() == QMetaType::QString && v.toString() == value;
        };
        const auto inv = unit->value(QStringLiteral("InvocationID"));
        const auto pid = service->value(QStringLiteral("MainPID"));
        const auto delegate = service->value(QStringLiteral("Delegate"));
        if (!textIs(*unit, "Id", unitName) || !textIs(*unit, "ActiveState", QStringLiteral("activating"))
            || !textIs(*unit, "SubState", QStringLiteral("start")) || !textIs(*service, "Type", QStringLiteral("oneshot"))
            || pid.metaType().id() != QMetaType::UInt || pid.toUInt() != quint32(process)
            || delegate.metaType().id() != QMetaType::Bool || delegate.toBool()
            || inv.metaType().id() != QMetaType::QByteArray || inv.toByteArray().size() != 16) return {};
        const auto id = QUuid::fromRfc4122(inv.toByteArray());
        if (id.isNull() || !local() || !endpoint(deadline)) return {};
        return id.toString(QUuid::WithoutBraces);
    }
};
VirtualSessionCoordinatorIdentity::VirtualSessionCoordinatorIdentity(std::unique_ptr<Data> value) : d(std::move(value)) {}
VirtualSessionCoordinatorIdentity::~VirtualSessionCoordinatorIdentity() = default;
VirtualSessionCoordinatorIdentity::VirtualSessionCoordinatorIdentity(VirtualSessionCoordinatorIdentity &&) noexcept = default;
VirtualSessionCoordinatorIdentity &VirtualSessionCoordinatorIdentity::operator=(VirtualSessionCoordinatorIdentity &&) noexcept = default;
QString VirtualSessionCoordinatorIdentity::invocation() const { return d ? d->invocation : QString(); }
QString VirtualSessionCoordinatorIdentity::busId() const { return d ? d->guid : QString(); }
QString VirtualSessionCoordinatorIdentity::uniqueOwner() const { return d ? d->owner : QString(); }
QString VirtualSessionCoordinatorIdentity::bootId() const { return d ? d->bootId : QString(); }
bool VirtualSessionCoordinatorIdentity::validBusId(const QString &value) {
    if (value.size() != 32) return false;
    for (const auto c : value)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}
bool VirtualSessionCoordinatorIdentity::validUniqueOwner(const QString &value) {
    if (!value.startsWith(QLatin1Char(':'))) return false;
    bool component = false, dot = false;
    for (qsizetype i = 1; i < value.size(); ++i) {
        const auto c = value[i];
        if (c == QLatin1Char('.')) {
            if (!component) return false;
            dot = true; component = false;
        } else {
            if (!((c >= QLatin1Char('A') && c <= QLatin1Char('Z')) || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                  || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('_') || c == QLatin1Char('-'))) return false;
            component = true;
        }
    }
    return dot && component;
}
bool VirtualSessionCoordinatorIdentity::revalidate(int timeoutMs) const {
    if (!d || timeoutMs <= 0 || timeoutMs > 3000) return false;
    QDeadlineTimer deadline(timeoutMs, Qt::PreciseTimer);
    return d->snapshot(deadline) == d->invocation && d->snapshot(deadline) == d->invocation
        && d->bus.isConnected() && !deadline.hasExpired();
}
std::optional<VirtualSessionCoordinatorIdentity> VirtualSessionCoordinatorIdentity::pin(const QString &executable, int timeoutMs) {
    if (!root()) return {};
    return pinOnBus(QDBusConnection::systemBus(), executable, 0, 1, false, timeoutMs);
}
std::optional<VirtualSessionCoordinatorIdentity> VirtualSessionCoordinatorIdentity::pinOnBus(const QDBusConnection &bus,
    const QString &executable, uid_t managerUid, pid_t managerPid, bool fixture, int timeoutMs) {
    if (timeoutMs <= 0 || timeoutMs > 3000 || !bus.isConnected() || managerPid < 1
        || !executable.startsWith(QLatin1Char('/')) || executable.contains(QChar::Null) || (!fixture && !root())) return {};
    QDeadlineTimer deadline(timeoutMs, Qt::PreciseTimer);
    auto data = std::make_unique<Data>(bus);
    data->fixture = fixture; data->managerUid = managerUid; data->managerPid = managerPid;
    data->process = getpid(); data->executable = executable; data->bootId = boot();
    data->pidfd = int(syscall(SYS_pidfd_open, data->process, 0));
    data->exe = ::open(QFile::encodeName(executable).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    const auto ticks = virtualProcessStartTime(data->process), inode = virtualPidfdIdentity(data->pidfd);
    if (!ticks || !inode || data->bootId.isEmpty()) return {};
    data->ticks = *ticks; data->inode = *inode;
    if (!data->local()) return {};
    data->guid = data->string(deadline, QStringLiteral("GetId"));
    data->owner = data->string(deadline, QStringLiteral("GetNameOwner"), {managerName});
    if (!validBusId(data->guid) || !validUniqueOwner(data->owner)) return {};
    data->invocation = data->snapshot(deadline);
    if (data->invocation.isEmpty() || data->snapshot(deadline) != data->invocation || deadline.hasExpired()) return {};
    return VirtualSessionCoordinatorIdentity(std::move(data));
}
}
