// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cerrno>
#include <functional>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QString>

namespace KRdp::SafeTreeRemoval
{
/**
 * Removing a directory tree that a less privileged account can write to, as root (the console host's per-launch worker
 * directory is chowned to the worker's uid). QDir::removeRecursively() walks by path, so that account could swap a
 * subdirectory for a symlink mid-walk and make root delete the link target's contents. This walk never follows a link:
 * every directory is opened with O_NOFOLLOW, checked against the stat that chose it, emptied through its own descriptor
 * and removed with unlinkat; everything else is unlinked itself. A directory on another device (a mount point) is left
 * alone. The technique is the one HostFileStaging.h uses for the clipboard staging tree (copied, not shared, so the two
 * files stay independent).
 */
constexpr int MaxDepth = 128;

class Fd
{
public:
    Fd() = default;
    explicit Fd(int value)
        : m_fd(value)
    {
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept
        : m_fd(std::exchange(other.m_fd, -1))
    {
    }
    Fd &operator=(Fd &&other) noexcept
    {
        if (this != &other) {
            if (m_fd >= 0) ::close(m_fd);
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }
    ~Fd()
    {
        if (m_fd >= 0) ::close(m_fd);
    }
    bool valid() const { return m_fd >= 0; }
    operator int() const { return m_fd; }
    /// Gives up ownership (fdopendir takes it over).
    int release() { return std::exchange(m_fd, -1); }

private:
    int m_fd = -1;
};

constexpr int DirFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

/// The names in \a dirFd, without "." and "..", read through a fresh description so the caller's offset is untouched.
inline bool listDirectory(int dirFd, QList<QByteArray> *names)
{
    Fd copy(::openat(dirFd, ".", DirFlags));
    if (!copy.valid()) return false;
    DIR *stream = ::fdopendir(copy.release());
    if (!stream) return false;
    while (const dirent *entry = ::readdir(stream)) {
        const QByteArray name(entry->d_name);
        if (name != "." && name != "..") names->append(name);
    }
    ::closedir(stream);
    return true;
}

/// Test seam: called with a directory's name after it was examined and before it is opened (a place for a test to swap it).
inline std::function<void(const QByteArray &name)> &raceHookForTest()
{
    static std::function<void(const QByteArray &)> hook;
    return hook;
}

/// Removes \a name below \a parent without following links. False when something could not be removed.
inline bool removeAt(int parent, const QByteArray &name, int depth = 0)
{
    struct stat st{};
    if (::fstatat(parent, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT; // already gone
    }
    if (!S_ISDIR(st.st_mode)) {
        return ::unlinkat(parent, name.constData(), 0) == 0 || errno == ENOENT;
    }
    if (depth >= MaxDepth) return false;
    bool ok = true;
    {
        if (raceHookForTest()) raceHookForTest()(name);
        Fd dir(::openat(parent, name.constData(), DirFlags));
        if (!dir.valid()) return errno == ENOENT;
        struct stat inner{};
        if (::fstat(dir, &inner) != 0 || inner.st_dev != st.st_dev || inner.st_ino != st.st_ino) {
            return false; // swapped between the lstat and the open: not what was looked at
        }
        QList<QByteArray> children;
        if (!listDirectory(dir, &children)) return false;
        for (const QByteArray &child : std::as_const(children)) {
            struct stat cs{};
            if (::fstatat(dir, child.constData(), &cs, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno != ENOENT) ok = false;
                continue;
            }
            if (S_ISDIR(cs.st_mode) && cs.st_dev != st.st_dev) {
                ok = false; // a mount point inside the tree: left alone
                continue;
            }
            ok = removeAt(dir, child, depth + 1) && ok;
        }
    }
    return (::unlinkat(parent, name.constData(), AT_REMOVEDIR) == 0 || errno == ENOENT) && ok;
}

/**
 * Removes the tree at \a path. Only the parent directory is resolved by path (it is the trusted one: the root-owned
 * runtime directory); the last component and everything below it are reached through descriptors and never through a
 * link. A \a path that is itself a symlink is unlinked, its target left alone. True when nothing is left.
 */
inline bool removeTree(const QString &path)
{
    const QByteArray encoded = QFile::encodeName(path);
    const qsizetype slash = encoded.lastIndexOf('/');
    if (encoded.isEmpty() || slash < 0 || slash == encoded.size() - 1) return false;
    const QByteArray parent = slash == 0 ? QByteArray("/") : encoded.left(slash);
    const QByteArray name = encoded.mid(slash + 1);
    if (name == "." || name == "..") return false;
    Fd parentFd(::open(parent.constData(), DirFlags));
    if (!parentFd.valid()) return errno == ENOENT;
    return removeAt(parentFd, name);
}
}
