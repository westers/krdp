// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <sys/file.h>
#include <sys/stat.h>
#include <pwd.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QList>
#include <QRandomGenerator>
#include <QSet>
#include <QString>
#include <QStringList>

#include "ClipboardFileFetch.h"
#include "ClipboardFileLimits.h"
#include "ClipboardFilePush.h"
#include "ClipboardFileStaging.h"

namespace KRdp::HostFileStaging
{
/**
 * OPT-078 phase 3 (spec 3.2 and 4.2): the worker's staging file system for client-to-host copies. POSIX, header-only, thread-confined
 * (the stager runs every call on its one writer thread).
 *
 * Layout: `<cache>/krdp-clip/c-<16 hex>/<seq>/<relative path>`; the cache directory is the user's XDG cache (a fixed place on the
 * home filesystem, never tmpfs, never configurable). `c-<hex>` is this worker process's instance: it holds `.lock` with flock(2) for
 * its whole life, which is how a later worker tells a live instance from the leftover of a crash (the kernel drops the lock when the
 * process dies). Everything below the root is created with openat/mkdirat relative to a directory file descriptor, with O_NOFOLLOW,
 * and files with O_EXCL, mode 0600 (directories 0700): no execute bit is ever set, and nothing is ever followed, overwritten
 * or written outside the root, whatever a (hostile) descriptor or a racing process of the same user does.
 *
 *   - The root is created 0700 and verified on EVERY use: it is a directory, not a symlink, owned by the effective uid, and no group or
 *     other access (a wrong mode is repaired with fchmod and verified again; anything else refuses to stage).
 *   - A file is written as `name.part` and renamed (renameat2 RENAME_NOREPLACE, so a final name that appeared meanwhile is reported
 *     and never replaced) only when complete and fsync'd, so a reader never sees a half file under its final name.
 *   - Chunks must arrive in order, inside the declared size (a peer that sends more is cut off, the file removed).
 *   - Free space: before the first byte and every SpaceCheckIntervalBytes (64 MiB) the staging filesystem is probed (injectable); the
 *     declared remainder plus the reserve must fit, else the transfer is refused up front or, mid-way, aborted and its partial files removed.
 *   - Removal (a replaced copy, the end of the connection, the start-up sweep, a failure) uses unlinkat relative to the dirfd and never
 *     follows a link; a link inside the tree is unlinked itself, never its target.
 * Errors name a Status, never a path or a name.
 */
enum class Status : quint8 {
    Ok,
    NoSpace, ///< the declared remainder plus the reserve does not fit, or a write hit ENOSPC/EDQUOT
    SpaceUnknown, ///< the free-space probe failed: refuse, never guess
    IoError,
    OutOfOrder, ///< a chunk that is not the next expected (index, offset)
    Hostile, ///< a chunk past the declared size, for a file that is not fetched, or for a tree that was tampered with
    Exists, ///< something already occupies the name (a collision is reported, never overwritten)
    RootUnsafe, ///< the staging root is a symlink, owned by someone else, or has a mode that cannot be fixed
    Gone, ///< no such transfer / it was aborted
};

inline const char *statusName(Status status)
{
    switch (status) {
    case Status::Ok: return "ok";
    case Status::NoSpace: return "no-space";
    case Status::SpaceUnknown: return "space-unknown";
    case Status::IoError: return "io-error";
    case Status::OutOfOrder: return "out-of-order";
    case Status::Hostile: return "hostile";
    case Status::Exists: return "exists";
    case Status::RootUnsafe: return "root-unsafe";
    case Status::Gone: return "gone";
    }
    return "unknown";
}

/// The wire's ack status for a staging status (Hostile is reported as Denied).
inline ClipboardFileFetch::AckStatus ackFor(Status status)
{
    using Ack = ClipboardFileFetch::AckStatus;
    switch (status) {
    case Status::Ok: return Ack::Ok;
    case Status::NoSpace: return Ack::NoSpace;
    case Status::OutOfOrder: return Ack::OutOfOrder;
    case Status::Hostile: return Ack::Denied;
    case Status::Exists: return Ack::Exists;
    case Status::Gone: return Ack::Gone;
    case Status::SpaceUnknown:
    case Status::IoError:
    case Status::RootUnsafe: return Ack::IoError;
    }
    return Ack::IoError;
}

/**
 * The user's cache directory for staging: XDG_CACHE_HOME when it is an absolute path, else $HOME/.cache, else the passwd entry's home
 * (a worker started by a system service may have no HOME). Empty when none is usable. Never tmpfs, never configurable (spec 3.2).
 */
inline QString defaultCacheDir(const QByteArray &xdgCacheHome, const QByteArray &home)
{
    if (xdgCacheHome.startsWith('/')) return QString::fromLocal8Bit(xdgCacheHome);
    if (home.startsWith('/')) return QString::fromLocal8Bit(home) + QStringLiteral("/.cache");
    struct passwd pwd {};
    struct passwd *result = nullptr;
    char buffer[4096];
    if (::getpwuid_r(::geteuid(), &pwd, buffer, sizeof buffer, &result) == 0 && result && result->pw_dir && result->pw_dir[0] == '/') {
        return QString::fromLocal8Bit(result->pw_dir) + QStringLiteral("/.cache");
    }
    return {};
}

struct SpaceInfo {
    quint64 freeBytes = 0; ///< f_bavail * f_frsize: what an unprivileged writer can still use
    quint64 totalBytes = 0; ///< f_blocks * f_frsize
};
using SpaceProbe = std::function<std::optional<SpaceInfo>(int directoryFd)>;

inline std::optional<SpaceInfo> probeSpace(int directoryFd)
{
    struct statvfs sv {};
    if (::fstatvfs(directoryFd, &sv) != 0) {
        return std::nullopt;
    }
    return SpaceInfo{quint64(sv.f_bavail) * quint64(sv.f_frsize), quint64(sv.f_blocks) * quint64(sv.f_frsize)};
}

/// When a transfer cannot start (or must stop) for lack of space: the numbers for the user's message.
struct SpaceFailure {
    quint64 freeBytes = 0;
    quint64 neededBytes = 0;
};

struct Config {
    QString cacheDir; ///< the user's cache directory (XDG_CACHE_HOME or ~/.cache); `krdp-clip` is created inside it
    quint64 reserveBytes = 0; ///< ClipboardFileMinFreeBytes; 0 = max(2 GiB, 5% of the filesystem)
    SpaceProbe space = probeSpace;
    std::function<uid_t()> effectiveUid = [] { return ::geteuid(); };
    quint64 spaceCheckIntervalBytes = ClipboardFileLimits::SpaceCheckIntervalBytes;
};

// ---- the janitor ----

struct SweepOps {
    std::function<QList<ClipboardFileStaging::RootEntry>()> list; ///< the root's entries (lstat, never following)
    std::function<bool(const QString &name)> locked; ///< true: a live process holds that instance's lock
    std::function<bool(const QString &name)> removeTree; ///< unlinkat-based, never following links
    std::function<bool(const QString &name)> unlinkOnly; ///< removes the entry itself, never what it points to
};
struct SweepReport {
    int removedTrees = 0;
    int unlinked = 0;
    int kept = 0;
    int leftAlone = 0;
    int failed = 0;
    bool operator==(const SweepReport &) const = default;
};

/// An instance younger than this (mkdir done, lock not yet taken) is treated as live: a starting worker must not lose its directory to a sweep.
constexpr qint64 SweepGraceSeconds = 10;

/**
 * The start-up sweep (spec 3.2 c and d), pure over \a ops: removes instances nobody holds the lock of (a crashed worker's leftovers, a
 * connection that ended), anything not locked that is over 24 h old, unlinks symlinks and stray files named like ours without
 * following them, and leaves names it did not create. An instance whose lock is held is live and kept whatever its age: its owner
 * removes its own old transfers.
 */
inline SweepReport runSweep(const SweepOps &ops, qint64 nowSeconds, qint64 graceSeconds = SweepGraceSeconds)
{
    SweepReport report;
    const auto entries = ops.list ? ops.list() : QList<ClipboardFileStaging::RootEntry>();
    QSet<QString> live;
    for (const auto &entry : entries) {
        if (entry.isDirectory && !entry.isSymlink && ClipboardFileStaging::isConnectionDirName(entry.name)
            && ((nowSeconds - entry.mtimeSeconds) < graceSeconds || (ops.locked && ops.locked(entry.name)))) {
            live.insert(entry.name);
        }
    }
    const auto plan = ClipboardFileStaging::planSweep(entries, live, nowSeconds);
    for (const auto &item : plan) {
        using Action = ClipboardFileStaging::SweepAction;
        switch (item.action) {
        case Action::Keep:
            ++report.kept;
            break;
        case Action::LeaveAlone:
            ++report.leftAlone;
            break;
        case Action::UnlinkOnly:
            if (ops.unlinkOnly && ops.unlinkOnly(item.name)) ++report.unlinked;
            else ++report.failed;
            break;
        case Action::RemoveTree:
            if (live.contains(item.name)) { // too old by the clock, but its owner is alive: not ours to remove
                ++report.kept;
            } else if (ops.removeTree && ops.removeTree(item.name)) {
                ++report.removedTrees;
            } else {
                ++report.failed;
            }
            break;
        }
    }
    return report;
}

namespace Detail
{
constexpr int MaxRemoveDepth = 128;

struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(int value)
        : fd(value)
    {
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept
        : fd(std::exchange(other.fd, -1))
    {
    }
    Fd &operator=(Fd &&other) noexcept
    {
        if (this != &other) {
            reset();
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }
    void reset()
    {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    bool valid() const { return fd >= 0; }
    operator int() const { return fd; }
};

constexpr int DirFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

inline Fd openDirAt(int parent, const QByteArray &name)
{
    return Fd(::openat(parent, name.constData(), DirFlags));
}

inline QByteArray enc(const QString &name)
{
    return name.toUtf8();
}

/// The names in the directory \a dirFd (a fresh open file description, so the caller's offset is untouched), without "." and "..".
inline std::optional<QList<QByteArray>> listDir(int dirFd)
{
    Fd copy(::openat(dirFd, ".", DirFlags));
    if (!copy.valid()) {
        return std::nullopt;
    }
    DIR *stream = ::fdopendir(copy.fd);
    if (!stream) {
        return std::nullopt;
    }
    copy.fd = -1; // the stream owns it now
    QList<QByteArray> names;
    while (const dirent *entry = ::readdir(stream)) {
        const QByteArray name(entry->d_name);
        if (name != "." && name != "..") names.append(name);
    }
    ::closedir(stream);
    return names;
}

/// A space check is due every \a interval staged bytes (the check before the first byte is done by Transfer::create()).
inline bool spaceDue(quint64 sinceCheck, quint64 interval)
{
    return sinceCheck >= interval;
}

/// Removes \a name below \a parent without ever following a link: directories are emptied through their own descriptor, everything else is unlinked itself.
/// A directory on another device (a mount point) is not entered. Returns false when something could not be removed.
inline bool removeTreeAt(int parent, const QByteArray &name, int depth = 0)
{
    struct stat st {};
    if (::fstatat(parent, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT; // already gone
    }
    if (!S_ISDIR(st.st_mode)) {
        return ::unlinkat(parent, name.constData(), 0) == 0 || errno == ENOENT;
    }
    if (depth >= MaxRemoveDepth) {
        return false;
    }
    bool ok = true;
    {
        Fd dir = openDirAt(parent, name);
        if (!dir.valid()) {
            return errno == ENOENT;
        }
        struct stat inner {};
        if (::fstat(dir, &inner) != 0 || inner.st_dev != st.st_dev || inner.st_ino != st.st_ino) {
            return false; // swapped between the lstat and the open: not what we looked at
        }
        const auto children = listDir(dir);
        if (!children) {
            return false;
        }
        for (const QByteArray &child : std::as_const(*children)) {
            struct stat cs {};
            if (::fstatat(dir, child.constData(), &cs, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno != ENOENT) ok = false;
                continue;
            }
            if (S_ISDIR(cs.st_mode) && cs.st_dev != st.st_dev) {
                ok = false; // a mount point inside the tree: left alone
                continue;
            }
            ok = removeTreeAt(dir, child, depth + 1) && ok;
        }
    }
    return (::unlinkat(parent, name.constData(), AT_REMOVEDIR) == 0 || errno == ENOENT) && ok;
}

struct Shared;
}

class Transfer;

/**
 * The staging root and this process's instance directory. Created lazily on first use; the instance's `.lock` is held until the Area is
 * destroyed (or the process ends). Not copyable. Every Transfer keeps the directory descriptors it needs alive on its own.
 */
class Area
{
public:
    /// Opens (creating, 0700) `<cacheDir>/krdp-clip` and this process's instance directory. nullptr and \a status say why not.
    static std::unique_ptr<Area> open(const Config &config, Status *status = nullptr);
    ~Area();
    Area(const Area &) = delete;
    Area &operator=(const Area &) = delete;

    QString rootPath() const;
    QString instanceName() const;
    QString instancePath() const;

    /// Starts a transfer for \a plan under \a seq (an unused number). The staging tree is not created before Transfer::create().
    std::unique_ptr<Transfer> begin(quint64 seq, const ClipboardFilePush::Plan &plan, std::optional<quint64> reserveBytes = std::nullopt);
    /// Removes transfer \a seq's directory (a replaced copy), whether or not a Transfer object still exists. Returns false when something stayed.
    bool removeTransfer(quint64 seq);
    /// Removes every transfer of this instance (the connection ended, control moved).
    bool removeAll();
    /// A clean exit: removes every transfer, the lock file and the instance directory itself (a crash leaves them to the next sweep).
    bool removeInstance();
    /// The start-up sweep of the root (stale instances of crashed or ended workers).
    SweepReport sweep(qint64 nowSeconds);
    /**
     * The same sweep without creating anything: a worker that has not staged yet cleans up after crashed ones at start. Does nothing when the
     * root does not exist, is a symlink, or is not ours.
     */
    static SweepReport sweepStale(const Config &config, qint64 nowSeconds);
    /// Transfers older than \a maxAgeSeconds in this instance are removed (spec 3.2 d, for a worker that outlives a day).
    int removeOlderThan(qint64 nowSeconds, qint64 maxAgeSeconds = ClipboardFileStaging::MaxAgeSeconds);

private:
    friend class Transfer;
    explicit Area(std::shared_ptr<Detail::Shared> shared);
    std::shared_ptr<Detail::Shared> d;
};

namespace Detail
{
struct Shared {
    Config config;
    QString rootPath;
    QString instanceName;
    Fd root;
    Fd instance;
    Fd lock;

    /// Verifies the root on every use (spec 4.2): a directory, owned by us, no group/other bits (repaired once, then verified again).
    Status verifyRoot()
    {
        for (int attempt = 0; attempt < 2; ++attempt) {
            struct stat st {};
            if (::fstat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
                return Status::RootUnsafe;
            }
            if (st.st_uid != config.effectiveUid()) {
                return Status::RootUnsafe;
            }
            if ((st.st_mode & 07777) == 0700) {
                return Status::Ok;
            }
            if (attempt == 0 && ::fchmod(root, 0700) != 0) {
                return Status::RootUnsafe;
            }
        }
        return Status::RootUnsafe;
    }
};
}

/// One client-to-host copy. Thread-confined. Create the tree, write the chunks in order, then offer finishedTopLevelPaths() to the host.
class Transfer
{
public:
    ~Transfer() { closeCurrent(); }
    Transfer(const Transfer &) = delete;
    Transfer &operator=(const Transfer &) = delete;

    quint64 seq() const { return m_seq; }
    QString path() const { return m_path; }
    const ClipboardFilePush::Plan &plan() const { return m_plan; }
    quint64 totalBytes() const { return m_plan.totalBytes; }
    quint64 writtenBytes() const { return m_written; }
    bool aborted() const { return m_aborted; }
    bool complete() const { return !m_aborted && m_nextFetch >= m_fetch.size(); }
    QSet<quint32> completeFiles() const { return m_complete; }
    qsizetype completeCount() const { return m_complete.size(); }

    /**
     * The free-space guard, then the transfer directory, the plan's directories and its empty files. On any failure everything created
     * is removed again. NoSpace / SpaceUnknown: nothing was created (\a failure carries the numbers for NoSpace).
     */
    Status create(SpaceFailure *failure = nullptr)
    {
        if (m_created || m_aborted) return Status::Gone;
        if (const Status safe = m_d->verifyRoot(); safe != Status::Ok) return safe;
        const auto info = m_d->config.space(m_d->root);
        const auto decision = ClipboardFileLimits::checkFreeSpace(info ? std::optional(info->freeBytes) : std::nullopt, info ? std::optional(info->totalBytes) : std::nullopt,
                                                                  m_plan.totalBytes, reserve());
        if (decision.verdict == ClipboardFileLimits::SpaceVerdict::Unknown) return Status::SpaceUnknown;
        if (decision.verdict == ClipboardFileLimits::SpaceVerdict::NotEnough) {
            if (failure) *failure = {decision.freeBytes, decision.neededBytes};
            return Status::NoSpace;
        }
        const QByteArray name = QByteArray::number(m_seq);
        if (::mkdirat(m_d->instance, name.constData(), 0700) != 0) {
            return errno == EEXIST ? Status::Exists : Status::IoError;
        }
        m_created = true;
        m_dir = Detail::openDirAt(m_d->instance, name);
        if (!m_dir.valid()) {
            remove();
            return Status::IoError;
        }
        for (const auto &entry : m_plan.entries) {
            if (!entry.wanted()) continue;
            const Status status = entry.isDirectory ? makeDirectory(entry) : (entry.size == 0 ? makeEmptyFile(entry) : Status::Ok);
            if (status != Status::Ok) {
                remove();
                return status;
            }
        }
        for (const auto &entry : m_plan.entries) {
            if (entry.wanted() && entry.isFile() && entry.size == 0) m_complete.insert(entry.index);
        }
        m_fetch = ClipboardFilePush::fetchList(m_plan);
        m_sinceCheck = 0;
        return Status::Ok;
    }

    /**
     * Writes the next chunk. Chunks must come in order: the files of the plan's fetch list one after the other, each from offset 0 in
     * consecutive ranges, never past the declared size. Hostile or failing chunks abort the transfer (staging removed).
     */
    Status write(quint32 index, quint64 offset, const QByteArray &data, SpaceFailure *failure = nullptr)
    {
        if (!m_created || m_aborted) return Status::Gone;
        if (m_nextFetch >= m_fetch.size()) return fail(Status::Hostile); // nothing more is expected
        if (data.isEmpty()) return fail(Status::Hostile);
        const auto &file = m_fetch.at(m_nextFetch);
        if (index != file.index || offset != m_nextOffset) {
            // A later file, or an offset out of line: not ours to guess.
            return fail(Status::OutOfOrder);
        }
        if (quint64(data.size()) > file.size - offset) return fail(Status::Hostile); // more than declared: cut off
        if (const Status safe = m_d->verifyRoot(); safe != Status::Ok) return fail(safe);
        if (Detail::spaceDue(m_sinceCheck, m_d->config.spaceCheckIntervalBytes)) {
            const auto info = m_d->config.space(m_d->root);
            const auto decision = ClipboardFileLimits::checkFreeSpace(info ? std::optional(info->freeBytes) : std::nullopt,
                                                                      info ? std::optional(info->totalBytes) : std::nullopt, m_plan.totalBytes - m_written,
                                                                      reserve());
            if (decision.verdict == ClipboardFileLimits::SpaceVerdict::Unknown) return fail(Status::SpaceUnknown);
            if (decision.verdict == ClipboardFileLimits::SpaceVerdict::NotEnough) {
                if (failure) *failure = {decision.freeBytes, decision.neededBytes};
                return fail(Status::NoSpace);
            }
            m_sinceCheck = 0;
        }
        if (offset == 0) {
            if (const Status opened = openPart(index); opened != Status::Ok) return fail(opened);
        }
        if (m_fd < 0 || m_fdIndex != index) return fail(Status::Gone);
        quint64 done = 0;
        while (done < quint64(data.size())) {
            const ssize_t n = ::pwrite(m_fd, data.constData() + done, size_t(quint64(data.size()) - done), off_t(offset + done));
            if (n < 0) {
                if (errno == EINTR) continue;
                return fail(errno == ENOSPC || errno == EDQUOT ? Status::NoSpace : Status::IoError);
            }
            if (n == 0) return fail(Status::IoError);
            done += quint64(n);
        }
        m_written += quint64(data.size());
        m_sinceCheck += quint64(data.size());
        m_nextOffset += quint64(data.size());
        if (m_nextOffset == file.size) {
            const Status finished = finishFile(file.index);
            if (finished != Status::Ok) return fail(finished);
            ++m_nextFetch;
            m_nextOffset = 0;
        }
        return Status::Ok;
    }

    /// Absolute paths of the finished top-level units, in plan order (what the host clipboard is offered).
    QStringList finishedTopLevelPaths() const
    {
        QStringList paths;
        for (const QString &name : ClipboardFilePush::finishedTopLevel(m_plan, m_complete)) {
            paths.append(m_path + QLatin1Char('/') + name);
        }
        return paths;
    }

    /// Stops and removes everything this transfer staged (idempotent). Returns false when something stayed.
    bool abort()
    {
        m_aborted = true;
        closeCurrent();
        return remove();
    }

    /// Removes the staged tree without marking the transfer failed (a finished copy being replaced).
    bool remove()
    {
        closeCurrent();
        m_dir.reset();
        if (!m_created) return true;
        m_created = false;
        return Detail::removeTreeAt(m_d->instance, QByteArray::number(m_seq));
    }

private:
    friend class Area;
    Transfer(std::shared_ptr<Detail::Shared> shared, quint64 seq, const ClipboardFilePush::Plan &plan, const QString &path, std::optional<quint64> reserve)
        : m_d(std::move(shared))
        , m_seq(seq)
        , m_plan(plan)
        , m_path(path)
        , m_reserve(reserve)
    {
    }

    quint64 reserve() const { return m_reserve.value_or(m_d->config.reserveBytes); }

    Status fail(Status status)
    {
        abort();
        return status;
    }

    void closeCurrent()
    {
        if (m_fd >= 0) ::close(m_fd);
        m_fd = -1;
        m_fdIndex = ~0u;
        m_parent.reset();
    }

    /// Opens (never creating) the directory of \a components below the transfer directory, one O_NOFOLLOW step at a time.
    Detail::Fd openParent(const QStringList &components, int count, Status *status)
    {
        Detail::Fd current(::dup(m_dir));
        if (!current.valid()) {
            *status = Status::IoError;
            return {};
        }
        for (int i = 0; i < count; ++i) {
            Detail::Fd next = Detail::openDirAt(current, Detail::enc(components.at(i)));
            if (!next.valid()) {
                *status = errno == ELOOP || errno == ENOTDIR ? Status::Hostile : Status::IoError;
                return {};
            }
            current = std::move(next);
        }
        *status = Status::Ok;
        return current;
    }

    Status makeDirectory(const ClipboardFilePush::Entry &entry)
    {
        Detail::Fd current(::dup(m_dir));
        if (!current.valid()) return Status::IoError;
        for (int i = 0; i < entry.components.size(); ++i) {
            const QByteArray part = Detail::enc(entry.components.at(i));
            if (::mkdirat(current, part.constData(), 0700) != 0 && errno != EEXIST) return Status::IoError;
            Detail::Fd next = Detail::openDirAt(current, part);
            if (!next.valid()) return errno == ELOOP || errno == ENOTDIR ? Status::Hostile : Status::IoError;
            current = std::move(next);
        }
        return Status::Ok;
    }

    Status makeEmptyFile(const ClipboardFilePush::Entry &entry)
    {
        Status status = Status::Ok;
        // parents exist for entries listed after their directory; an implied parent is created first
        if (entry.components.size() > 1) {
            ClipboardFilePush::Entry parent;
            parent.components = entry.components.mid(0, entry.components.size() - 1);
            if (const Status made = makeDirectory(parent); made != Status::Ok) return made;
        }
        Detail::Fd parent = openParent(entry.components, int(entry.components.size()) - 1, &status);
        if (!parent.valid()) return status;
        const QByteArray name = Detail::enc(entry.components.last());
        Detail::Fd file(::openat(parent, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!file.valid()) return errno == EEXIST ? Status::Exists : Status::IoError;
        applyTimes(file, entry);
        return Status::Ok;
    }

    static void applyTimes(int fd, const ClipboardFilePush::Entry &entry)
    {
        if (!entry.mtimeSeconds || *entry.mtimeSeconds < 0 || *entry.mtimeSeconds > 0x7fffffffLL) return;
        const struct timespec times[2] = {{0, UTIME_OMIT}, {time_t(*entry.mtimeSeconds), 0}};
        (void)::futimens(fd, times); // best effort: a wrong mtime is not worth failing a copy
    }

    const ClipboardFilePush::Entry *entryFor(quint32 index) const
    {
        return index < quint32(m_plan.entries.size()) ? &m_plan.entries.at(qsizetype(index)) : nullptr;
    }

    Status openPart(quint32 index)
    {
        closeCurrent();
        const auto *entry = entryFor(index);
        if (!entry) return Status::Hostile;
        Status status = Status::Ok;
        if (entry->components.size() > 1) {
            ClipboardFilePush::Entry parent;
            parent.components = entry->components.mid(0, entry->components.size() - 1);
            if (const Status made = makeDirectory(parent); made != Status::Ok) return made;
        }
        m_parent = openParent(entry->components, int(entry->components.size()) - 1, &status);
        if (!m_parent.valid()) return status;
        const QByteArray part = Detail::enc(ClipboardFileStaging::partNameFor(entry->components.last()));
        m_fd = ::openat(m_parent, part.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (m_fd < 0) {
            m_parent.reset();
            return errno == EEXIST ? Status::Exists : Status::IoError;
        }
        m_fdIndex = index;
        return Status::Ok;
    }

    Status finishFile(quint32 index)
    {
        const auto *entry = entryFor(index);
        if (!entry || m_fd < 0 || m_fdIndex != index) return Status::Gone;
        applyTimes(m_fd, *entry);
        if (::fsync(m_fd) != 0) return errno == ENOSPC || errno == EDQUOT ? Status::NoSpace : Status::IoError;
        const QByteArray part = Detail::enc(ClipboardFileStaging::partNameFor(entry->components.last()));
        const QByteArray name = Detail::enc(entry->components.last());
        if (::renameat2(m_parent, part.constData(), m_parent, name.constData(), RENAME_NOREPLACE) != 0) {
            return errno == EEXIST ? Status::Exists : Status::IoError;
        }
        closeCurrent();
        m_complete.insert(index);
        return Status::Ok;
    }

    std::shared_ptr<Detail::Shared> m_d;
    quint64 m_seq;
    ClipboardFilePush::Plan m_plan;
    QString m_path;
    std::optional<quint64> m_reserve;
    QList<ClipboardFilePush::FetchFile> m_fetch;
    qsizetype m_nextFetch = 0;
    quint64 m_nextOffset = 0;
    quint64 m_written = 0;
    quint64 m_sinceCheck = 0;
    QSet<quint32> m_complete;
    Detail::Fd m_dir;
    Detail::Fd m_parent;
    int m_fd = -1;
    quint32 m_fdIndex = ~0u;
    bool m_created = false;
    bool m_aborted = false;
};

inline Area::Area(std::shared_ptr<Detail::Shared> shared)
    : d(std::move(shared))
{
}

inline Area::~Area() = default;

inline std::unique_ptr<Area> Area::open(const Config &config, Status *status)
{
    const auto refuse = [status](Status why) {
        if (status) *status = why;
        return std::unique_ptr<Area>();
    };
    const QByteArray cache = QFile::encodeName(config.cacheDir);
    if (cache.isEmpty() || !cache.startsWith('/')) {
        return refuse(Status::RootUnsafe);
    }
    auto shared = std::make_shared<Detail::Shared>();
    shared->config = config;
    // The cache directory itself may be a symlink (some users keep ~/.cache elsewhere); only what is created below it is held to O_NOFOLLOW.
    Detail::Fd cacheFd(::open(cache.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!cacheFd.valid() && errno == ENOENT) {
        // mkdir -p, every level 0700 (an XDG_CACHE_HOME whose parents do not exist yet, a fresh account): existing levels are left as they are.
        QByteArray built;
        for (const QByteArray &part : cache.split('/')) {
            if (part.isEmpty()) continue;
            built += '/' + part;
            if (::mkdir(built.constData(), 0700) != 0 && errno != EEXIST) return refuse(Status::IoError);
        }
        cacheFd = Detail::Fd(::open(cache.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    }
    if (!cacheFd.valid()) {
        return refuse(Status::IoError);
    }
    if (::mkdirat(cacheFd, "krdp-clip", 0700) != 0 && errno != EEXIST) {
        return refuse(Status::IoError);
    }
    shared->root = Detail::openDirAt(cacheFd, "krdp-clip");
    if (!shared->root.valid()) {
        return refuse(errno == ELOOP || errno == ENOTDIR ? Status::RootUnsafe : Status::IoError); // a symlink (ELOOP) is refused
    }
    if (const Status safe = shared->verifyRoot(); safe != Status::Ok) {
        return refuse(safe);
    }
    shared->rootPath = QDir::cleanPath(config.cacheDir) + QStringLiteral("/krdp-clip");
    // This process's instance: a fresh random name, a lock held until the Area is destroyed.
    for (int attempt = 0; attempt < 8 && !shared->instance.valid(); ++attempt) {
        const QString name = QStringLiteral("c-%1").arg(QRandomGenerator::system()->generate64(), 16, 16, QLatin1Char('0'));
        const QByteArray encoded = name.toUtf8();
        if (::mkdirat(shared->root, encoded.constData(), 0700) != 0) {
            if (errno == EEXIST) continue;
            return refuse(Status::IoError);
        }
        shared->instance = Detail::openDirAt(shared->root, encoded);
        if (!shared->instance.valid()) {
            return refuse(Status::IoError);
        }
        shared->lock = Detail::Fd(::openat(shared->instance, ".lock", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!shared->lock.valid() || ::flock(shared->lock, LOCK_EX | LOCK_NB) != 0) {
            return refuse(Status::IoError);
        }
        shared->instanceName = name;
    }
    if (!shared->instance.valid()) {
        return refuse(Status::IoError);
    }
    if (status) *status = Status::Ok;
    return std::unique_ptr<Area>(new Area(std::move(shared)));
}

inline QString Area::rootPath() const
{
    return d->rootPath;
}
inline QString Area::instanceName() const
{
    return d->instanceName;
}
inline QString Area::instancePath() const
{
    return d->rootPath + QLatin1Char('/') + d->instanceName;
}

inline std::unique_ptr<Transfer> Area::begin(quint64 seq, const ClipboardFilePush::Plan &plan, std::optional<quint64> reserveBytes)
{
    return std::unique_ptr<Transfer>(new Transfer(d, seq, plan, instancePath() + QLatin1Char('/') + QString::number(seq), reserveBytes));
}

inline bool Area::removeTransfer(quint64 seq)
{
    return Detail::removeTreeAt(d->instance, QByteArray::number(seq));
}

inline bool Area::removeAll()
{
    const auto names = Detail::listDir(d->instance);
    if (!names) {
        return false;
    }
    bool ok = true;
    for (const QByteArray &name : std::as_const(*names)) {
        if (name != ".lock") ok = Detail::removeTreeAt(d->instance, name) && ok;
    }
    return ok;
}

inline bool Area::removeInstance()
{
    bool ok = removeAll();
    ok = (::unlinkat(d->instance, ".lock", 0) == 0 || errno == ENOENT) && ok;
    ok = (::unlinkat(d->root, Detail::enc(d->instanceName).constData(), AT_REMOVEDIR) == 0 || errno == ENOENT) && ok;
    return ok;
}

inline int Area::removeOlderThan(qint64 nowSeconds, qint64 maxAgeSeconds)
{
    int removed = 0;
    const auto names = Detail::listDir(d->instance);
    if (!names) {
        return 0;
    }
    for (const QByteArray &name : std::as_const(*names)) {
        if (name == ".lock") continue;
        struct stat st {};
        if (::fstatat(d->instance, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0 && nowSeconds - qint64(st.st_mtime) > maxAgeSeconds
            && Detail::removeTreeAt(d->instance, name)) {
            ++removed;
        }
    }
    return removed;
}

namespace Detail
{
/// The real fs behind a sweep of the root held as \a rootFd; \a ownInstance (may be empty) is always live.
inline SweepOps sweepOps(int rootFd, const QString &ownInstance)
{
    SweepOps ops;
    ops.list = [rootFd]() {
        QList<ClipboardFileStaging::RootEntry> entries;
        const auto listed = listDir(rootFd);
        const QList<QByteArray> names = listed ? *listed : QList<QByteArray>();
        for (const QByteArray &name : names) {
            struct stat st {};
            if (::fstatat(rootFd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
            entries.append({QString::fromUtf8(name), qint64(st.st_mtime), S_ISDIR(st.st_mode), S_ISLNK(st.st_mode)});
        }
        return entries;
    };
    ops.locked = [rootFd, ownInstance](const QString &name) {
        if (!ownInstance.isEmpty() && name == ownInstance) return true;
        Fd dir = openDirAt(rootFd, enc(name));
        if (!dir.valid()) return false;
        Fd lock(::openat(dir, ".lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC));
        if (!lock.valid()) return false; // no lock file: nobody lives there
        if (::flock(lock, LOCK_EX | LOCK_NB) == 0) {
            ::flock(lock, LOCK_UN);
            return false;
        }
        return errno == EWOULDBLOCK;
    };
    ops.removeTree = [rootFd](const QString &name) { return removeTreeAt(rootFd, enc(name)); };
    ops.unlinkOnly = [rootFd](const QString &name) { return ::unlinkat(rootFd, enc(name).constData(), 0) == 0 || errno == ENOENT; };
    return ops;
}
}

inline SweepReport Area::sweep(qint64 nowSeconds)
{
    return runSweep(Detail::sweepOps(d->root, d->instanceName), nowSeconds);
}

inline SweepReport Area::sweepStale(const Config &config, qint64 nowSeconds)
{
    const QByteArray cache = QFile::encodeName(config.cacheDir);
    if (cache.isEmpty() || !cache.startsWith('/')) return {};
    Detail::Fd cacheFd(::open(cache.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!cacheFd.valid()) return {};
    Detail::Fd root = Detail::openDirAt(cacheFd, "krdp-clip"); // never created here; a symlink fails (O_NOFOLLOW)
    struct stat st {};
    if (!root.valid() || ::fstat(root, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != config.effectiveUid()) return {};
    return runSweep(Detail::sweepOps(root, QString()), nowSeconds);
}
}
