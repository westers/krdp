// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringDecoder>
#include <QUrl>

#include "ClipboardFileDescriptor.h"
#include "ClipboardFileLimits.h"
#include "ClipboardFileNames.h"

namespace KRdp::HostFiles
{
/**
 * OPT-078 phase 2: the worker's side of host-to-client file copy, as code that touches the file system and nothing else (no Wayland,
 * no sockets, no Qt event loop), so it is tested against real directories.
 *
 *  - parseUriList(): the `text/uri-list` of a file manager copy -> absolute local paths (bytes, as the file system has them).
 *  - walk(): one bounded pass over the selection -> a Snapshot (the entries, their identity, the FileGroupDescriptorW).
 *  - Snapshot::read(): a range of one entry, re-opened without ever following a symlink, with the entry's identity (device, inode,
 *    size, mtime) checked before and after the read. A changed or vanished file fails the read; bytes are never padded or truncated.
 *
 * Nothing is copied: the files stay where they are. File names exist only inside the Snapshot and the descriptor; no function here
 * logs, and none puts a name into an error (the results are fixed enums and counts).
 */

// ---- the selection ----

struct UriParse {
    QList<QByteArray> paths; ///< absolute, no `.`/`..`/empty components, no trailing slash, in order, without repeats
    int refused = 0; ///< lines that were not a local file URI (other schemes, remote hosts, relative or malformed paths, the root)
};

inline UriParse parseUriList(const QByteArray &data, qsizetype maxPaths = ClipboardFileLimits::MaxEntries)
{
    UriParse result;
    QSet<QByteArray> seen;
    qsizetype pos = 0;
    while (pos < data.size()) {
        qsizetype end = data.indexOf('\n', pos);
        if (end < 0) end = data.size();
        QByteArray line = data.mid(pos, end - pos).trimmed();
        pos = end + 1;
        if (line.isEmpty() || line.startsWith('#')) continue; // RFC 2483 comments
        const QUrl url = QUrl::fromEncoded(line, QUrl::StrictMode);
        if (!url.isValid() || url.scheme() != QLatin1String("file") || url.hasQuery() || url.hasFragment() || url.userInfo().size()
            || (!url.host().isEmpty() && url.host() != QLatin1String("localhost"))) {
            ++result.refused;
            continue;
        }
        QByteArray path = QByteArray::fromPercentEncoding(url.path(QUrl::FullyEncoded).toLatin1());
        while (path.size() > 1 && path.endsWith('/')) path.chop(1);
        bool good = path.startsWith('/') && path.size() > 1 && !path.contains('\0');
        if (good) {
            for (const auto &part : path.mid(1).split('/')) {
                if (part.isEmpty() || part == "." || part == "..") good = false;
            }
        }
        if (!good) {
            ++result.refused;
            continue;
        }
        if (seen.contains(path)) continue;
        if (result.paths.size() >= maxPaths) {
            ++result.refused;
            continue;
        }
        seen.insert(path);
        result.paths.append(path);
    }
    return result;
}

// ---- the snapshot ----

enum class ReadStatus { Ok, Changed, Gone, Denied, IoError };

struct ReadResult {
    ReadStatus status = ReadStatus::IoError;
    QByteArray data;
};

class Walker;

class Snapshot
{
public:
    struct Entry {
        QList<QByteArray> parts; ///< the real names from the root's parent directory down (bytes)
        int root = 0;
        bool isDirectory = false;
        quint64 size = 0;
        dev_t dev = 0;
        ino_t ino = 0;
        qint64 mtimeSec = 0;
        qint64 mtimeNsec = 0;
    };

    Snapshot() = default;
    Snapshot(const Snapshot &) = delete;
    Snapshot &operator=(const Snapshot &) = delete;
    ~Snapshot()
    {
        for (const auto &open : m_open) ::close(open.fd);
        for (const int fd : m_rootFds) ::close(fd);
    }

    const QList<Entry> &entries() const { return m_entries; }
    quint64 totalBytes() const { return m_totalBytes; }

