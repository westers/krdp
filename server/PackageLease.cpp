// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "PackageLease.h"
#include <QFile>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace KRdp {
namespace {
bool fail(QString *error) { if (error) *error = QStringLiteral("Package locks busy, unsupported, missing or unsafe"); return false; }
bool same(int a, int b) {
    struct stat x{}, y{};
    return a >= 0 && b >= 0 && !fstat(a, &x) && !fstat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
bool file(int fd, uid_t owner) {
    struct stat st{};
    // Package modes differ from guard0600 (Sol uses0640). Group membership
    // does not confer UID0 trust: reject group/other-write, exec and special bits.
    return fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1
        && st.st_uid == owner && st.st_gid == owner && !(st.st_mode & 07133) && (st.st_mode & 0400);
}
int directory(const QString &path, uid_t owner, bool fixture) {
    if (!path.startsWith(QLatin1Char('/')) || path.contains(QChar::Null)) return -1;
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) || st.st_uid || st.st_gid || (st.st_mode & 0022)) { if (fd >= 0) ::close(fd); return -1; }
    const auto parts = path.mid(1).split(QLatin1Char('/'));
    for (int i = 0; i < parts.size(); ++i) {
        const auto &part = parts[i];
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")) { ::close(fd); return -1; }
        int next = openat(fd, QFile::encodeName(part).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(fd); fd = next;
        if (fd < 0) return -1;
        const bool last = i == parts.size() - 1;
        if (fstat(fd, &st) || !S_ISDIR(st.st_mode)
            || ((last || !fixture) && (st.st_uid != (last ? owner : 0) || st.st_gid != (last ? owner : 0) || (st.st_mode & 0022)))) {
            ::close(fd); return -1;
        }
    }
    return fd;
}
int lockFile(int dir, const char *name, uid_t owner) {
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    struct flock lock{};
    lock.l_type = F_RDLCK; lock.l_whence = SEEK_SET; lock.l_start = 0; lock.l_len = 0; lock.l_pid = 0;
    if (!file(fd, owner) || fcntl(fd, F_OFD_SETLK, &lock)) { if (fd >= 0) ::close(fd); return -1; }
    return fd;
}
}
void PackageLease::close() {
    // No F_UNLCK: inherited copies share the open file description.
    if (m_backend >= 0) ::close(std::exchange(m_backend, -1));
    if (m_frontend >= 0) ::close(std::exchange(m_frontend, -1));
    if (m_directory >= 0) ::close(std::exchange(m_directory, -1));
}
PackageLease::~PackageLease() { close(); }
PackageLease::PackageLease(PackageLease &&other) noexcept { *this = std::move(other); }
PackageLease &PackageLease::operator=(PackageLease &&other) noexcept {
    if (this == &other) return *this;
    close(); m_directory = std::exchange(other.m_directory, -1);
    m_frontend = std::exchange(other.m_frontend, -1); m_backend = std::exchange(other.m_backend, -1);
    m_owner = other.m_owner; m_process = other.m_process; m_fixture = other.m_fixture; m_path = std::move(other.m_path);
    return *this;
}
bool PackageLease::associated(QString *error) const {
    if (m_process != getpid() || !file(m_frontend, m_owner) || !file(m_backend, m_owner) || same(m_frontend, m_backend)) return fail(error);
    int dir = directory(m_path, m_owner, m_fixture);
    int front = dir < 0 ? -1 : openat(dir, "lock-frontend", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    int back = dir < 0 ? -1 : openat(dir, "lock", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    bool ok = same(dir, m_directory) && file(front, m_owner) && file(back, m_owner) && same(front, m_frontend) && same(back, m_backend);
    if (back >= 0) ::close(back);
    if (front >= 0) ::close(front);
    if (dir >= 0) ::close(dir);
    return ok || fail(error);
}
std::optional<PackageLease> PackageLease::acquireAt(const QString &path, uid_t owner, bool fixture, QString *error) {
    PackageLease lease; lease.m_path = path; lease.m_owner = owner; lease.m_fixture = fixture; lease.m_process = getpid();
    lease.m_directory = directory(path, owner, fixture);
    if (lease.m_directory >= 0) lease.m_frontend = lockFile(lease.m_directory, "lock-frontend", owner);
    if (lease.m_frontend >= 0) lease.m_backend = lockFile(lease.m_directory, "lock", owner);
    if (!lease.associated(error)) return {};
    return lease;
}
std::optional<PackageLease> PackageLease::acquire(QString *error) {
    if (getuid() || geteuid()) { fail(error); return {}; }
    return acquireAt(QStringLiteral("/var/lib/dpkg"), 0, false, error);
}
}
