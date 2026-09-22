// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionServiceScope.h"
#include <QFile>
#include <QSet>
#include <QUuid>
#include <csignal>
#include <climits>
#include <dirent.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace KRdp {
namespace {
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
QByteArray processMembership(pid_t pid)
{
    QFile file(QStringLiteral("/proc/%1/cgroup").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.read(65537);
    return bytes.size() <= 65536 && file.error() == QFileDevice::NoError ? bytes : QByteArray();
}
bool ownedDirectory(int fd)
{
    struct stat st{};
    return fd >= 0 && !fstat(fd, &st) && S_ISDIR(st.st_mode) && !st.st_uid && !(st.st_mode & 0022);
}
}
QByteArray VirtualSessionServiceScope::expectedMembership(const QString &session)
{
    if (QUuid(session).isNull() || QUuid(session).toString(QUuid::WithoutBraces) != session) return {};
    return QByteArray("0::/system.slice/krdp-virtual-session@") + session.toLatin1() + ".service\n";
}
std::unique_ptr<VirtualSessionServiceScope> VirtualSessionServiceScope::open(const QString &session)
{
    const auto membership = expectedMembership(session);
    if (getuid() || geteuid() || membership.isEmpty() || processMembership(getpid()) != membership) return {};
    Fd root(::open("/sys/fs/cgroup", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct statfs fs{};
    if (!ownedDirectory(root.value) || fstatfs(root.value, &fs) || fs.f_type != CGROUP2_SUPER_MAGIC) return {};
    Fd slice(openat(root.value, "system.slice", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!ownedDirectory(slice.value)) return {};
    const QByteArray name = "krdp-virtual-session@" + session.toLatin1() + ".service";
    Fd unit(openat(slice.value, name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!ownedDirectory(unit.value)) return {};
    auto result = std::unique_ptr<VirtualSessionServiceScope>(new VirtualSessionServiceScope(unit.value, membership));
    unit.value = -1;
    if (!result->members()) return {};
    return result;
}
VirtualSessionServiceScope::~VirtualSessionServiceScope() { close(m_fd); }

std::optional<QList<pid_t>> VirtualSessionServiceScope::parseMembers(const QByteArray &bytes, pid_t parent)
{
    if (parent <= 1 || bytes.isEmpty() || bytes.size() > 65536 || !bytes.endsWith('\n')) return {};
    QSet<pid_t> seen;
    QList<pid_t> result;
    const auto entries = bytes.chopped(1).split('\n');
    if (entries.size() > 4096) return {};
    for (const auto &entry : entries) {
        bool valid = false;
        const auto pid = entry.toLongLong(&valid);
        if (!valid || pid <= 1 || pid > INT_MAX || QByteArray::number(pid) != entry) return {};
        // cgroup.procs can contain duplicate PIDs during concurrent movement.
        if (seen.contains(pid_t(pid))) continue;
        seen.insert(pid_t(pid));
        if (pid != parent) result.append(pid_t(pid));
    }
    if (!seen.contains(parent)) return {};
    return result;
}
std::optional<QList<pid_t>> VirtualSessionServiceScope::members() const
{
    if (getuid() || geteuid() || getpid() != m_parent || processMembership(m_parent) != m_membership) return {};
    // Directory ownership alone does not exclude delegation of individual
    // migration/control files. Refuse an observed writable delegation.
    for (const auto *name : {"cgroup.procs", "cgroup.threads", "cgroup.subtree_control"}) {
        struct stat st{};
        if (fstatat(m_fd, name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)
            || st.st_uid != 0 || (st.st_mode & 0022)) return {};
    }
    return readMembers(m_fd, m_parent);
}
std::optional<QList<pid_t>> VirtualSessionServiceScope::readMembers(int group, pid_t parent)
{
    // This nondelegated service must have no descendant cgroups. Do not mistake
    // an empty parent cgroup for extinction when a nested group holds processes.
    Fd directory(openat(group, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.value < 0) return {};
    DIR *stream = fdopendir(directory.value);
    if (!stream) return {};
    directory.value = -1;
    bool safe = true;
    int count = 0;
    errno = 0;
    while (const auto *entry = readdir(stream)) {
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..") continue;
        struct stat st{};
        if (++count > 256 || fstatat(group, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) { safe = false; break; }
        errno = 0;
    }
    if (errno) safe = false;
    closedir(stream);
    if (!safe) return {};
    const int fd = openat(group, "cgroup.procs", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return {};
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { close(fd); return {}; }
    const auto bytes = file.read(65537);
    if (file.error() != QFileDevice::NoError) return {};
    return parseMembers(bytes, parent);
}
std::optional<bool> VirtualSessionServiceScope::descendantsGone() const
{
    const auto processes = members();
    return processes ? std::optional<bool>(processes->isEmpty()) : std::nullopt;
}
std::optional<bool> VirtualSessionServiceScope::onlyKeeperRemains(pid_t verifiedKeeper) const
{
    if (verifiedKeeper <= 1 || verifiedKeeper == m_parent) return {};
    const auto processes = members();
    if (!processes) return {};
    for (const auto pid : *processes) if (pid != verifiedKeeper) return false;
    return true;
}
bool VirtualSessionServiceScope::signalDescendants(int signal) const
{
    if (signal != SIGTERM && signal != SIGKILL) return false;
    const auto processes = members();
    if (!processes) return false;
    std::vector<std::unique_ptr<Fd>> targets;
    for (const auto pid : *processes) {
        auto fd = std::make_unique<Fd>(int(syscall(SYS_pidfd_open, pid, 0)));
        if (fd->value < 0) { if (errno == ESRCH) continue; return false; }
        // Pin identity BEFORE checking membership. If this process exits and
        // the numeric PID is reused afterwards, signaling its pidfd cannot hit
        // the replacement. Refuse an observed move outside our unit. This is
        // not atomic against concurrent migration by another privileged actor;
        // the service contract forbids external cgroup changes during teardown.
        if (processMembership(pid) != m_membership) return false;
        targets.push_back(std::move(fd));
    }
    bool success = true;
    for (const auto &fd : targets)
        if (syscall(SYS_pidfd_send_signal, fd->value, signal, nullptr, 0) && errno != ESRCH) success = false;
    return success;
}
}