    /** Read \a length bytes at \a offset of entry \a index. Thread-safe (one reader at a time). */
    ReadResult read(quint32 index, quint64 offset, quint32 length)
    {
        std::lock_guard lock(m_mutex);
        ReadResult result;
        if (index >= quint32(m_entries.size())) {
            result.status = ReadStatus::Denied;
            return result;
        }
        const Entry &entry = m_entries.at(qsizetype(index));
        if (entry.isDirectory || !length || length > ClipboardFileLimits::MaxRangeBytes || offset > ~quint64(0) - length || offset + length > entry.size) {
            result.status = ReadStatus::Denied;
            return result;
        }
        const int fd = fdFor(index, entry, result.status);
        if (fd < 0) return result;
        if (!sameIdentity(fd, entry, result.status)) {
            drop(index);
            return result;
        }
        QByteArray data(qsizetype(length), Qt::Uninitialized);
        quint32 done = 0;
        while (done < length) {
            const ssize_t n = ::pread(fd, data.data() + done, length - done, off_t(offset + done));
            if (n < 0) {
                if (errno == EINTR) continue;
                result.status = ReadStatus::IoError;
                drop(index);
                return result;
            }
            if (n == 0) { // the file got shorter under us
                result.status = ReadStatus::Changed;
                drop(index);
                return result;
            }
            done += quint32(n);
        }
        if (!sameIdentity(fd, entry, result.status)) { // changed while it was being read
            drop(index);
            return result;
        }
        result.status = ReadStatus::Ok;
        result.data = data;
        return result;
    }

    /** Open descriptors cached (for tests: never more than MaxOpen). */
    int openCount() const
    {
        std::lock_guard lock(m_mutex);
        return int(m_open.size());
    }
    static constexpr int MaxOpen = 8;

private:
    friend class Walker;

    struct Open {
        quint32 index;
        int fd;
    };

    void drop(quint32 index)
    {
        for (qsizetype i = 0; i < m_open.size(); ++i) {
            if (m_open[i].index == index) {
                ::close(m_open[i].fd);
                m_open.removeAt(i);
                return;
            }
        }
    }

    int fdFor(quint32 index, const Entry &entry, ReadStatus &status)
    {
        for (qsizetype i = 0; i < m_open.size(); ++i) {
            if (m_open[i].index == index) {
                const Open found = m_open[i];
                m_open.move(i, m_open.size() - 1); // most recently used last
                return found.fd;
            }
        }
        // Walk the real names from the root's parent, each step O_NOFOLLOW: a symlink swapped in after the walk fails the open.
        int current = m_rootFds.at(entry.root);
        bool owned = false;
        for (qsizetype i = 0; i < entry.parts.size(); ++i) {
            const bool last = i == entry.parts.size() - 1;
            const int flags = last ? (O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY) : (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            const int next = ::openat(current, entry.parts.at(i).constData(), flags);
            const int error = errno;
            if (owned) ::close(current);
            if (next < 0) {
                status = error == ENOENT || error == ENOTDIR ? ReadStatus::Gone : error == ELOOP ? ReadStatus::Changed : error == EACCES || error == EPERM ? ReadStatus::Denied : ReadStatus::IoError;
                return -1;
            }
            current = next;
            owned = true;
        }
        if (m_open.size() >= MaxOpen) {
            ::close(m_open.first().fd);
            m_open.removeFirst();
        }
        m_open.append({index, current});
        return current;
    }

    static bool sameIdentity(int fd, const Entry &entry, ReadStatus &status)
    {
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            status = ReadStatus::IoError;
            return false;
        }
        if (st.st_nlink == 0) { // unlinked: the file is gone even though we still hold it
            status = ReadStatus::Gone;
            return false;
        }
        if (!S_ISREG(st.st_mode) || st.st_dev != entry.dev || st.st_ino != entry.ino || quint64(st.st_size) != entry.size || qint64(st.st_mtim.tv_sec) != entry.mtimeSec
            || qint64(st.st_mtim.tv_nsec) != entry.mtimeNsec) {
            status = ReadStatus::Changed;
            return false;
        }
        return true;
    }

