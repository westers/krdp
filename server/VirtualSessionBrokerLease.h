// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QFileInfo>
#include <QFile>
#include <memory>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace KRdp {
/** One broker endpoint owner. Kernel lock survives neither crash nor teardown.
 * Only reclaim a socket proven refused by the kernel, under this exclusive lock.
 * A live/unresponsive/foreign endpoint is never unlinked. Same UID is trusted.
 */
class VirtualSessionBrokerLease {
public:
    ~VirtualSessionBrokerLease() { if (m_lock >= 0) close(m_lock); }
    VirtualSessionBrokerLease(const VirtualSessionBrokerLease &) = delete;
    VirtualSessionBrokerLease &operator=(const VirtualSessionBrokerLease &) = delete;
    static std::unique_ptr<VirtualSessionBrokerLease> acquire(quint32 uid, const QString &socket)
    {
        const QFileInfo info(socket), parent(info.absolutePath());
        if (!uid || (geteuid() && geteuid() != uid) || info.fileName() != QStringLiteral("worker.sock")
            || socket != parent.absoluteFilePath() + QStringLiteral("/worker.sock")
            || parent.canonicalFilePath() != parent.absoluteFilePath()) return {};
        const auto path = QFile::encodeName(socket);
        if (path.size() >= qsizetype(sizeof(sockaddr_un::sun_path))) return {};
        const int dir = open(QFile::encodeName(parent.absoluteFilePath()).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dir < 0) return {};
        struct Directory { int fd; ~Directory() { close(fd); } } directory{dir};
        struct stat metadata{};
        if (fstat(dir, &metadata) || metadata.st_uid != uid || (metadata.st_mode & 0777) != 0700) return {};
        auto lease = std::unique_ptr<VirtualSessionBrokerLease>(new VirtualSessionBrokerLease);
        lease->m_lock = openat(dir, "broker.lock", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (lease->m_lock >= 0) {
            if (fchown(lease->m_lock, uid, -1) || fchmod(lease->m_lock, 0600)) return {};
        } else {
            if (errno != EEXIST) return {};
            lease->m_lock = openat(dir, "broker.lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        }
        if (lease->m_lock < 0 || fstat(lease->m_lock, &metadata) || !S_ISREG(metadata.st_mode)
            || metadata.st_uid != uid || (metadata.st_mode & 0777) != 0600 || metadata.st_nlink != 1
            || flock(lease->m_lock, LOCK_EX | LOCK_NB)) return {};
        struct stat original{};
        if (fstatat(dir, "worker.sock", &original, AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT) return lease;
            return {};
        }
        // System brokers create root-owned sockets; a root successor may reclaim
        // those too, but only inside the selected user's verified private dir.
        if (!S_ISSOCK(original.st_mode) || (original.st_uid != uid && !(geteuid() == 0 && original.st_uid == 0))
            || original.st_nlink != 1) return {};
        const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (probe < 0) return {};
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.constData(), size_t(path.size()) + 1);
        const int connected = ::connect(probe, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
        const int error = errno;
        close(probe);
        if (connected == 0 || error != ECONNREFUSED) return {};
        struct stat current{};
        if (fstatat(dir, "worker.sock", &current, AT_SYMLINK_NOFOLLOW)
            || current.st_dev != original.st_dev || current.st_ino != original.st_ino
            || unlinkat(dir, "worker.sock", 0)) return {};
        return lease;
    }
private:
    VirtualSessionBrokerLease() = default;
    int m_lock = -1;
};
}
