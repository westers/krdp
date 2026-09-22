// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionMaintenanceGuard.h"
#include <QFile>
#include <QRegularExpression>
#include <QUuid>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace KRdp {
namespace {
bool fail(QString *error, const char *message) { if (error) *error = QString::fromLatin1(message); return false; }
bool uuid(const QString &s) {
    return QRegularExpression(QStringLiteral("\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\z")).match(s).hasMatch()
        && !QUuid(s).isNull();
}
bool profile(const QString &s) {
    return QRegularExpression(QStringLiteral("\\A[0-9a-f]{64}\\z")).match(s).hasMatch();
}
QString boot() {
    QFile file(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.read(128)).trimmed() : QString();
}
bool same(int a, int b) {
    struct stat x{}, y{};
    return !fstat(a, &x) && !fstat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
bool regular(int fd, uid_t owner) {
    struct stat st{};
    return fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == owner && st.st_gid == owner
        && (st.st_mode & 07777) == 0600 && st.st_nlink == 1;
}
int directory(const QString &path, uid_t owner, bool fixture) {
    if (!path.startsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('/'))) return -1;
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat root{};
    if (fd < 0 || fstat(fd, &root) || !S_ISDIR(root.st_mode) || root.st_uid || root.st_gid || (root.st_mode & 0022)) {
        if (fd >= 0) ::close(fd);
        return -1;
    }
    const auto parts = path.mid(1).split(QLatin1Char('/'));
    for (int i = 0; fd >= 0 && i < parts.size(); ++i) {
        const auto &part = parts[i];
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")) { ::close(fd); return -1; }
        int next = openat(fd, QFile::encodeName(part).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(fd); fd = next;
        struct stat st{};
        const bool last = i == parts.size() - 1;
        if (fd < 0) return -1;
        const bool safe = !fstat(fd, &st) && S_ISDIR(st.st_mode)
            && (last ? st.st_uid == owner && st.st_gid == owner && (st.st_mode & 07777) == 0700
                     : (fixture || (st.st_uid == 0 && st.st_gid == 0 && !(st.st_mode & 0022))));
        if (!safe) { ::close(fd); return -1; }
    }
    return fd;
}
QByteArray bytes(const VirtualSessionMaintenanceGuard::State &s) {
    return QByteArray("KRDP-MAINTENANCE-1\n") + (s.clean ? "clean\n" : "blocked\n") + s.boot.toLatin1() + '\n'
        + s.epoch.toLatin1() + '\n' + (s.clean ? s.profile.toLatin1() + '\n' : QByteArray());
}
}
void VirtualSessionMaintenanceGuard::Lease::close() {
    // Never explicitly unlock: a fork child closing its copy must not release
    // the parent's shared open-file-description lock.
    if (m_lock >= 0) ::close(std::exchange(m_lock, -1));
    if (m_directory >= 0) ::close(std::exchange(m_directory, -1));
    m_packages.reset(); // Gate first, then backend, then frontend.
}
VirtualSessionMaintenanceGuard::Lease::~Lease() { close(); }
VirtualSessionMaintenanceGuard::Lease::Lease(Lease &&other) noexcept { *this = std::move(other); }
VirtualSessionMaintenanceGuard::Lease &VirtualSessionMaintenanceGuard::Lease::operator=(Lease &&other) noexcept {
    if (this == &other) return *this;
    close();
    m_directory = std::exchange(other.m_directory, -1); m_lock = std::exchange(other.m_lock, -1);
    m_process = other.m_process; m_owner = other.m_owner; m_exclusive = other.m_exclusive;
    m_fixture = other.m_fixture; m_path = std::move(other.m_path); m_boot = std::move(other.m_boot);
    m_packages = std::move(other.m_packages); other.m_packages.reset();
    return *this;
}
bool VirtualSessionMaintenanceGuard::Lease::associated(QString *error) const {
    if (m_packages && !m_packages->associated(error)) return false;
    if (m_process != getpid() || m_directory < 0 || !regular(m_lock, m_owner)) return fail(error, "Invalid or inherited maintenance lease");
    const int dir = directory(m_path, m_owner, m_fixture);
    const int lock = dir < 0 ? -1 : openat(dir, "lock", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    const bool valid = dir >= 0 && lock >= 0 && same(dir, m_directory) && regular(lock, m_owner) && same(lock, m_lock);
    if (lock >= 0) ::close(lock);
    if (dir >= 0) ::close(dir);
    return valid || fail(error, "Maintenance directory or lock identity changed");
}
int VirtualSessionMaintenanceGuard::Lease::stateFile(State &s, QString *error) const {
    if (!associated(error)) return -1;
    int fd = openat(m_directory, "state", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    char buffer[256], tail;
    struct stat st{};
    bool valid = regular(fd, m_owner) && !fstat(fd, &st) && st.st_size > 0 && st.st_size <= 256;
    const ssize_t size = valid ? ::read(fd, buffer, st.st_size) : -1;
    valid = valid && size == st.st_size && ::read(fd, &tail, 1) == 0;
    if (valid) {
        const QByteArray data(buffer, size);
        const auto fields = data.split('\n');
        s.clean = fields.value(1) == "clean";
        s.boot = QString::fromLatin1(fields.value(2)); s.epoch = QString::fromLatin1(fields.value(3));
        s.profile = s.clean ? QString::fromLatin1(fields.value(4)) : QString();
        valid = (s.clean || fields.value(1) == "blocked") && uuid(s.boot) && uuid(s.epoch)
            && (!s.clean || profile(s.profile)) && data == bytes(s);
    }
    if (!valid || !associated(error)) {
        if (fd >= 0) ::close(fd);
        fail(error, "Missing, unsafe or malformed maintenance state"); return -1;
    }
    return fd;
}
std::optional<VirtualSessionMaintenanceGuard::State> VirtualSessionMaintenanceGuard::Lease::read(QString *error) const {
    State s; int fd = stateFile(s, error);
    if (fd < 0) return {};
    ::close(fd); return s;
}
bool VirtualSessionMaintenanceGuard::Lease::admit(const QString &approved, QString *error) {
    State s; int fd = stateFile(s, error);
    if (fd < 0) return false;
    bool ok = profile(approved) && s.clean && s.boot == m_boot && s.profile == approved;
    if (ok) ok = !fsync(fd) && !fsync(m_directory) && associated(error);
    // Confirm that the validated/synced descriptor is still the admission file.
    int named = openat(m_directory, "state", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    ok = ok && regular(named, m_owner) && same(fd, named);
    if (named >= 0) ::close(named);
    ::close(fd);
    return ok || fail(error, "Maintenance admission blocked or durability uncertain");
}
VirtualSessionMaintenanceGuard::Publication VirtualSessionMaintenanceGuard::Lease::publish(const State &s, QString *error) {
    if (!m_exclusive || !associated(error) || !uuid(s.boot) || !uuid(s.epoch)
        || (s.clean ? !profile(s.profile) : !s.profile.isEmpty())) {
        fail(error, "Invalid maintenance publication lease or state"); return Publication::FailedBeforeRename;
    }
    // Refuse unsafe existing objects, but allow explicit blocked initialization
    // of absent state. Never bootstrap clean state.
    struct stat st{};
    if (!fstatat(m_directory, "state", &st, AT_SYMLINK_NOFOLLOW)) {
        if (!read(error)) return Publication::FailedBeforeRename;
    } else if (errno != ENOENT) return Publication::FailedBeforeRename;
    const QByteArray name = ".state-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
    int fd = openat(m_directory, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    const auto data = bytes(s);
    bool ok = regular(fd, m_owner) && ::write(fd, data.constData(), data.size()) == data.size() && !fsync(fd) && associated(error);
    if (fd >= 0) ::close(fd);
    // Leave partial temporary evidence untouched on uncertainty; never admitted.
    if (!ok || renameat(m_directory, name.constData(), m_directory, "state")) {
        fail(error, "Maintenance publication failed before rename; writer forbidden"); return Publication::FailedBeforeRename;
    }
    if (fsync(m_directory) || !associated(error)) {
        fail(error, "Maintenance publication durability uncertain; writer forbidden"); return Publication::UncertainAfterRename;
    }
    return Publication::Durable;
}
VirtualSessionMaintenanceGuard::Publication VirtualSessionMaintenanceGuard::Lease::block(QString *transaction, QString *error) {
    State s{false, m_boot, QUuid::createUuid().toString(QUuid::WithoutBraces), {}};
    if (transaction) *transaction = s.epoch;
    return publish(s, error);
}
VirtualSessionMaintenanceGuard::Publication VirtualSessionMaintenanceGuard::Lease::publishClean(const QString &transaction, const QString &approved, QString *error) {
    const auto s = read(error);
    if (!m_exclusive || !s || s->clean || s->boot != m_boot || s->epoch != transaction || !profile(approved)) {
        fail(error, "Stale or invalid maintenance validation transaction"); return Publication::FailedBeforeRename;
    }
    return publish({true, m_boot, QUuid::createUuid().toString(QUuid::WithoutBraces), approved}, error);
}
std::optional<VirtualSessionMaintenanceGuard::Lease> VirtualSessionMaintenanceGuard::acquire(
    const QString &path, uid_t owner, const QString &bootId, bool exclusive, bool fixture, QString *error) {
    if (!uuid(bootId)) { fail(error, "Invalid boot identity"); return {}; }
    Lease lease;
    lease.m_path = path; lease.m_owner = owner; lease.m_boot = bootId; lease.m_exclusive = exclusive;
    lease.m_fixture = fixture; lease.m_process = getpid();
    lease.m_directory = directory(path, owner, fixture);
    if (lease.m_directory >= 0) lease.m_lock = openat(lease.m_directory, "lock", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (!regular(lease.m_lock, owner) || flock(lease.m_lock, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) || !lease.associated(error)) {
        fail(error, "Maintenance gate busy, missing or unsafe"); return {};
    }
    return lease;
}
std::optional<VirtualSessionMaintenanceGuard::Lease> VirtualSessionMaintenanceGuard::admission(const QString &approved, QString *error) {
    if (getuid() || geteuid()) { fail(error, "Root admission caller required"); return {}; }
    return admissionAt(QStringLiteral("/var/lib/krdp/maintenance"), QStringLiteral("/var/lib/dpkg"), 0, boot(), approved, false, error);
}
std::optional<VirtualSessionMaintenanceGuard::Lease> VirtualSessionMaintenanceGuard::admissionAt(
    const QString &path, const QString &packagePath, uid_t owner, const QString &bootId, const QString &approved, bool fixture, QString *error) {
    auto packages = PackageLease::acquireAt(packagePath, owner, fixture, error);
    if (!packages) return {};
    auto lease = acquire(path, owner, bootId, false, fixture, error);
    if (!lease) return {};
    lease->m_packages = std::move(packages);
    if (!lease->admit(approved, error)) return {};
    return lease;
}
std::optional<VirtualSessionMaintenanceGuard::Lease> VirtualSessionMaintenanceGuard::maintenance(QString *error) {
    if (getuid() || geteuid()) { fail(error, "Root maintenance caller required"); return {}; }
    return acquire(QStringLiteral("/var/lib/krdp/maintenance"), 0, boot(), true, false, error);
}
}
