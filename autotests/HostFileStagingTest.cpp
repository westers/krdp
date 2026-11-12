// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 3: the worker's staging file system against real temporary directories (spec 3.2, 4.2): private root verified on every
// use, openat/O_EXCL/O_NOFOLLOW, a temporary file in `<seq>.parts/` then rename, no overwrite, no execute bit, the disk guard with an injected statvfs, the janitor with
// an injected fs and a real one, and failures injected with -Wl,--wrap (pwrite, fsync, fstatvfs, openat, renameat2, fchmod, unlinkat).

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <cstdarg>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "HostFileStaging.h"

using namespace KRdp;
using namespace KRdp::HostFileStaging;
using namespace Qt::StringLiterals;
using ClipboardFileDescriptor::Descriptor;

namespace
{
// ---- injected failures (the link wraps below) ----
struct Fault {
    int error = 0; ///< errno to return; 0 = off
    int skip = 0; ///< let this many matching calls through first
    QByteArray match; ///< openat: only paths ending with this
    int times = -1; ///< fire this many times, then stop (-1 = every time)
    int shortWrite = 0; ///< pwrite: return at most this many bytes per call (0 = off)
    int hits = 0;
    bool fire(const char *path = nullptr)
    {
        if (!error || times == 0) return false;
        if (!match.isEmpty() && (!path || !QByteArray(path).endsWith(match))) return false;
        if (skip > 0) {
            --skip;
            return false;
        }
        if (times > 0) --times;
        ++hits;
        return true;
    }
};
Fault g_pwrite, g_fsync, g_fstatvfs, g_openat, g_renameat2, g_fchmod, g_unlinkat;
}

extern "C" {
ssize_t __real_pwrite(int fd, const void *buf, size_t count, off_t offset);
int __real_fsync(int fd);
int __real_fstatvfs(int fd, struct statvfs *buf);
int __real_openat(int dirfd, const char *path, int flags, ...);
int __real_renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags);
int __real_fchmod(int fd, mode_t mode);
int __real_unlinkat(int dirfd, const char *path, int flags);

ssize_t __wrap_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    if (g_pwrite.fire()) {
        errno = g_pwrite.error;
        return -1;
    }
    if (g_pwrite.shortWrite > 0 && count > size_t(g_pwrite.shortWrite)) count = size_t(g_pwrite.shortWrite);
    return __real_pwrite(fd, buf, count, offset);
}
int __wrap_fsync(int fd)
{
    if (g_fsync.fire()) {
        errno = g_fsync.error;
        return -1;
    }
    return __real_fsync(fd);
}
int __wrap_fstatvfs(int fd, struct statvfs *buf)
{
    if (g_fstatvfs.fire()) {
        errno = g_fstatvfs.error;
        return -1;
    }
    return __real_fstatvfs(fd, buf);
}
int __wrap_openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    if (g_openat.fire(path)) {
        errno = g_openat.error;
        return -1;
    }
    return __real_openat(dirfd, path, flags, mode);
}
int __wrap_renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags)
{
    if (g_renameat2.fire()) {
        errno = g_renameat2.error;
        return -1;
    }
    return __real_renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
}
int __wrap_fchmod(int fd, mode_t mode)
{
    if (g_fchmod.fire()) {
        errno = g_fchmod.error;
        return -1;
    }
    return __real_fchmod(fd, mode);
}
int __wrap_unlinkat(int dirfd, const char *path, int flags)
{
    if (g_unlinkat.fire(path)) {
        errno = g_unlinkat.error;
        return -1;
    }
    return __real_unlinkat(dirfd, path, flags);
}
}

namespace
{
Descriptor file(const QString &name, quint64 size, std::optional<quint64> writeTime = std::nullopt)
{
    Descriptor d;
    d.name = name;
    d.flags = ClipboardFileDescriptor::FdAttributes | ClipboardFileDescriptor::FdFileSize;
    d.attributes = ClipboardFileDescriptor::AttrNormal | 0x20; // ARCHIVE: a descriptor can say anything; the host never sets an execute bit
    d.size = size;
    d.writeTime = writeTime;
    return d;
}
Descriptor dir(const QString &name)
{
    Descriptor d;
    d.name = name;
    d.flags = ClipboardFileDescriptor::FdAttributes;
    d.attributes = ClipboardFileDescriptor::AttrDirectory;
    return d;
}
ClipboardFilePush::Plan planOf(const QList<Descriptor> &items)
{
    auto plan = ClipboardFilePush::makePlan(items, true);
    return plan;
}
QByteArray pattern(quint64 size, int salt = 0)
{
    QByteArray data(int(size), '\0');
    for (int i = 0; i < data.size(); ++i) data[i] = char((i * 31 + salt) & 0xff);
    return data;
}
QByteArray slurp(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<unreadable>");
}
mode_t modeOf(const QString &path)
{
    struct stat st {};
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0 ? st.st_mode : 0;
}
void touch(const QString &path, const QByteArray &data = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}
void ageBy(const QString &path, qint64 seconds)
{
    const struct timespec times[2] = {{time(nullptr) - time_t(seconds), 0}, {time(nullptr) - time_t(seconds), 0}};
    ::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, AT_SYMLINK_NOFOLLOW);
}
QStringList entriesOf(const QString &dirPath)
{
    return QDir(dirPath).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
}

/// A fixed answer for the free-space probe (1 TiB free of 2 TiB): no rig depends on the disk the tests happen to run on (a nearly full
/// disk made most of them fail). Tests that mean the real probe say so (`config.space = probeSpace`) and bring their own reserve.
std::optional<SpaceInfo> plentyOfSpace(int)
{
    return SpaceInfo{1ULL << 40, 2ULL << 40};
}

struct Rig {
    QTemporaryDir cache;
    Config config;
    std::unique_ptr<Area> area;
    Status status = Status::Ok;
    Rig()
    {
        config.cacheDir = cache.path();
        config.space = plentyOfSpace;
    }
    bool open()
    {
        area = Area::open(config, &status);
        return bool(area);
    }
    QString root() const { return cache.filePath(u"krdp-clip"_s); }
};

// every regular file and directory below \a path has no group/other bits and no execute bit on files
void verifyPrivate(const QString &path, bool *ok)
{
    *ok = true;
    QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const mode_t mode = modeOf(it.filePath());
        if (S_ISLNK(mode)) {
            *ok = false;
            return;
        }
        if (mode & 077) *ok = false;
        if (S_ISREG(mode) && (mode & 0111)) *ok = false;
    }
}
}

class HostFileStagingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        g_pwrite = g_fsync = g_fstatvfs = g_openat = g_renameat2 = g_fchmod = g_unlinkat = Fault();
    }

    // ---- the root (spec 4.2) ----
    void rootIsCreatedPrivateWithALockedInstance()
    {
        Rig rig;
        QVERIFY(rig.open());
        QCOMPARE(modeOf(rig.root()) & 07777, mode_t(0700));
        QVERIFY(ClipboardFileStaging::isConnectionDirName(rig.area->instanceName()));
        QCOMPARE(modeOf(rig.area->instancePath()) & 07777, mode_t(0700));
        QCOMPARE(modeOf(rig.area->instancePath() + u"/.lock"_s) & 07777, mode_t(0600));
        // the lock is held: another open file description cannot take it
        const int other = ::open(QFile::encodeName(rig.area->instancePath() + u"/.lock"_s).constData(), O_RDWR);
        QVERIFY(other >= 0);
        QCOMPARE(::flock(other, LOCK_EX | LOCK_NB), -1);
        QCOMPARE(errno, EWOULDBLOCK);
        ::close(other);
        // and released when the area goes
        rig.area.reset();
        const int again = ::open(QFile::encodeName(rig.root() + u"/"_s + entriesOf(rig.root()).first() + u"/.lock"_s).constData(), O_RDWR);
        QVERIFY(again >= 0);
        QCOMPARE(::flock(again, LOCK_EX | LOCK_NB), 0);
        ::close(again);
    }
    void rootThatIsASymlinkIsRefused()
    {
        Rig rig;
        QTemporaryDir victim;
        QVERIFY(QFile::link(victim.path(), rig.root()));
        QVERIFY(!rig.open());
        QCOMPARE(rig.status, Status::RootUnsafe);
        QVERIFY(entriesOf(victim.path()).isEmpty()); // nothing was created through the link
    }
    void rootOwnedBySomeoneElseIsRefused()
    {
        Rig rig;
        rig.config.effectiveUid = [] { return ::geteuid() + 1; };
        QVERIFY(!rig.open());
        QCOMPARE(rig.status, Status::RootUnsafe);
    }
    void rootModeIsRepairedAndVerifiedOnEveryUse()
    {
        Rig rig;
        QVERIFY(QDir().mkpath(rig.root()));
        QCOMPARE(::chmod(QFile::encodeName(rig.root()).constData(), 0755), 0);
        QVERIFY(rig.open());
        QCOMPARE(modeOf(rig.root()) & 07777, mode_t(0700)); // repaired at open
        // someone loosens it later: the next use repairs it before anything is written
        QCOMPARE(::chmod(QFile::encodeName(rig.root()).constData(), 0777), 0);
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 3)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(modeOf(rig.root()) & 07777, mode_t(0700));
        QCOMPARE(::chmod(QFile::encodeName(rig.root()).constData(), 0770), 0);
        QCOMPARE(transfer->write(0, 0, QByteArray("abc")), Status::Ok);
        QCOMPARE(modeOf(rig.root()) & 07777, mode_t(0700));
    }
    void rootModeThatCannotBeFixedIsRefused()
    {
        Rig rig;
        QVERIFY(QDir().mkpath(rig.root()));
        QCOMPARE(::chmod(QFile::encodeName(rig.root()).constData(), 0755), 0);
        g_fchmod.error = EPERM;
        QVERIFY(!rig.open());
        QCOMPARE(rig.status, Status::RootUnsafe);
        QVERIFY(g_fchmod.hits > 0);
    }
    void aSymlinkedCacheDirectoryIsFine()
    {
        QTemporaryDir real;
        QTemporaryDir holder;
        const QString link = holder.filePath(u"cache"_s);
        QVERIFY(QFile::link(real.path(), link));
        Rig rig;
        rig.config.cacheDir = link;
        QVERIFY(rig.open());
        QVERIFY(QFileInfo::exists(real.filePath(u"krdp-clip"_s)));
    }
    // OPT-078 phase 3 (client 0.13.2 found the same shape on the other side): the space guard must never measure a root nothing has made yet.
    // Area::open makes `krdp-clip` first and the guard probes the open descriptor, so the order is right here. Every earlier guard test injected
    // the probe; these use the real one on a root that did not exist before the open.
    void theRealSpaceGuardWorksOnARootThatDidNotExistBeforeTheOpen()
    {
        Rig rig;
        rig.config.space = probeSpace; // the real probe...
        rig.config.reserveBytes = 1; // ...with a reserve that does not depend on how full this disk is
        QVERIFY(!QFileInfo::exists(rig.root()));
        QVERIFY(rig.open());
        QVERIFY(QFileInfo(rig.root()).isDir());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 5)}));
        QCOMPARE(transfer->create(), Status::Ok); // not SpaceUnknown
        QCOMPARE(transfer->write(0, 0, QByteArray("hello")), Status::Ok);
    }
    void aMissingCacheDirectoryAndMissingParentsAreMadePrivate()
    {
        Rig rig;
        rig.config.cacheDir = rig.cache.filePath(u"home/user/.cache"_s);
        QVERIFY(!QFileInfo::exists(rig.cache.filePath(u"home"_s)));
        QVERIFY2(rig.open(), statusName(rig.status));
        QCOMPARE(modeOf(rig.cache.filePath(u"home"_s)) & 07777, mode_t(0700));
        QCOMPARE(modeOf(rig.cache.filePath(u"home/user/.cache"_s)) & 07777, mode_t(0700));
        QCOMPARE(modeOf(rig.cache.filePath(u"home/user/.cache/krdp-clip"_s)) & 07777, mode_t(0700));
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 5)}));
        QCOMPARE(transfer->create(), Status::Ok);
    }
    void anUnwritableCacheParentIsAnIoErrorAndCreatesNothing()
    {
        if (::getuid() == 0) QSKIP("root can create anything");
        Rig rig;
        const QString locked = rig.cache.filePath(u"locked"_s);
        QVERIFY(QDir().mkpath(locked));
        QCOMPARE(::chmod(QFile::encodeName(locked).constData(), 0500), 0);
        rig.config.cacheDir = locked + u"/.cache"_s;
        const bool opened = rig.open();
        QCOMPARE(::chmod(QFile::encodeName(locked).constData(), 0700), 0); // so the temporary directory can go
        QVERIFY(!opened);
        QCOMPARE(rig.status, Status::IoError); // told apart from RootUnsafe (a link, someone else's)
        QVERIFY(entriesOf(locked).isEmpty());
    }
    void theDefaultCacheDirectoryFollowsXdgThenHomeThenThePasswdEntry()
    {
        QCOMPARE(defaultCacheDir("/xdg/cache", "/home/u"), u"/xdg/cache"_s);
        QCOMPARE(defaultCacheDir("", "/home/u"), u"/home/u/.cache"_s);
        QCOMPARE(defaultCacheDir("relative/cache", "/home/u"), u"/home/u/.cache"_s); // a relative XDG_CACHE_HOME is ignored (XDG says so)
        // a worker started by a system service may have no HOME at all: the passwd entry of this user is the last resort
        const QString fromPasswd = defaultCacheDir("", "");
        QVERIFY2(fromPasswd.isEmpty() || (fromPasswd.startsWith(u"/"_s) && fromPasswd.endsWith(u"/.cache"_s)), qPrintable(fromPasswd));
        QCOMPARE(defaultCacheDir("", "relative/home"), fromPasswd);
    }
    void aRelativeCacheDirectoryIsRefused()
    {
        Rig rig;
        rig.config.cacheDir = u"relative/cache"_s;
        QVERIFY(!rig.open());
        rig.config.cacheDir = QString();
        QVERIFY(!rig.open());
    }
    // a race: the root is renamed away and a symlink put in its place after we opened it; everything keeps going to the directory we hold
    void swappingTheRootForASymlinkAfterOpenRedirectsNothing()
    {
        Rig rig;
        QVERIFY(rig.open());
        QTemporaryDir evil;
        const QString moved = rig.cache.filePath(u"moved"_s);
        QVERIFY(QDir().rename(rig.root(), moved));
        QVERIFY(QFile::link(evil.path(), rig.root()));
        auto transfer = rig.area->begin(7, planOf({file(u"a.txt"_s, 5)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(0, 0, QByteArray("hello")), Status::Ok);
        QVERIFY(entriesOf(evil.path()).isEmpty());
        QCOMPARE(slurp(moved + u"/"_s + rig.area->instanceName() + u"/7/a.txt"_s), QByteArray("hello"));
    }

    // ---- the tree and the chunks ----
    void treeIsCreatedPrivateWithNoExecuteBit()
    {
        Rig rig;
        QVERIFY(rig.open());
        const auto plan = planOf({dir(u"tree"_s), dir(u"tree\\sub"_s), file(u"tree\\sub\\a.sh"_s, 10), file(u"empty.txt"_s, 0), dir(u"emptydir"_s), file(u"implied\\x\\y.bin"_s, 4)});
        auto transfer = rig.area->begin(1, plan);
        QCOMPARE(transfer->create(), Status::Ok);
        const QString base = rig.area->instancePath() + u"/1"_s;
        QCOMPARE(modeOf(base) & 07777, mode_t(0700));
        QVERIFY(S_ISDIR(modeOf(base + u"/tree/sub"_s)));
        QVERIFY(S_ISDIR(modeOf(base + u"/emptydir"_s)));
        QVERIFY(S_ISREG(modeOf(base + u"/empty.txt"_s))); // an empty file exists at once
        QVERIFY(!QFileInfo::exists(base + u"/tree/sub/a.sh"_s) && !QFileInfo::exists(transfer->partPath(2))); // data files appear with their first byte
        QCOMPARE(transfer->completeFiles(), (QSet<quint32>{3}));
        bool ok = false;
        verifyPrivate(base, &ok);
        QVERIFY(ok);
        QCOMPARE(transfer->write(2, 0, pattern(10)), Status::Ok);
        QCOMPARE(modeOf(base + u"/tree/sub/a.sh"_s) & 07777, mode_t(0600)); // .sh and ARCHIVE or not: never executable
        QCOMPARE(transfer->write(5, 0, pattern(4)), Status::Ok);
        QVERIFY(transfer->complete());
        verifyPrivate(base, &ok);
        QVERIFY(ok);
    }
    void aLooseUmaskDoesNotWidenModes()
    {
        const mode_t old = ::umask(0);
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({dir(u"d"_s), file(u"d\\f"_s, 3)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(1, 0, QByteArray("abc")), Status::Ok);
        ::umask(old);
        bool ok = false;
        verifyPrivate(rig.area->instancePath(), &ok);
        QVERIFY(ok);
        QCOMPARE(modeOf(rig.area->instancePath() + u"/1/d/f"_s) & 07777, mode_t(0600));
    }
    void chunksArriveInATemporaryFileAndAreRenamedWhenComplete()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(3, planOf({file(u"big.bin"_s, 300'000), file(u"other.txt"_s, 5)}));
        QCOMPARE(transfer->create(), Status::Ok);
        const QString base = rig.area->instancePath() + u"/3"_s;
        const QByteArray content = pattern(300'000);
        QCOMPARE(transfer->write(0, 0, content.left(100'000)), Status::Ok);
        QVERIFY(QFileInfo::exists(transfer->partPath(0)) && !QFileInfo::exists(base + u"/big.bin"_s)); // a reader never sees a half file as final
        QVERIFY(entriesOf(base).filter(u"big.bin"_s).isEmpty()); // ...and the transfer's own directory holds nothing of it yet
        QVERIFY(transfer->finishedTopLevelPaths().isEmpty());
        QCOMPARE(transfer->write(0, 100'000, content.mid(100'000, 100'000)), Status::Ok);
        QCOMPARE(transfer->write(0, 200'000, content.mid(200'000)), Status::Ok);
        QVERIFY(QFileInfo::exists(base + u"/big.bin"_s) && !QFileInfo::exists(transfer->partPath(0)));
        QCOMPARE(slurp(base + u"/big.bin"_s), content);
        QCOMPARE(transfer->finishedTopLevelPaths(), QStringList({base + u"/big.bin"_s}));
        QVERIFY(!transfer->complete());
        QCOMPARE(transfer->write(1, 0, QByteArray("12345")), Status::Ok);
        QVERIFY(transfer->complete());
        QCOMPARE(transfer->finishedTopLevelPaths(), QStringList({base + u"/big.bin"_s, base + u"/other.txt"_s}));
        QCOMPARE(transfer->writtenBytes(), quint64(300'005));
        QVERIFY(entriesOf(base).filter(u".part"_s).isEmpty());
        QVERIFY(entriesOf(rig.area->instancePath() + u"/3.parts"_s).isEmpty()); // every temporary file was renamed away
        QVERIFY(entriesOf(rig.area->instancePath() + u"/3.parts"_s).isEmpty()); // every temporary file was renamed away
    }
    // REVIEW-S5: the sanitizer allows names up to 255 UTF-8 bytes (NAME_MAX); staging must not need a single extra byte for its temporary name.
    void aNameOfTheLongestAllowedLengthIsStaged_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("251 bytes") << QString(251, QLatin1Char('a'));
        QTest::newRow("254 bytes") << QString(254, QLatin1Char('b'));
        QTest::newRow("255 bytes") << QString(255, QLatin1Char('c'));
        QTest::newRow("255 bytes of multi-byte characters") << (QString(127, QChar(0x00e9)) + QLatin1Char('x')); // 127 x 2 + 1 = 255 bytes
        QTest::newRow("255 bytes ending in .part") << (QString(250, QLatin1Char('d')) + u".part"_s);
    }
    void aNameOfTheLongestAllowedLengthIsStaged()
    {
        QFETCH(QString, name);
        Rig rig;
        QVERIFY(rig.open());
        const auto plan = planOf({dir(u"sub"_s), file(u"sub\\"_s + name, 6)});
        QVERIFY2(plan.ok(), "the planner accepts the name");
        QCOMPARE(plan.entries.last().components.last().toUtf8(), name.toUtf8()); // unchanged by the sanitizer
        auto transfer = rig.area->begin(1, plan);
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(1, 0, QByteArray("abcdef")), Status::Ok);
        QVERIFY(transfer->complete());
        QCOMPARE(slurp(rig.area->instancePath() + u"/1/sub/"_s + name), QByteArray("abcdef"));
        QVERIFY(entriesOf(rig.area->instancePath() + u"/1.parts"_s).isEmpty()); // the temporary file was renamed away
    }
    // REVIEW-S5: `X` beside `X.part` (the old temporary name of X): both arrive, neither is mistaken for the other.
    void aFileBesideItsOwnOldTemporaryNameIsStaged()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"X"_s, 5), file(u"X.part"_s, 3), file(u"Y.part"_s, 2), file(u"Y"_s, 4)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(0, 0, QByteArray("12345")), Status::Ok);
        QCOMPARE(transfer->write(1, 0, QByteArray("abc")), Status::Ok);
        QCOMPARE(transfer->write(2, 0, QByteArray("yy")), Status::Ok);
        QCOMPARE(transfer->write(3, 0, QByteArray("wxyz")), Status::Ok);
        QVERIFY(transfer->complete());
        const QString base = rig.area->instancePath() + u"/1/"_s;
        QCOMPARE(slurp(base + u"X"_s), QByteArray("12345"));
        QCOMPARE(slurp(base + u"X.part"_s), QByteArray("abc"));
        QCOMPARE(slurp(base + u"Y.part"_s), QByteArray("yy"));
        QCOMPARE(slurp(base + u"Y"_s), QByteArray("wxyz"));
    }
    // The same with an EMPTY `X.part`: it is created with the tree, before X's data arrives.
    void anEmptyFileBesideItsOwnOldTemporaryNameIsStaged()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"X.part"_s, 0), file(u"X"_s, 5)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(1, 0, QByteArray("12345")), Status::Ok);
        QVERIFY(transfer->complete());
        const QString base = rig.area->instancePath() + u"/1/"_s;
        QCOMPARE(slurp(base + u"X"_s), QByteArray("12345"));
        QVERIFY(QFileInfo(base + u"X.part"_s).isFile());
        QCOMPARE(QFileInfo(base + u"X.part"_s).size(), qint64(0));
    }
    // REVIEW-S5: whatever the temporary names are, no name a client can send may equal one (nor may a client file stand in the way of one).
    void aClientFileNamedLikeAnyTemporaryNameIsStaged()
    {
        Rig rig;
        QVERIFY(rig.open());
        const QStringList names = {u"0"_s, u"1"_s, u".krdp-part-0"_s, u".krdp-part-1"_s, u"1.parts"_s, u".parts"_s, u"a.part"_s};
        QList<Descriptor> items;
        for (const QString &name : names) items.append(file(name, 3));
        auto transfer = rig.area->begin(1, planOf(items));
        QCOMPARE(transfer->create(), Status::Ok);
        for (quint32 i = 0; i < quint32(names.size()); ++i) QCOMPARE(transfer->write(i, 0, QByteArray("xyz")), Status::Ok);
        QVERIFY(transfer->complete());
        for (const QString &name : names) QCOMPARE(slurp(rig.area->instancePath() + u"/1/"_s + name), QByteArray("xyz"));
    }
    void theDescriptorsModifiedTimeIsApplied()
    {
        Rig rig;
        QVERIFY(rig.open());
        const auto stamp = ClipboardFileDescriptor::fileTimeFromUnixSeconds(1'600'000'000);
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 2, stamp), file(u"e"_s, 0, stamp)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(0, 0, QByteArray("ab")), Status::Ok);
        const QString base = rig.area->instancePath() + u"/1/"_s;
        QCOMPARE(QFileInfo(base + u"a"_s).lastModified().toSecsSinceEpoch(), qint64(1'600'000'000));
        QCOMPARE(QFileInfo(base + u"e"_s).lastModified().toSecsSinceEpoch(), qint64(1'600'000'000));
    }
    void aSecondTransferWithTheSameSeqIsRefused()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto first = rig.area->begin(1, planOf({file(u"a"_s, 2)}));
        QCOMPARE(first->create(), Status::Ok);
        auto second = rig.area->begin(1, planOf({file(u"b"_s, 2)}));
        QCOMPARE(second->create(), Status::Exists);
        QCOMPARE(first->write(0, 0, QByteArray("ab")), Status::Ok); // the first is untouched
        QVERIFY(QFileInfo::exists(rig.area->instancePath() + u"/1/a"_s));
    }

    // ---- never overwrite, never follow (spec 4.2) ----
    void aPartFileThatAlreadyExistsIsReportedNotOverwritten()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a.txt"_s, 4)}));
        QCOMPARE(transfer->create(), Status::Ok);
        const QString part = transfer->partPath(0);
        touch(part, "precious"); // (this makes the temporary directory too)
        QFile held(part); // an open handle on the squatter: it stays readable after the abort removes the name
        QVERIFY(held.open(QIODevice::ReadOnly));
        QCOMPARE(transfer->write(0, 0, QByteArray("new!")), Status::Exists);
        QCOMPARE(held.readAll(), QByteArray("precious")); // not truncated, not written
        QVERIFY(transfer->aborted());
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s));
    }
    void aSymlinkPlantedAsThePartFileIsNotFollowed()
    {
        Rig rig;
        QVERIFY(rig.open());
        QTemporaryDir outside;
        const QString victim = outside.filePath(u"victim"_s);
        touch(victim, "original");
        auto transfer = rig.area->begin(1, planOf({file(u"a.txt"_s, 4)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QVERIFY(QDir().mkpath(rig.area->instancePath() + u"/1.parts"_s));
        QVERIFY(QFile::link(victim, transfer->partPath(0)));
        QVERIFY(transfer->write(0, 0, QByteArray("evil")) != Status::Ok);
        QCOMPARE(slurp(victim), QByteArray("original"));
    }
    // REVIEW-S5: the temporary directory is made by us below the instance; a link planted in its place is never followed.
    void aSymlinkPlantedAsTheTemporaryDirectoryIsNotFollowed()
    {
        Rig rig;
        QVERIFY(rig.open());
        QTemporaryDir outside;
        auto transfer = rig.area->begin(1, planOf({file(u"a.txt"_s, 4)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QVERIFY(QFile::link(outside.path(), rig.area->instancePath() + u"/1.parts"_s));
        QVERIFY(transfer->write(0, 0, QByteArray("evil")) != Status::Ok);
        QVERIFY(QDir(outside.path()).isEmpty()); // nothing was written through the link
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s)); // the failed transfer took its tree away
    }
    void aHardLinkPlantedAsThePartFileIsNotWrittenThrough()
    {
        Rig rig;
        QVERIFY(rig.open());
        QTemporaryDir outside;
        const QString victim = outside.filePath(u"victim"_s);
        touch(victim, "original");
        auto transfer = rig.area->begin(1, planOf({file(u"a.txt"_s, 4)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QVERIFY(QDir().mkpath(rig.area->instancePath() + u"/1.parts"_s));
        QCOMPARE(::link(QFile::encodeName(victim).constData(), QFile::encodeName(transfer->partPath(0)).constData()), 0);
        QCOMPARE(transfer->write(0, 0, QByteArray("evil")), Status::Exists);
        QCOMPARE(slurp(victim), QByteArray("original"));
        // and removing the staging unlinks the extra name, not the file
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(victim).constData(), &st), 0);
        QCOMPARE(qint64(st.st_nlink), qint64(1));
    }
    void aFinalNameThatAppearedMeanwhileIsNeverReplaced()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a.txt"_s, 8)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(0, 0, QByteArray("1234")), Status::Ok);
        const QString finalName = rig.area->instancePath() + u"/1/a.txt"_s;
        touch(finalName, "squatter");
        QFile held(finalName);
        QVERIFY(held.open(QIODevice::ReadOnly));
        QCOMPARE(transfer->write(0, 4, QByteArray("5678")), Status::Exists);
        QCOMPARE(held.readAll(), QByteArray("squatter")); // RENAME_NOREPLACE: the squatter was never replaced
        QVERIFY(!QFileInfo::exists(finalName)); // the abort then removed the whole staging, the squatter included
    }
    void aDirectorySwappedForASymlinkIsNotFollowed()
    {
        Rig rig;
        QVERIFY(rig.open());
        QTemporaryDir outside;
        auto transfer = rig.area->begin(1, planOf({dir(u"d"_s), file(u"d\\f.txt"_s, 4)}));
        QCOMPARE(transfer->create(), Status::Ok);
        const QString d = rig.area->instancePath() + u"/1/d"_s;
        QVERIFY(QDir().rmdir(d));
        QVERIFY(QFile::link(outside.path(), d));
        const auto status = transfer->write(1, 0, QByteArray("evil"));
        QVERIFY(status == Status::Hostile || status == Status::IoError);
        QVERIFY(entriesOf(outside.path()).isEmpty());
        QVERIFY(transfer->aborted());
    }

    // ---- hostile chunks (spec 4.3) ----
    void chunksOutOfOrderAbortAndRemoveTheStaging()
    {
        Rig rig;
        QVERIFY(rig.open());
        const auto plan = planOf({file(u"a"_s, 20), file(u"b"_s, 20)});
        const QString base = rig.area->instancePath() + u"/1"_s;
        {
            auto transfer = rig.area->begin(1, plan);
            QCOMPARE(transfer->create(), Status::Ok);
            QCOMPARE(transfer->write(0, 5, pattern(5)), Status::OutOfOrder); // not from the start
            QVERIFY(!QFileInfo::exists(base));
        }
        {
            auto transfer = rig.area->begin(1, plan);
            QCOMPARE(transfer->create(), Status::Ok);
            QCOMPARE(transfer->write(1, 0, pattern(5)), Status::OutOfOrder); // the second file first
            QVERIFY(!QFileInfo::exists(base));
        }
        {
            auto transfer = rig.area->begin(1, plan);
            QCOMPARE(transfer->create(), Status::Ok);
            QCOMPARE(transfer->write(0, 0, pattern(10)), Status::Ok);
            QCOMPARE(transfer->write(0, 0, pattern(10)), Status::OutOfOrder); // the same range again
            QVERIFY(!QFileInfo::exists(base));
        }
    }
    void aPeerThatSendsMoreThanTheDeclaredSizeIsCutOffAndTheFileRemoved()
    {
        Rig rig;
        QVERIFY(rig.open());
        const QString base = rig.area->instancePath() + u"/1"_s;
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 100)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(0, 0, pattern(60)), Status::Ok);
        QCOMPARE(transfer->write(0, 60, pattern(41)), Status::Hostile); // 101 bytes in all
        QVERIFY(transfer->aborted());
        QVERIFY(!QFileInfo::exists(base)); // the partial is gone
        QCOMPARE(transfer->write(0, 60, pattern(40)), Status::Gone); // and it stays down
    }
    void chunksForNothingAreHostile()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 3)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(0, 0, QByteArray()), Status::Hostile); // an empty chunk
        auto done = rig.area->begin(2, planOf({file(u"a"_s, 3)}));
        QCOMPARE(done->create(), Status::Ok);
        QCOMPARE(done->write(0, 0, QByteArray("abc")), Status::Ok);
        QVERIFY(done->complete());
        QCOMPARE(done->write(0, 3, QByteArray("x")), Status::Hostile); // nothing more is expected
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/2"_s));
        auto unknownIndex = rig.area->begin(3, planOf({file(u"a"_s, 3)}));
        QCOMPARE(unknownIndex->create(), Status::Ok);
        QCOMPARE(unknownIndex->write(99, 0, QByteArray("abc")), Status::OutOfOrder);
    }
    void directoriesAndEmptyFilesNeedNoData()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({dir(u"d"_s), file(u"e"_s, 0)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QVERIFY(transfer->complete());
        QCOMPARE(transfer->finishedTopLevelPaths().size(), 2);
        QCOMPARE(transfer->write(1, 0, QByteArray("x")), Status::Hostile);
    }

    // ---- removal ----
    void replacedAndEndedTransfersLeaveNothing()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto a = rig.area->begin(1, planOf({dir(u"t"_s), file(u"t\\a"_s, 3)}));
        auto b = rig.area->begin(2, planOf({file(u"b"_s, 3)}));
        QCOMPARE(a->create(), Status::Ok);
        QCOMPARE(b->create(), Status::Ok);
        QCOMPARE(a->write(1, 0, QByteArray("abc")), Status::Ok);
        QCOMPARE(b->write(0, 0, QByteArray("x")), Status::Ok); // half a file: its temporary file exists
        QVERIFY(QFileInfo::exists(b->partPath(0)));
        QVERIFY(QFileInfo::exists(rig.area->instancePath() + u"/1.parts"_s));
        QVERIFY(rig.area->removeTransfer(1)); // a newer copy replaced the offer
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s));
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1.parts"_s));
        QVERIFY(QFileInfo::exists(rig.area->instancePath() + u"/2"_s));
        QVERIFY(rig.area->removeAll()); // the connection ended
        QCOMPARE(entriesOf(rig.area->instancePath()), QStringList({u".lock"_s}));
        QVERIFY(rig.area->removeTransfer(99)); // unknown: nothing to do, not an error
    }
    void abortMidFileRemovesThePartFile()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 100)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(0, 0, pattern(50)), Status::Ok);
        QVERIFY(transfer->abort());
        QVERIFY(transfer->abort()); // idempotent
        QCOMPARE(entriesOf(rig.area->instancePath()), QStringList({u".lock"_s}));
    }
    void removalNeverFollowsOrDamagesWhatALinkPointsAt()
    {
        Rig rig;
        QVERIFY(rig.open());
        QTemporaryDir outside;
        touch(outside.filePath(u"keep/inner.txt"_s), "keep");
        touch(outside.filePath(u"victim"_s), "victim");
        auto transfer = rig.area->begin(1, planOf({dir(u"d"_s), file(u"d\\a"_s, 1)}));
        QCOMPARE(transfer->create(), Status::Ok);
        const QString inside = rig.area->instancePath() + u"/1/d"_s;
        QVERIFY(QFile::link(outside.filePath(u"keep"_s), inside + u"/dirlink"_s)); // a symlink to a directory
        QVERIFY(QFile::link(outside.filePath(u"victim"_s), inside + u"/filelink"_s));
        QCOMPARE(::link(QFile::encodeName(outside.filePath(u"victim"_s)).constData(), QFile::encodeName(inside + u"/hardlink"_s).constData()), 0);
        QVERIFY(transfer->remove());
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s));
        QCOMPARE(slurp(outside.filePath(u"keep/inner.txt"_s)), QByteArray("keep"));
        QCOMPARE(slurp(outside.filePath(u"victim"_s)), QByteArray("victim"));
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(outside.filePath(u"victim"_s)).constData(), &st), 0);
        QCOMPARE(qint64(st.st_nlink), qint64(1));
    }
    void aDeepTreeIsRemovedAndTheDepthIsBounded()
    {
        Rig rig;
        QVERIFY(rig.open());
        QString path = rig.area->instancePath() + u"/1"_s;
        QDir().mkpath(path);
        for (int i = 0; i < 100; ++i) path += u"/d"_s;
        QVERIFY(QDir().mkpath(path));
        QVERIFY(rig.area->removeTransfer(1));
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s));
        // deeper than the bound: refused rather than recursed without limit
        path = rig.area->instancePath() + u"/2"_s;
        QDir().mkpath(path);
        for (int i = 0; i < 200; ++i) {
            path += u"/e"_s;
            if (!QDir().mkpath(path)) break; // PATH_MAX
        }
        QVERIFY(!rig.area->removeTransfer(2)); // beyond the bound: reported, not recursed into without limit (the temp dir cleanup removes the rest)
    }
    void unlinkFailingDuringAbortIsReportedNotFatal()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 100)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(0, 0, pattern(50)), Status::Ok);
        g_unlinkat.error = EPERM;
        QVERIFY(!transfer->abort());
        QVERIFY(g_unlinkat.hits > 0);
        g_unlinkat = Fault();
        QVERIFY(rig.area->removeAll()); // and a later sweep of our own succeeds
    }
    void transfersOlderThanADayAreRemovedByTheirOwner()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto old = rig.area->begin(1, planOf({file(u"a"_s, 2)}));
        auto young = rig.area->begin(2, planOf({file(u"a"_s, 1)}));
        QCOMPARE(old->create(), Status::Ok);
        QCOMPARE(young->create(), Status::Ok);
        QCOMPARE(old->write(0, 0, QByteArray("x")), Status::Ok); // half a file: its temporary directory is younger than the transfer's
        QCOMPARE(young->write(0, 0, QByteArray("y")), Status::Ok);
        ageBy(rig.area->instancePath() + u"/1"_s, ClipboardFileStaging::MaxAgeSeconds + 60);
        QCOMPARE(rig.area->removeOlderThan(time(nullptr)), 1); // a transfer is counted once, with its temporary directory
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s));
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1.parts"_s));
        QVERIFY(QFileInfo::exists(rig.area->instancePath() + u"/2"_s));
        QVERIFY(QFileInfo::exists(rig.area->instancePath() + u"/2.parts"_s));
        // a temporary directory whose transfer is long gone goes on its own age
        QVERIFY(QDir().mkpath(rig.area->instancePath() + u"/7.parts"_s));
        ageBy(rig.area->instancePath() + u"/7.parts"_s, ClipboardFileStaging::MaxAgeSeconds + 60);
        QCOMPARE(rig.area->removeOlderThan(time(nullptr)), 0);
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/7.parts"_s));
    }

    // ---- the disk guard (spec 3.2) ----
    void theGuardRefusesBeforeTheFirstByteAndSaysWhy()
    {
        Rig rig;
        rig.config.space = [](int) { return std::optional(SpaceInfo{3ULL << 30, 100ULL << 30}); };
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 2ULL << 30)}));
        SpaceFailure failure;
        QCOMPARE(transfer->create(&failure), Status::NoSpace); // 2 GiB + a 5 GiB reserve (5% of 100 GiB) > 3 GiB
        QCOMPARE(failure.freeBytes, quint64(3ULL << 30));
        QCOMPARE(failure.neededBytes, quint64(7ULL << 30));
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s)); // nothing was created
        QCOMPARE(transfer->write(0, 0, QByteArray("x")), Status::Gone);
    }
    void theGuardDecisionTable_data()
    {
        QTest::addColumn<quint64>("free");
        QTest::addColumn<quint64>("total");
        QTest::addColumn<quint64>("need");
        QTest::addColumn<quint64>("configured");
        QTest::addColumn<int>("expected");
        constexpr quint64 G = 1ULL << 30;
        const int ok = int(Status::Ok), no = int(Status::NoSpace), unknown = int(Status::SpaceUnknown);
        // default reserve: 5% clamped to [2 GiB, 8 GiB] -> on 100 GiB that is 5 GiB; on 10 GiB it is the 2 GiB floor; on a 3.6 TiB disk the 8 GiB ceiling
        QTest::newRow("fits exactly (100G fs)") << (15 * G) << (100 * G) << (10 * G) << quint64(0) << ok;
        QTest::newRow("one byte over (100G fs)") << (15 * G - 1) << (100 * G) << (10 * G) << quint64(0) << no;
        QTest::newRow("floor applies on a small fs") << (12 * G) << (20 * G) << (10 * G) << quint64(0) << ok; // 5% of 20 GiB is 1 GiB: the 2 GiB floor wins, 10 + 2 <= 12
        QTest::newRow("floor, one byte over") << (12 * G - 1) << (20 * G) << (10 * G) << quint64(0) << no;
        QTest::newRow("big fs: the 8 GiB ceiling (was 183 GiB)") << (9 * G) << (3666 * G) << (1 * G) << quint64(0) << ok;
        QTest::newRow("big fs: the ceiling, one byte over") << (9 * G - 1) << (3666 * G) << (1 * G) << quint64(0) << no;
        QTest::newRow("configured reserve replaces the rule") << (11 * G) << (100 * G) << (10 * G) << (1 * G) << ok;
        QTest::newRow("configured reserve, one byte over") << (11 * G - 1) << (100 * G) << (10 * G) << (1 * G) << no;
        QTest::newRow("zero need still keeps the reserve") << (5 * G) << (100 * G) << quint64(0) << quint64(0) << ok;
        QTest::newRow("zero need, below the reserve") << (5 * G - 1) << (100 * G) << quint64(0) << quint64(0) << no;
        QTest::newRow("huge need saturates") << (50 * G) << (100 * G) << ~quint64(0) << quint64(0) << no;
        QTest::newRow("nothing usable") << quint64(0) << quint64(0) << quint64(1) << quint64(0) << unknown;
    }
    void theGuardDecisionTable()
    {
        QFETCH(quint64, free);
        QFETCH(quint64, total);
        QFETCH(quint64, need);
        QFETCH(quint64, configured);
        QFETCH(int, expected);
        Rig rig;
        rig.config.reserveBytes = configured;
        rig.config.space = [=](int) { return total == 0 ? std::optional<SpaceInfo>() : std::optional(SpaceInfo{free, total}); };
        QVERIFY(rig.open());
        // one declared file of size \a need (the plan sums sizes; a descriptor can say 2^64 - 1)
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, need)}));
        const Status status = transfer->create();
        QCOMPARE(int(status), expected);
        QCOMPARE(QFileInfo::exists(rig.area->instancePath() + u"/1"_s), expected == int(Status::Ok));
    }
    void aFailingProbeRefusesInsteadOfGuessing()
    {
        Rig rig;
        rig.config.space = [](int) { return std::optional<SpaceInfo>(); };
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 5)}));
        QCOMPARE(transfer->create(), Status::SpaceUnknown);
        // the real probe through a failing fstatvfs
        Rig real;
        real.config.space = probeSpace;
        QVERIFY(real.open());
        g_fstatvfs.error = EIO;
        auto second = real.area->begin(1, planOf({file(u"a"_s, 5)}));
        QCOMPARE(second->create(), Status::SpaceUnknown);
        QVERIFY(g_fstatvfs.hits > 0);
    }
    void theRealProbeSeesTheRealFilesystem()
    {
        QTemporaryDir dir;
        const int fd = ::open(QFile::encodeName(dir.path()).constData(), O_RDONLY | O_DIRECTORY);
        QVERIFY(fd >= 0);
        const auto info = probeSpace(fd);
        ::close(fd);
        QVERIFY(info.has_value());
        QVERIFY(info->totalBytes > 0 && info->freeBytes <= info->totalBytes);
    }
    void theGuardRechecksEveryIntervalAndAbortsMidWay()
    {
        Rig rig;
        rig.config.spaceCheckIntervalBytes = 1000;
        int probes = 0;
        quint64 freeNow = 100ULL << 30;
        rig.config.reserveBytes = 1; // the reserve must not hide the arithmetic
        rig.config.space = [&](int) {
            ++probes;
            return std::optional(SpaceInfo{freeNow, 200ULL << 30});
        };
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 10'000)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(probes, 1); // before the first byte
        QCOMPARE(transfer->write(0, 0, pattern(600)), Status::Ok);
        QCOMPARE(probes, 1); // not due yet
        QCOMPARE(transfer->write(0, 600, pattern(600)), Status::Ok); // 1200 staged: due before the NEXT chunk
        QCOMPARE(probes, 1);
        freeNow = 5000; // other writers filled the disk: 8800 bytes still to come + 1 reserve do not fit
        SpaceFailure failure;
        QCOMPARE(transfer->write(0, 1200, pattern(600), &failure), Status::NoSpace);
        QCOMPARE(probes, 2);
        QCOMPARE(failure.freeBytes, quint64(5000));
        QCOMPARE(failure.neededBytes, quint64(10'000 - 1200 + 1));
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s)); // aborted: the partial is deleted
        QVERIFY(transfer->aborted());
    }
    void theGuardChecksEvery64MiBByDefault()
    {
        Rig rig;
        int probes = 0;
        rig.config.reserveBytes = 1;
        rig.config.space = [&](int) {
            ++probes;
            return std::optional(SpaceInfo{1ULL << 40, 1ULL << 41});
        };
        QVERIFY(rig.open());
        const quint64 size = 70ULL * 1024 * 1024;
        auto transfer = rig.area->begin(1, planOf({file(u"big"_s, size)}));
        QCOMPARE(transfer->create(), Status::Ok);
        const QByteArray chunk(1024 * 1024, 'z');
        for (quint64 offset = 0; offset < size; offset += chunk.size()) QCOMPARE(transfer->write(0, offset, chunk.left(int(std::min<quint64>(chunk.size(), size - offset)))), Status::Ok);
        QCOMPARE(probes, 2); // once up front, once after the first 64 MiB
        QVERIFY(transfer->complete());
    }

    // ---- failures injected with link wraps (spec 9) ----
    void anFullDiskDuringAWriteIsNoSpaceAndTheStagingIsGone()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 100)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(0, 0, pattern(10)), Status::Ok);
        g_pwrite.error = ENOSPC;
        QCOMPARE(transfer->write(0, 10, pattern(10)), Status::NoSpace);
        g_pwrite = Fault();
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s));
        QCOMPARE(int(ackFor(Status::NoSpace)), int(ClipboardFileFetch::AckStatus::NoSpace));
    }
    void quotaAndIoErrorsAreTold_data()
    {
        QTest::addColumn<int>("error");
        QTest::addColumn<int>("expected");
        QTest::newRow("quota") << EDQUOT << int(Status::NoSpace);
        QTest::newRow("io") << EIO << int(Status::IoError);
        QTest::newRow("read-only fs") << EROFS << int(Status::IoError);
    }
    void quotaAndIoErrorsAreTold()
    {
        QFETCH(int, error);
        QFETCH(int, expected);
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 100)}));
        QCOMPARE(transfer->create(), Status::Ok);
        g_pwrite.error = error;
        QCOMPARE(int(transfer->write(0, 0, pattern(10))), expected);
        g_pwrite = Fault();
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s));
    }
    void shortWritesAreCompletedNotTruncated()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 1000)}));
        QCOMPARE(transfer->create(), Status::Ok);
        g_pwrite.shortWrite = 7; // the kernel takes 7 bytes at a time
        QCOMPARE(transfer->write(0, 0, pattern(1000)), Status::Ok);
        g_pwrite = Fault();
        QCOMPARE(slurp(rig.area->instancePath() + u"/1/a"_s), pattern(1000));
    }
    void anInterruptedWriteIsRetried()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 10)}));
        QCOMPARE(transfer->create(), Status::Ok);
        g_pwrite.error = EINTR;
        g_pwrite.times = 3; // three interruptions, then the write goes through
        QCOMPARE(transfer->write(0, 0, pattern(10)), Status::Ok);
        QCOMPARE(g_pwrite.hits, 3);
        QCOMPARE(slurp(rig.area->instancePath() + u"/1/a"_s), pattern(10));
    }
    void aFailingFsyncKeepsTheFileOutOfItsFinalName()
    {
        Rig rig;
        rig.config.syncFiles = true; // the per-file fsync is a tuning, off by default
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 4)}));
        QCOMPARE(transfer->create(), Status::Ok);
        g_fsync.error = EIO;
        QCOMPARE(transfer->write(0, 0, pattern(4)), Status::IoError);
        g_fsync = Fault();
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s));
        auto late = rig.area->begin(2, planOf({file(u"a"_s, 4)}));
        QCOMPARE(late->create(), Status::Ok);
        g_fsync.error = ENOSPC; // delayed allocation reporting a full disk at fsync
        QCOMPARE(late->write(0, 0, pattern(4)), Status::NoSpace);
    }
    // REVIEW-S5: by default a file is not fsync'd (measured: many small files were bound by it); a full disk is still told by the write.
    void byDefaultNoFsyncIsMadeAndAFullDiskIsStillTold()
    {
        Rig rig;
        QVERIFY(!rig.config.syncFiles);
        QVERIFY(rig.open());
        g_fsync.error = EIO; // would fail every fsync
        auto transfer = rig.area->begin(1, planOf({file(u"a"_s, 4), file(u"b"_s, 4)}));
        QCOMPARE(transfer->create(), Status::Ok);
        QCOMPARE(transfer->write(0, 0, pattern(4)), Status::Ok);
        QCOMPARE(g_fsync.hits, 0); // not even called
        QVERIFY(QFileInfo::exists(rig.area->instancePath() + u"/1/a"_s)); // complete: final name
        g_pwrite.error = ENOSPC;
        QCOMPARE(transfer->write(1, 0, pattern(4)), Status::NoSpace);
        QVERIFY(transfer->aborted());
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/1"_s));
    }
    void aFailingOpenOrRenameIsAnIoErrorNotAFinalFile()
    {
        Rig rig;
        QVERIFY(rig.open());
        auto transfer = rig.area->begin(1, planOf({file(u"a.txt"_s, 4)}));
        QCOMPARE(transfer->create(), Status::Ok);
        g_openat.error = EACCES;
        g_openat.match = "0"; // the temporary file is named by its plan index
        QCOMPARE(transfer->write(0, 0, pattern(4)), Status::IoError);
        QVERIFY(g_openat.hits > 0);
        g_openat = Fault();
        auto second = rig.area->begin(2, planOf({file(u"a.txt"_s, 4)}));
        QCOMPARE(second->create(), Status::Ok);
        g_renameat2.error = EIO;
        QCOMPARE(second->write(0, 0, pattern(4)), Status::IoError);
        g_renameat2 = Fault();
        QVERIFY(!QFileInfo::exists(rig.area->instancePath() + u"/2"_s)); // nothing under the final name, nothing left behind
    }

    // ---- the janitor, over an injected fs ----
    void sweepDecisionsWithAnInjectedFs()
    {
        using ClipboardFileStaging::RootEntry;
        constexpr qint64 Now = 2'000'000'000;
        QStringList removed, unlinked;
        QSet<QString> lockedNames{u"c-11fe"_s, u"c-11fe01d"_s};
        SweepOps ops;
        ops.list = [] {
            return QList<RootEntry>{
                {u"c-11fe"_s, Now - 3600, true, false}, // locked, young
                {u"c-11fe01d"_s, Now - 5 * ClipboardFileStaging::MaxAgeSeconds, true, false}, // locked, ancient: its owner is alive, kept
                {u"c-dead"_s, Now - 3600, true, false}, // nobody holds it
                {u"c-dead01d"_s, Now - 5 * ClipboardFileStaging::MaxAgeSeconds, true, false},
                {u"c-57a7"_s, Now - 2, true, false}, // created 2 s ago, no lock yet: a worker is starting
                {u"c-a1"_s, Now, false, true}, // a symlink named like ours
                {u"c-f1e"_s, Now, false, false}, // a regular file named like ours
                {u"notes"_s, Now - 9 * ClipboardFileStaging::MaxAgeSeconds, true, false}, // not ours
                {u"other-link"_s, Now, false, true},
            };
        };
        ops.locked = [&](const QString &name) { return lockedNames.contains(name); };
        ops.removeTree = [&](const QString &name) {
            removed.append(name);
            return true;
        };
        ops.unlinkOnly = [&](const QString &name) {
            unlinked.append(name);
            return true;
        };
        const auto report = runSweep(ops, Now);
        QCOMPARE(removed, QStringList({u"c-dead"_s, u"c-dead01d"_s}));
        QCOMPARE(unlinked, QStringList({u"c-a1"_s, u"c-f1e"_s}));
        QCOMPARE(report, (SweepReport{2, 2, 3, 2, 0}));
        // a removal that fails is counted, not hidden
        ops.removeTree = [](const QString &) { return false; };
        QCOMPARE(runSweep(ops, Now).failed, 2);
    }
    void sweepWithAnEmptyOrMissingFsDoesNothing()
    {
        QCOMPARE(runSweep(SweepOps{}, 1000), SweepReport{});
    }

    // ---- the janitor, over a real directory ----
    void startupSweepRemovesCrashLeftoversAndKeepsLiveInstances()
    {
        Rig rig;
        QVERIFY(rig.open());
        QTemporaryDir outside;
        touch(outside.filePath(u"victim"_s), "victim");
        const QString root = rig.root();
        // a crashed worker's instance: a tree with a part file, a symlink out and a hard link out; no process holds its lock
        QDir().mkpath(root + u"/c-0000000000000dead/5/sub"_s);
        touch(root + u"/c-0000000000000dead/.lock"_s);
        touch(root + u"/c-0000000000000dead/5/sub/f.part"_s, "half");
        touch(root + u"/c-0000000000000dead/5.parts/0"_s, "half of the temporary file"); // the temporary directory of the same transfer
        QVERIFY(QFile::link(outside.path(), root + u"/c-0000000000000dead/5/escape"_s));
        QCOMPARE(::link(QFile::encodeName(outside.filePath(u"victim"_s)).constData(), QFile::encodeName(root + u"/c-0000000000000dead/5/hard"_s).constData()), 0);
        ageBy(root + u"/c-0000000000000dead"_s, 3600);
        // another live worker (of the same user) holding its lock
        QDir().mkpath(root + u"/c-0000000000000a11e/9"_s);
        touch(root + u"/c-0000000000000a11e/.lock"_s);
        touch(root + u"/c-0000000000000a11e/9/keep.txt"_s, "mine");
        const int liveLock = ::open(QFile::encodeName(root + u"/c-0000000000000a11e/.lock"_s).constData(), O_RDWR);
        QVERIFY(liveLock >= 0);
        QCOMPARE(::flock(liveLock, LOCK_EX | LOCK_NB), 0);
        ageBy(root + u"/c-0000000000000a11e"_s, 5 * ClipboardFileStaging::MaxAgeSeconds); // ancient, but alive
        // an instance with no lock file and an old mtime (an interrupted creation long ago), a young one (still being created)
        QDir().mkpath(root + u"/c-00000000000000001"_s);
        ageBy(root + u"/c-00000000000000001"_s, 3600);
        QDir().mkpath(root + u"/c-00000000000000002"_s);
        // things that are not ours, and symlink / file impostors
        QDir().mkpath(root + u"/notes"_s);
        touch(root + u"/README"_s, "hi");
        QVERIFY(QFile::link(outside.path(), root + u"/c-0000000000000bad"_s));
        touch(root + u"/c-0000000000000f11e"_s, "x");
        // our own instance: a transfer in flight
        auto own = rig.area->begin(1, planOf({file(u"a"_s, 5)}));
        QCOMPARE(own->create(), Status::Ok);
        ageBy(rig.area->instancePath(), 5 * ClipboardFileStaging::MaxAgeSeconds); // even a stale-looking own directory stays

        const auto report = rig.area->sweep(time(nullptr));

        QVERIFY(!QFileInfo::exists(root + u"/c-0000000000000dead"_s));
        QVERIFY(QFileInfo::exists(root + u"/c-0000000000000a11e/9/keep.txt"_s));
        QVERIFY(!QFileInfo::exists(root + u"/c-00000000000000001"_s));
        QVERIFY(QFileInfo::exists(root + u"/c-00000000000000002"_s)); // young: left for its creator
        QVERIFY(QFileInfo::exists(root + u"/notes"_s) && QFileInfo::exists(root + u"/README"_s));
        QVERIFY(!QFileInfo(root + u"/c-0000000000000bad"_s).isSymLink() && !QFileInfo::exists(root + u"/c-0000000000000bad"_s));
        QVERIFY(!QFileInfo::exists(root + u"/c-0000000000000f11e"_s));
        QVERIFY(QFileInfo::exists(rig.area->instancePath() + u"/1"_s));
        QCOMPARE(report.removedTrees, 2);
        QCOMPARE(report.unlinked, 2);
        QCOMPARE(report.failed, 0);
        // nothing outside was touched
        QCOMPARE(slurp(outside.filePath(u"victim"_s)), QByteArray("victim"));
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(outside.filePath(u"victim"_s)).constData(), &st), 0);
        QCOMPARE(qint64(st.st_nlink), qint64(1));
        QVERIFY(QFileInfo(outside.path()).isDir());
        ::close(liveLock);
    }
    void aLockDroppedByACrashMakesTheInstanceStale()
    {
        // the "worker" is a child process that takes the lock and is killed with SIGKILL; its directory is then swept
        Rig rig;
        QVERIFY(rig.open());
        const QString root = rig.root();
        QDir().mkpath(root + u"/c-0000000000000c0de/3"_s);
        touch(root + u"/c-0000000000000c0de/.lock"_s);
        touch(root + u"/c-0000000000000c0de/3/a.part"_s, "x");
        ageBy(root + u"/c-0000000000000c0de"_s, 600);
        const pid_t child = ::fork();
        if (child == 0) {
            const int fd = ::open(QFile::encodeName(root + u"/c-0000000000000c0de/.lock"_s).constData(), O_RDWR);
            if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) ::_exit(2);
            ::pause();
            ::_exit(0);
        }
        QVERIFY(child > 0);
        QTest::qWait(300); // the child took the lock (a fork of a Qt test: only syscalls above)
        auto report = rig.area->sweep(time(nullptr));
        QVERIFY2(QFileInfo::exists(root + u"/c-0000000000000c0de/3/a.part"_s), "a live holder keeps its instance");
        QCOMPARE(report.removedTrees, 0);
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        report = rig.area->sweep(time(nullptr));
        QVERIFY(!QFileInfo::exists(root + u"/c-0000000000000c0de"_s));
        QCOMPARE(report.removedTrees, 1);
    }

    // ---- a worker cleans up after crashed ones at start, before it stages anything ----
    void sweepAtStartCreatesNothingAndNeverFollowsTheRoot()
    {
        Rig rig;
        // no root yet: nothing happens, nothing is created
        QCOMPARE(Area::sweepStale(rig.config, time(nullptr)), SweepReport{});
        QVERIFY(!QFileInfo::exists(rig.root()));
        // a stale instance is removed without creating an instance of our own
        QDir().mkpath(rig.root() + u"/c-0000000000000dead/4"_s);
        ageBy(rig.root() + u"/c-0000000000000dead"_s, 600);
        QCOMPARE(::chmod(QFile::encodeName(rig.root()).constData(), 0700), 0);
        const auto report = Area::sweepStale(rig.config, time(nullptr));
        QCOMPARE(report.removedTrees, 1);
        QCOMPARE(entriesOf(rig.root()), QStringList());
        // a root that is a symlink, or not ours, is left strictly alone
        QTemporaryDir victim;
        QDir().mkpath(victim.path() + u"/c-0000000000000dead"_s);
        ageBy(victim.path() + u"/c-0000000000000dead"_s, 600);
        Rig linked;
        QVERIFY(QFile::link(victim.path(), linked.root()));
        QCOMPARE(Area::sweepStale(linked.config, time(nullptr)), SweepReport{});
        QVERIFY(QFileInfo::exists(victim.path() + u"/c-0000000000000dead"_s));
        Rig foreign;
        QDir().mkpath(foreign.root() + u"/c-0000000000000dead"_s);
        ageBy(foreign.root() + u"/c-0000000000000dead"_s, 600);
        foreign.config.effectiveUid = [] { return ::geteuid() + 1; };
        QCOMPARE(Area::sweepStale(foreign.config, time(nullptr)), SweepReport{});
        QVERIFY(QFileInfo::exists(foreign.root() + u"/c-0000000000000dead"_s));
    }
    void aTransfersOwnReserveOverridesTheConfiguredOne()
    {
        Rig rig;
        rig.config.reserveBytes = 1ULL << 40; // would refuse everything
        rig.config.space = [](int) { return std::optional(SpaceInfo{10ULL << 30, 100ULL << 30}); };
        QVERIFY(rig.open());
        auto refused = rig.area->begin(1, planOf({file(u"a"_s, 1000)}));
        QCOMPARE(refused->create(), Status::NoSpace);
        auto allowed = rig.area->begin(2, planOf({file(u"a"_s, 1000)}), quint64(1ULL << 30));
        QCOMPARE(allowed->create(), Status::Ok);
    }

    // REVIEW-S5: a measurement, not a test: KRDP_STAGING_BENCH=1 prints files/s for 2000 files of 20 KB (40 MB) staged with and without the
    // per-file fsync, on the file system TMPDIR is on (3 rounds each, alternating). Skipped otherwise.
    void manySmallFilesWithAndWithoutPerFileFsync()
    {
        if (qEnvironmentVariableIsEmpty("KRDP_STAGING_BENCH")) QSKIP("set KRDP_STAGING_BENCH=1 to measure");
        constexpr int Count = 2000;
        constexpr quint64 Size = 20 * 1024;
        const QByteArray data = pattern(Size, 3);
        QList<Descriptor> items;
        for (int i = 0; i < Count; ++i) items.append(file(u"d%1\\f%2.bin"_s.arg(i / 100).arg(i), Size));
        const auto plan = planOf(items);
        QVERIFY(plan.ok());
        for (int round = 0; round < 3; ++round) {
            for (const bool sync : {true, false}) {
                Rig rig;
                rig.config.syncFiles = sync;
                QVERIFY(rig.open());
                auto transfer = rig.area->begin(1, plan);
                QCOMPARE(transfer->create(), Status::Ok);
                QElapsedTimer timer;
                timer.start();
                for (quint32 i = 0; i < quint32(Count); ++i) QCOMPARE(transfer->write(i, 0, data), Status::Ok);
                const qint64 ms = timer.elapsed();
                QVERIFY(transfer->complete());
                qInfo().noquote() << QStringLiteral("BENCH fsync=%1 round=%2 files=%3 bytes=%4 ms=%5 files/s=%6 MB/s=%7")
                                         .arg(sync ? u"on"_s : u"off"_s).arg(round).arg(Count).arg(quint64(Count) * Size).arg(ms)
                                         .arg(ms ? Count * 1000 / ms : 0).arg(ms ? double(quint64(Count) * Size) / 1e6 * 1000 / double(ms) : 0.0, 0, 'f', 1);
            }
        }
    }

    // ---- no names in what this module says ----
    void statusNamesAreFixedWords()
    {
        for (int s = 0; s <= int(Status::Gone); ++s) {
            const QByteArray name = statusName(Status(s));
            QVERIFY(!name.isEmpty() && !name.contains('/') && !name.contains(' '));
        }
    }
};

QTEST_GUILESS_MAIN(HostFileStagingTest)
#include "HostFileStagingTest.moc"