    mutable std::mutex m_mutex;
    QList<Entry> m_entries;
    QList<int> m_rootFds; ///< O_PATH descriptors of the selected items' parent directories
    quint64 m_totalBytes = 0;
    QList<Open> m_open;
};

// ---- the walk ----

enum class WalkStatus {
    Ok,
    Empty, ///< nothing offerable (every item vanished, was refused or skipped)
    TooMany, ///< more than MaxEntries (or the path bytes of the list over its cap)
    TooDeep,
    Budget, ///< the time budget ran out
    Failed,
};

struct WalkOptions {
    bool hugeFiles = false; ///< the peer negotiated files over 4 GiB; without it they are skipped
    qint64 budgetMs = ClipboardFileLimits::WalkBudgetMs;
    std::function<qint64()> nowMs; ///< monotonic milliseconds; the steady clock when empty
    /// Directories whose canonical path is at or under one of these are refused as roots (kernel pseudo file systems).
    QList<QByteArray> refusedRoots = {"/proc", "/sys", "/dev"};
};

struct WalkResult {
    WalkStatus status = WalkStatus::Failed;
    std::shared_ptr<Snapshot> snapshot;
    QByteArray descriptor; ///< the FileGroupDescriptorW, entries in snapshot order
    quint32 skipped = 0; ///< entries left out: symlinks, special files, other file systems, unreadable, names, over 4 GiB without huge support
    int count = 0;
};

class Walker
{
public:
    explicit Walker(const WalkOptions &options)
        : m_options(options)
        , m_snapshot(new Snapshot())
    {
        m_start = now();
    }

    WalkResult run(const QList<QByteArray> &paths)
    {
        WalkResult result;
        for (const auto &path : paths) {
            if (!addRoot(path)) break;
        }
        if (m_status != WalkStatus::Ok) {
            result.status = m_status;
            result.skipped = m_skipped;
            return result;
        }
        return finish();
    }

private:
    friend class Snapshot;

    struct Node {
        QList<QString> components; ///< wire components (sanitized for the host to client direction)
    };

