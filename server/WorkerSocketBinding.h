// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <QByteArray>
#include <QFile>
#include <QString>

#include "SafeTreeRemoval.h"

namespace KRdp::WorkerSocketBinding
{
/**
 * Creating the worker socket as the console/virtual broker (root) inside a directory that belongs to the worker's
 * uid. Binding by path lets that account swap the directory (or an ancestor it controls) for a symlink after the
 * broker checked it, so root would unlink and create the socket wherever the link points. Here the directory is
 * opened once without following a link, checked, and every later step (unlink, bind, chmod, cleanup) goes through
 * that descriptor. The socket stays at `<directory>/<name>`, the path the worker connects to.
 */

/// Whether \a info may be used as the socket directory by a process running as \a effectiveUid for a worker running as
/// \a workerUid. An unprivileged broker has nothing to escalate, so only the type matters; root requires the
/// directory to belong to the worker's uid and to be closed to everyone else (the 0700 directory both routes create).
inline bool directoryAcceptable(const struct stat &info, uid_t effectiveUid, uid_t workerUid)
{
    if (!S_ISDIR(info.st_mode)) return false;
    if (effectiveUid != 0) return true;
    return info.st_uid == workerUid && (info.st_mode & 077) == 0;
}

/// Constant-time equality of two byte strings (the worker token). Lengths are not secret (the token is fixed size).
inline bool constantTimeEqual(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size()) return false;
    unsigned int difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i) difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    return difference == 0;
}

/// A verified, held directory plus the file name of the socket inside it.
class Directory
{
public:
    /// Opens the parent of \a socketPath without following a final link and verifies it. False with \a error set otherwise.
    bool open(const QString &socketPath, uid_t workerUid, QString *error)
    {
        close();
        const QByteArray encoded = QFile::encodeName(socketPath);
        const qsizetype slash = encoded.lastIndexOf('/');
        if (encoded.isEmpty() || slash <= 0 || slash == encoded.size() - 1) return fail(error, QStringLiteral("worker socket path must be absolute with a file name"));
        m_name = encoded.mid(slash + 1);
        if (m_name == "." || m_name == "..") return fail(error, QStringLiteral("invalid worker socket name"));
        m_fd = SafeTreeRemoval::Fd(::open(encoded.left(slash).constData(), SafeTreeRemoval::DirFlags));
        struct stat info{};
        if (!m_fd.valid() || ::fstat(m_fd, &info) != 0) return fail(error, QStringLiteral("cannot open the worker socket directory"));
        if (!directoryAcceptable(info, ::geteuid(), workerUid)) return fail(error, QStringLiteral("the worker socket directory is not private to the worker's user"));
        return true;
    }

    bool valid() const { return m_fd.valid(); }
    int fd() const { return m_fd; }
    const QByteArray &name() const { return m_name; }

    /// The directory reached through the descriptor, whatever happened to its path since it was opened.
    QByteArray socketPathViaDescriptor() const { return "/proc/self/fd/" + QByteArray::number(int(m_fd)) + "/" + m_name; }

    /// Removes a previous entry of that name (never following a link). Not an error when nothing is there.
    bool removeStale() const
    {
        struct stat info{};
        if (::fstatat(m_fd, m_name.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;
        if (S_ISDIR(info.st_mode)) return false;
        return ::unlinkat(m_fd, m_name.constData(), 0) == 0 || errno == ENOENT;
    }

    /**
     * Binds and listens a stream socket at `<directory>/<name>` through the held descriptor and makes it connectable
     * by every account (the directory is what keeps others out). Returns the listening descriptor, or -1 with
     * \a error set; nothing is left behind on failure.
     */
    int bindListening(QString *error)
    {
        if (!removeStale()) {
            fail(error, QStringLiteral("cannot remove the previous worker socket"));
            return -1;
        }
        const QByteArray path = socketPathViaDescriptor();
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= qsizetype(sizeof(address.sun_path))) {
            fail(error, QStringLiteral("worker socket name too long"));
            return -1;
        }
        std::memcpy(address.sun_path, path.constData(), size_t(path.size()) + 1);
        const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listener < 0) {
            fail(error, QStringLiteral("cannot create the worker socket"));
            return -1;
        }
        struct stat bound{};
        bool ok = ::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
        const int bindError = errno;
        if (ok) {
            // From here a socket file of ours exists: remember which one so every failure (and close()) removes only that.
            ok = ::fstatat(m_fd, m_name.constData(), &bound, AT_SYMLINK_NOFOLLOW) == 0 && S_ISSOCK(bound.st_mode);
            if (ok) {
                m_device = bound.st_dev;
                m_inode = bound.st_ino;
                m_bound = true;
            }
        }
        // fchmodat with AT_SYMLINK_NOFOLLOW refuses a link planted at the name after the bind.
        if (!ok || ::fchmodat(m_fd, m_name.constData(), 0777, AT_SYMLINK_NOFOLLOW) != 0 || ::listen(listener, 50) != 0) {
            const int reason = ok ? errno : bindError;
            ::close(listener);
            removeOwned();
            fail(error, QStringLiteral("cannot listen on the worker socket: %1").arg(QString::fromLocal8Bit(std::strerror(reason))));
            return -1;
        }
        return listener;
    }

    /// Unlinks the socket this object bound, if the name still refers to it. Never follows or removes anything else.
    void removeOwned()
    {
        if (!m_bound || !m_fd.valid()) return;
        m_bound = false;
        struct stat info{};
        if (::fstatat(m_fd, m_name.constData(), &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISSOCK(info.st_mode) && info.st_dev == m_device
            && info.st_ino == m_inode) {
            ::unlinkat(m_fd, m_name.constData(), 0);
        }
    }

    /// Cleans up and drops the descriptor.
    void close()
    {
        removeOwned();
        m_fd = SafeTreeRemoval::Fd();
        m_name.clear();
    }

private:
    static bool fail(QString *error, const QString &message)
    {
        if (error) *error = message;
        return false;
    }

    SafeTreeRemoval::Fd m_fd;
    QByteArray m_name;
    dev_t m_device = 0;
    ino_t m_inode = 0;
    bool m_bound = false;
};
}