    qint64 now() const
    {
        if (m_options.nowMs) return m_options.nowMs();
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool tick()
    {
        if ((++m_ticks & 63) == 0 && now() - m_start > m_options.budgetMs) {
            m_status = WalkStatus::Budget;
            return false;
        }
        return true;
    }

    // A host name -> a wire component. Characters that would be refused or reinterpreted on the client are replaced here
    // (a Linux name may hold ':' or '\' or a control character; refusing the whole selection for that would be hostile to the user).
    static std::optional<QString> wireComponent(const QByteArray &name)
    {
        QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
        const QString text = decoder(name);
        if (decoder.hasError() || text.isEmpty()) return std::nullopt;
        QString out;
        out.reserve(text.size());
        for (qsizetype i = 0; i < text.size();) {
            char32_t cp = text[i].unicode();
            qsizetype units = 1;
            if (QChar::isHighSurrogate(text[i].unicode()) && i + 1 < text.size() && QChar::isLowSurrogate(text[i + 1].unicode())) {
                cp = QChar::surrogateToUcs4(text[i], text[i + 1]);
                units = 2;
            }
            if (ClipboardFileNames::isControl(cp) || ClipboardFileNames::isFormatOrBidi(cp) || cp == U':' || cp == U'\\' || cp == U'/') {
                out.append(QLatin1Char('_'));
            } else {
                out.append(text.mid(i, units));
            }
            i += units;
        }
        const auto checked = ClipboardFileNames::sanitizeComponent(out, false);
        if (checked.reject) return std::nullopt;
        return checked.name;
    }

    bool addRoot(const QByteArray &path)
    {
        if (!tick()) return false;
        const qsizetype slash = path.lastIndexOf('/');
        QByteArray parent = slash <= 0 ? QByteArray("/") : path.left(slash);
        const QByteArray leaf = path.mid(slash + 1);
        char *real = ::realpath(parent.constData(), nullptr);
        if (!real) {
            ++m_skipped; // the item vanished or its directory is unreadable
            return true;
        }
        const QByteArray canonical(real);
        std::free(real);
        for (const auto &refused : m_options.refusedRoots) {
            if (canonical == refused || canonical.startsWith(QByteArray(refused + '/'))) {
                ++m_skipped;
                return true;
            }
        }
        const int parentFd = ::open(canonical.constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (parentFd < 0) {
            ++m_skipped;
            return true;
        }
        const int rootIndex = int(m_snapshot->m_rootFds.size());
        m_snapshot->m_rootFds.append(parentFd);
        struct stat st {};
        if (::fstatat(parentFd, leaf.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            ++m_skipped;
            return true;
        }
        const auto component = wireComponent(leaf);
        if (!component) {
            ++m_skipped;
            return true;
        }
        return visit(rootIndex, parentFd, {leaf}, {*component}, st, st.st_dev);
    }

    // Records \a st as the next entry (a symlink or a special file is skipped) and descends into a directory.
    // \a dirFd is the directory that holds the entry; \a parts / \a components are its names so far.
    bool visit(int rootIndex, int dirFd, const QList<QByteArray> &parts, const QList<QString> &components, const struct stat &st, dev_t rootDev)
    {
        if (!tick()) return false;
        if (int(parts.size()) > ClipboardFileLimits::MaxDepth) {
            m_status = WalkStatus::TooDeep;
            return false;
        }
        if (st.st_dev != rootDev) { // another file system inside the tree
            ++m_skipped;
            return true;
        }
        const bool isDir = S_ISDIR(st.st_mode);
        if (!isDir && !S_ISREG(st.st_mode)) { // symlinks, sockets, FIFOs, devices
            ++m_skipped;
            return true;
        }
        if (!isDir && !m_options.hugeFiles && !ClipboardFileLimits::fitsWithoutHugeSupport(quint64(st.st_size))) {
            ++m_skipped;
            return true;
        }
        if (m_snapshot->m_entries.size() >= qsizetype(ClipboardFileLimits::MaxEntries)) {
            m_status = WalkStatus::TooMany;
            return false;
        }
        Snapshot::Entry entry;
        entry.parts = parts;
        entry.root = rootIndex;
        entry.isDirectory = isDir;
        entry.size = isDir ? 0 : quint64(st.st_size);
        entry.dev = st.st_dev;
        entry.ino = st.st_ino;
        entry.mtimeSec = qint64(st.st_mtim.tv_sec);
        entry.mtimeNsec = qint64(st.st_mtim.tv_nsec);
        m_snapshot->m_entries.append(entry);
        m_components.append(components);
        if (!isDir) {
            m_snapshot->m_totalBytes += entry.size;
            return true;
        }
        const int fd = ::openat(dirFd, parts.last().constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            ++m_skipped; // unreadable directory: listed, but empty
            return true;
        }
        struct stat check {};
        if (::fstat(fd, &check) != 0 || check.st_ino != st.st_ino || check.st_dev != st.st_dev) { // swapped between the stat and the open
            ::close(fd);
            ++m_skipped;
            return true;
        }
        DIR *dir = ::fdopendir(fd);
        if (!dir) {
            ::close(fd);
            ++m_skipped;
            return true;
        }
        QList<QByteArray> names;
        while (const dirent *item = ::readdir(dir)) {
            const char *name = item->d_name;
            if ((name[0] == '.' && name[1] == 0) || (name[0] == '.' && name[1] == '.' && name[2] == 0)) continue;
            names.append(QByteArray(name));
            if (names.size() > qsizetype(ClipboardFileLimits::MaxEntries) + 1) { // a directory of more than the whole list allows
                ::closedir(dir);
                m_status = WalkStatus::TooMany;
                return false;
            }
        }
        std::sort(names.begin(), names.end());
        bool keepGoing = true;
        for (const auto &name : names) {
            struct stat childStat {};
            if (::fstatat(::dirfd(dir), name.constData(), &childStat, AT_SYMLINK_NOFOLLOW) != 0) {
                ++m_skipped;
                continue;
            }
            const auto component = wireComponent(name);
            if (!component) {
                ++m_skipped;
                continue;
            }
            auto childParts = parts;
            childParts.append(name);
            auto childComponents = components;
            childComponents.append(*component);
            if (!visit(rootIndex, ::dirfd(dir), childParts, childComponents, childStat, rootDev)) {
                keepGoing = false;
                break;
            }
        }
        ::closedir(dir);
        return keepGoing;
    }

    WalkResult finish()
    {
        WalkResult result;
        auto &entries = m_snapshot->m_entries;
        if (entries.isEmpty()) {
            result.status = WalkStatus::Empty;
            result.skipped = m_skipped;
            return result;
        }
        // The sanitizer decides the final names (collisions: case- and normalisation-insensitive duplicates get "name (2)").
        // An entry whose relative path would not fit a FILEDESCRIPTORW (259 UTF-16 units) is dropped with its children; a rename can
        // lengthen a name, so this repeats until nothing is over.
        QList<bool> dropped(entries.size(), false);
        QList<ClipboardFileNames::Entry> sanitized;
        for (int round = 0; round < 4; ++round) {
            QList<ClipboardFileNames::RawEntry> raw;
            QList<int> indices;
            for (int i = 0; i < entries.size(); ++i) {
                if (dropped[i]) continue;
                raw.append({m_components.at(i).join(QLatin1Char('\\')), entries.at(i).isDirectory});
                indices.append(i);
            }
            if (raw.isEmpty()) break;
            const auto clean = ClipboardFileNames::sanitize(raw);
            if (!clean.ok()) {
                result.status = clean.reject == ClipboardFileNames::Reject::TooManyEntries || clean.reject == ClipboardFileNames::Reject::TotalPathBytes ? WalkStatus::TooMany
                    : clean.reject == ClipboardFileNames::Reject::TooDeep                                                                             ? WalkStatus::TooDeep
                                                                                                                                                     : WalkStatus::Failed;
                result.skipped = m_skipped;
                return result;
            }
            bool again = false;
            QList<QString> over; // wire paths (with a trailing separator for directories) whose subtree is dropped
            for (int k = 0; k < indices.size(); ++k) {
                const QString wire = clean.entries.at(k).components.join(QLatin1Char('\\'));
                if (wire.size() > ClipboardFileDescriptor::MaxNameUnits) {
                    dropped[indices.at(k)] = true;
                    ++m_skipped;
                    again = true;
                    if (entries.at(indices.at(k)).isDirectory) over.append(wire + QLatin1Char('\\'));
                }
            }
            if (!over.isEmpty()) { // children of a dropped directory are over too (they extend its path)
                for (int k = 0; k < indices.size(); ++k) {
                    if (dropped[indices.at(k)]) continue;
                    const QString wire = clean.entries.at(k).components.join(QLatin1Char('\\'));
                    for (const auto &prefix : over) {
                        if (wire.startsWith(prefix)) {
                            dropped[indices.at(k)] = true;
                            ++m_skipped;
                            break;
                        }
                    }
                }
            }
            if (!again) {
                sanitized = clean.entries;
                // keep only the surviving entries, in order
                QList<Snapshot::Entry> kept;
                for (int k = 0; k < indices.size(); ++k) kept.append(entries.at(indices.at(k)));
                entries = kept;
                break;
            }
            if (round == 3) {
                result.status = WalkStatus::Failed;
                result.skipped = m_skipped;
                return result;
            }
        }
        if (sanitized.isEmpty()) {
            result.status = WalkStatus::Empty;
            result.skipped = m_skipped;
            return result;
        }
        QList<ClipboardFileDescriptor::EncodeItem> items;
        items.reserve(entries.size());
        quint64 total = 0;
        for (int i = 0; i < entries.size(); ++i) {
            const auto &entry = entries.at(i);
            ClipboardFileDescriptor::EncodeItem item;
            item.name = sanitized.at(i).components.join(QLatin1Char('\\'));
            item.isDirectory = entry.isDirectory;
            item.size = entry.size;
            item.writeTime = ClipboardFileDescriptor::fileTimeFromUnixSeconds(entry.mtimeSec, quint32(entry.mtimeNsec));
            items.append(item);
            total += entry.size;
        }
        m_snapshot->m_totalBytes = total;
        const auto encoded = ClipboardFileDescriptor::encode(items);
        if (!encoded.ok()) {
            result.status = encoded.error == ClipboardFileDescriptor::Error::TooManyItems ? WalkStatus::TooMany : WalkStatus::Failed;
            result.skipped = m_skipped;
            return result;
        }
        result.status = WalkStatus::Ok;
        result.descriptor = encoded.data;
        result.skipped = m_skipped;
        result.count = int(entries.size());
        result.snapshot = m_snapshot;
        return result;
    }

    WalkOptions m_options;
    std::shared_ptr<Snapshot> m_snapshot;
    QList<QList<QString>> m_components;
    qint64 m_start = 0;
    quint32 m_ticks = 0;
    quint32 m_skipped = 0;
    WalkStatus m_status = WalkStatus::Ok;
};

inline WalkResult walk(const QList<QByteArray> &paths, const WalkOptions &options = {})
{
    return Walker(options).run(paths);
}
}
