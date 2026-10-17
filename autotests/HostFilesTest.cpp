// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 2: the worker's file source (URI list, walk, snapshot reads) against real directories. Everything runs in a private
// temporary directory; the large-file cases use SPARSE files (ftruncate) and are skipped on a file system without sparse support.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "HostFiles.h"

using namespace KRdp;
using namespace KRdp::HostFiles;
using namespace Qt::StringLiterals;

extern "C" {
// Link-wrapped (-Wl,--wrap=pread): lets a test make the next pread fail.
ssize_t __real_pread(int fd, void *buf, size_t count, off_t offset);
}
static int g_failPread = 0; // errno to inject into the next pread, 0 = off
extern "C" ssize_t __wrap_pread(int fd, void *buf, size_t count, off_t offset)
{
    if (g_failPread) {
        errno = g_failPread;
        g_failPread = 0;
        return -1;
    }
    return __real_pread(fd, buf, count, offset);
}

class HostFilesTest : public QObject
{
    Q_OBJECT

    static QByteArray pathOf(const QDir &dir, const QString &name)
    {
        return QFile::encodeName(dir.filePath(name));
    }
    static void write(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
        file.write(data);
    }
    static QStringList names(const WalkResult &result)
    {
        QStringList out;
        const auto decoded = ClipboardFileDescriptor::decode(result.descriptor);
        for (const auto &item : decoded.items) out.append(item.name);
        return out;
    }
    static bool sparseWorks(const QString &directory)
    {
        const QString path = directory + QStringLiteral("/probe-sparse");
        const int fd = ::open(QFile::encodeName(path).constData(), O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0) return false;
        const bool grown = ::ftruncate(fd, qint64(5) << 30) == 0;
        struct stat st {};
        ::fstat(fd, &st);
        ::close(fd);
        ::unlink(QFile::encodeName(path).constData());
        return grown && st.st_blocks < 1024; // 512 KiB at most allocated for a 5 GiB file
    }

private Q_SLOTS:
    void uriListParsesLocalFilesOnly()
    {
        const QByteArray list = "# a comment\r\n"
                                "file:///home/user/a%20b.txt\r\n"
                                "\r\n"
                                "file://localhost/home/user/dir/\r\n"
                                "file:///home/user/a%20b.txt\r\n" // repeated
                                "file:///home/user/bad%FFname\r\n" // bytes that are not UTF-8 survive
                                "smb://server/share/file\r\n"
                                "file://otherhost/etc/passwd\r\n"
                                "file:///home/../etc/passwd\r\n"
                                "file:///home/user/./x\r\n"
                                "file:///\r\n"
                                "file://user@host/x\r\n"
                                "file:///home/user/q?x=1\r\n"
                                "relative/path\r\n"
                                "https://example.org/x\r\n"
                                "file:///home/user/nul%00name\r\n";
        const auto parsed = parseUriList(list);
        QCOMPARE(parsed.paths, (QList<QByteArray>{"/home/user/a b.txt", "/home/user/dir", QByteArray("/home/user/bad\xff") + "name"}));
        QCOMPARE(parsed.refused, 10);
        QCOMPARE(parseUriList("file:///a\nfile:///b\nfile:///c\n", 2).paths.size(), 2); // capped
        QVERIFY(parseUriList(QByteArray()).paths.isEmpty());
        QCOMPARE(parseUriList("file:///a\n").paths, (QList<QByteArray>{"/a"})); // no trailing newline needed
    }

    void walkBuildsAPreOrderListWithBackslashPaths()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QDir root(tmp.path());
        write(root.filePath(QStringLiteral("sel/b.txt")), "bb");
        write(root.filePath(QStringLiteral("sel/a/z.txt")), "zzz");
        write(root.filePath(QStringLiteral("sel/a/y/deep.bin")), QByteArray(1000, 'd'));
        QDir().mkpath(root.filePath(QStringLiteral("sel/empty")));
        write(root.filePath(QStringLiteral("single.txt")), "one");
        const auto result = walk({pathOf(root, QStringLiteral("sel")), pathOf(root, QStringLiteral("single.txt"))});
        QCOMPARE(result.status, WalkStatus::Ok);
        QCOMPARE(result.skipped, 0u);
        // Directories before their children, children in byte order, the selection in the order given.
        QCOMPARE(names(result),
                 (QStringList{u"sel"_s, u"sel\\a"_s, u"sel\\a\\y"_s, u"sel\\a\\y\\deep.bin"_s, u"sel\\a\\z.txt"_s, u"sel\\b.txt"_s, u"sel\\empty"_s, u"single.txt"_s}));
        const auto decoded = ClipboardFileDescriptor::decode(result.descriptor);
        QVERIFY(decoded.ok());
        QVERIFY(decoded.items.at(0).isDirectory());
        QCOMPARE(decoded.items.at(3).size, std::optional<quint64>(1000));
        QCOMPARE(decoded.items.at(7).size, std::optional<quint64>(3));
        QVERIFY(decoded.items.at(7).writeTime.has_value());
        QCOMPARE(result.snapshot->totalBytes(), quint64(1000 + 3 + 2 + 3));
        QCOMPARE(result.snapshot->entries().size(), qsizetype(8));
        // Reads return the bytes.
        QCOMPARE(result.snapshot->read(3, 0, 1000).data, QByteArray(1000, 'd'));
        QCOMPARE(result.snapshot->read(3, 990, 10).data, QByteArray(10, 'd'));
        QCOMPARE(result.snapshot->read(7, 1, 2).data, QByteArray("ne"));
        QCOMPARE(result.snapshot->read(1, 0, 1).status, ReadStatus::Denied); // a directory has no bytes
    }

    void walkSkipsSymlinksSpecialFilesAndPseudoFileSystems()
    {
        QTemporaryDir tmp;
        const QDir root(tmp.path());
        write(root.filePath(QStringLiteral("sel/real.txt")), "x");
        write(root.filePath(QStringLiteral("elsewhere/secret.txt")), "secret");
        QVERIFY(QFile::link(root.filePath(QStringLiteral("elsewhere/secret.txt")), root.filePath(QStringLiteral("sel/link.txt")))); // file symlink
        QVERIFY(QFile::link(root.filePath(QStringLiteral("elsewhere")), root.filePath(QStringLiteral("sel/dirlink")))); // directory symlink
        QVERIFY(QFile::link(QStringLiteral("missing"), root.filePath(QStringLiteral("sel/dangling"))));
        QCOMPARE(::mkfifo(QFile::encodeName(root.filePath(QStringLiteral("sel/pipe"))).constData(), 0600), 0);
        {
            const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            const QByteArray socketPath = QFile::encodeName(root.filePath(QStringLiteral("sel/sock")));
            strncpy(address.sun_path, socketPath.constData(), sizeof(address.sun_path) - 1);
            QCOMPARE(::bind(s, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
            ::close(s);
        }
        auto result = walk({pathOf(root, QStringLiteral("sel"))});
        QCOMPARE(result.status, WalkStatus::Ok);
        QCOMPARE(names(result), (QStringList{u"sel"_s, u"sel\\real.txt"_s}));
        QCOMPARE(result.skipped, 5u); // two links, a dangling link, a FIFO, a socket
        // A symlink as the selected item itself is refused, and a path THROUGH a symlinked directory is the user's own choice.
        result = walk({pathOf(root, QStringLiteral("sel/link.txt"))});
        QCOMPARE(result.status, WalkStatus::Empty);
        QCOMPARE(result.skipped, 1u);
        result = walk({pathOf(root, QStringLiteral("sel/dirlink/secret.txt"))});
        QCOMPARE(result.status, WalkStatus::Ok); // sel/dirlink resolves to elsewhere/: the parent is canonicalised, the leaf is a real file
        QCOMPARE(names(result), (QStringList{u"secret.txt"_s}));
        // Kernel pseudo file systems are refused as roots.
        result = walk({"/proc/self/status", "/sys/kernel", "/dev/null"});
        QCOMPARE(result.status, WalkStatus::Empty);
        QCOMPARE(result.skipped, 3u);
        // Missing items count as skipped; the rest is still offered.
        result = walk({pathOf(root, QStringLiteral("nothere")), pathOf(root, QStringLiteral("sel/real.txt"))});
        QCOMPARE(result.status, WalkStatus::Ok);
        QCOMPARE(result.skipped, 1u);
    }

    void walkReplacesHostileNamesAndRenamesCollisions()
    {
        QTemporaryDir tmp;
        const QDir root(tmp.path());
        write(root.filePath(QStringLiteral("sel/a:b.txt")), "1");
        write(root.filePath(QStringLiteral("sel/back\\slash.txt")), "2");
        write(root.filePath(QStringLiteral("sel/ctl\x01.txt")), "3");
        write(root.filePath(QStringLiteral("sel/bidi‮gpj.txt")), "4"); // right-to-left override in the extension
        write(root.filePath(QStringLiteral("sel/Case.txt")), "5");
        write(root.filePath(QStringLiteral("sel/case.TXT")), "6");
        // a name that is not UTF-8
        {
            const QByteArray bad = pathOf(root, QStringLiteral("sel")) + "/bad\xff" + "name";
            const int fd = ::open(bad.constData(), O_CREAT | O_WRONLY, 0600);
            QVERIFY(fd >= 0);
            ::close(fd);
        }
        const auto result = walk({pathOf(root, QStringLiteral("sel"))});
        ::unlink(QByteArray(pathOf(root, QStringLiteral("sel")) + "/bad\xff" + "name").constData()); // QDir cannot remove a name that is not UTF-8
        QCOMPARE(result.status, WalkStatus::Ok);
        const auto got = names(result);
        QVERIFY(got.contains(u"sel\\a_b.txt"_s));
        QVERIFY(got.contains(u"sel\\back_slash.txt"_s));
        QVERIFY(got.contains(u"sel\\ctl_.txt"_s));
        QVERIFY(got.contains(u"sel\\bidi_gpj.txt"_s));
        QCOMPARE(result.skipped, 1u); // the invalid UTF-8 name
        QVERIFY(got.contains(u"sel\\Case.txt"_s));
        QVERIFY(got.contains(u"sel\\case (2).TXT"_s)); // collides case-insensitively: the later one is renamed
        for (const auto &name : got) QVERIFY(!name.contains(QLatin1Char(':')));
        QCOMPARE(got.size(), 7); // the directory and six files
    }

    void walkDropsEntriesThatDoNotFitADescriptor()
    {
        QTemporaryDir tmp;
        const QDir root(tmp.path());
        // 5 levels of 60 characters = a leaf path of 300 units: the deep leaf does not fit a FILEDESCRIPTORW (259), its parents do.
        QString dir = QStringLiteral("sel");
        for (int i = 0; i < 4; ++i) dir += u"/"_s + QString(60, QLatin1Char('a' + i));
        write(root.filePath(dir + u"/"_s + QString(60, QLatin1Char('z'))), "deep");
        write(root.filePath(QStringLiteral("sel/short.txt")), "ok");
        const auto result = walk({pathOf(root, QStringLiteral("sel"))});
        QCOMPARE(result.status, WalkStatus::Ok);
        QCOMPARE(result.skipped, 1u);
        for (const auto &name : names(result)) QVERIFY(name.size() <= ClipboardFileDescriptor::MaxNameUnits);
        QVERIFY(names(result).contains(u"sel\\short.txt"_s));
        // A directory whose own path is too long takes its children with it.
        QString longDir = QStringLiteral("sel2");
        for (int i = 0; i < 5; ++i) longDir += u"/"_s + QString(60, QLatin1Char('a' + i));
        write(root.filePath(longDir + QStringLiteral("/f.txt")), "x");
        const auto second = walk({pathOf(root, QStringLiteral("sel2"))});
        QCOMPARE(second.status, WalkStatus::Ok);
        QVERIFY(second.skipped >= 2u);
        for (const auto &name : names(second)) QVERIFY(name.size() <= ClipboardFileDescriptor::MaxNameUnits);
    }

    void walkIsBounded()
    {
        QTemporaryDir tmp;
        const QDir root(tmp.path());
        // depth
        QString deep = QStringLiteral("deep");
        for (int i = 0; i < 70; ++i) deep += QStringLiteral("/d");
        QDir().mkpath(root.filePath(deep));
        QCOMPARE(walk({pathOf(root, QStringLiteral("deep"))}).status, WalkStatus::TooDeep);
        WalkOptions slow; // a Debug build needs more than the production 5 s for the 100 000-entry case
        slow.budgetMs = 600000;
        // time budget: a fake clock that jumps 10 s per reading
        write(root.filePath(QStringLiteral("many/f")), "x");
        for (int i = 0; i < 200; ++i) write(root.filePath(QStringLiteral("many/f%1").arg(i)), "x");
        WalkOptions options;
        qint64 t = 0;
        options.nowMs = [&t] { return t += 10000; };
        options.budgetMs = 5000;
        QCOMPARE(walk({pathOf(root, QStringLiteral("many"))}, options).status, WalkStatus::Budget);
        // entries: one directory of 100 001 empty files is over the list's cap
        const QString big = root.filePath(QStringLiteral("big"));
        QDir().mkpath(big);
        const QByteArray bigPath = QFile::encodeName(big);
        for (quint32 i = 0; i < ClipboardFileLimits::MaxEntries; ++i) {
            const QByteArray file = bigPath + "/" + QByteArray::number(i);
            const int fd = ::open(file.constData(), O_CREAT | O_WRONLY, 0600);
            QVERIFY(fd >= 0);
            ::close(fd);
        }
        QCOMPARE(walk({bigPath}, slow).status, WalkStatus::TooMany); // the directory itself is entry 1
        // exactly at the cap is fine: remove the directory entry's share by selecting the files alone is not possible here, so remove one file
        QFile::remove(QFile::decodeName(bigPath + "/0"));
        const auto exact = walk({bigPath}, slow);
        QCOMPARE(exact.status, WalkStatus::Ok);
        QCOMPARE(exact.count, int(ClipboardFileLimits::MaxEntries));
        QDir(big).removeRecursively();
    }

    void filesOverFourGiBNeedHugeSupportAndKeepTheirOffsets()
    {
        QTemporaryDir tmp;
        if (!sparseWorks(tmp.path())) QSKIP("the file system has no sparse file support");
        const QDir root(tmp.path());
        const QString big = root.filePath(QStringLiteral("big.bin"));
        {
            QFile file(big);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.resize(qint64(5) << 30));
            QVERIFY(file.seek((qint64(4) << 30) - 3));
            QCOMPARE(file.write("ABCDEFG"), 7); // crosses 4 GiB
            QVERIFY(file.seek((qint64(5) << 30) - 4));
            QCOMPARE(file.write("TAIL"), 4);
        }
        write(root.filePath(QStringLiteral("small.txt")), "s");
        // Without huge support the 5 GiB file is skipped and counted, never truncated.
        auto result = walk({QFile::encodeName(big), pathOf(root, QStringLiteral("small.txt"))});
        QCOMPARE(result.status, WalkStatus::Ok);
        QCOMPARE(names(result), (QStringList{u"small.txt"_s}));
        QCOMPARE(result.skipped, 1u);
        WalkOptions huge;
        huge.hugeFiles = true;
        result = walk({QFile::encodeName(big), pathOf(root, QStringLiteral("small.txt"))}, huge);
        QCOMPARE(result.status, WalkStatus::Ok);
        QCOMPARE(names(result), (QStringList{u"big.bin"_s, u"small.txt"_s}));
        const auto decoded = ClipboardFileDescriptor::decode(result.descriptor);
        QCOMPARE(decoded.items.at(0).size, std::optional<quint64>(quint64(5) << 30));
        QCOMPARE(result.snapshot->totalBytes(), (quint64(5) << 30) + 1);
        const quint64 four = quint64(4) << 30;
        QCOMPARE(result.snapshot->read(0, four - 3, 7).data, QByteArray("ABCDEFG"));
        QCOMPARE(result.snapshot->read(0, four - 100, 200).data.mid(97, 6), QByteArray("ABCDEF"));
        QCOMPARE(result.snapshot->read(0, (quint64(5) << 30) - 4, 4).data, QByteArray("TAIL"));
        QCOMPARE(result.snapshot->read(0, four + 1000, 4096).data, QByteArray(4096, '\0')); // a hole
        QCOMPARE(result.snapshot->read(0, quint64(5) << 30, 1).status, ReadStatus::Denied); // past the end
    }

    void aChangedOrVanishedFileFailsTheRead()
    {
        QTemporaryDir tmp;
        const QDir root(tmp.path());
        const auto make = [&](const QString &name, const QByteArray &data) { write(root.filePath(name), data); };
        for (const char *name : {"grow", "shrink", "touch", "vanish", "replace", "symlink", "mode", "same"}) make(QString::fromLatin1(name), QByteArray(4096, 'x'));
        QList<QByteArray> selection;
        for (const char *name : {"grow", "shrink", "touch", "vanish", "replace", "symlink", "mode", "same"}) selection.append(pathOf(root, QString::fromLatin1(name)));
        const auto result = walk(selection);
        QCOMPARE(result.status, WalkStatus::Ok);
        const auto snapshot = result.snapshot;
        const auto indexOf = [&](const char *name) {
            const auto decoded = ClipboardFileDescriptor::decode(result.descriptor);
            for (int i = 0; i < decoded.items.size(); ++i) if (decoded.items.at(i).name == QLatin1String(name)) return quint32(i);
            return quint32(9999);
        };
        // Untouched: reads fine, repeatedly (the descriptor is cached).
        QCOMPARE(snapshot->read(indexOf("same"), 0, 4096).status, ReadStatus::Ok);
        QCOMPARE(snapshot->read(indexOf("same"), 1, 100).status, ReadStatus::Ok);
        // The first read opens them all while they are intact, so the "changed after the first read" paths are exercised too.
        for (const char *name : {"grow", "shrink", "touch", "vanish", "replace", "symlink", "mode"}) QCOMPARE(snapshot->read(indexOf(name), 0, 16).status, ReadStatus::Ok);
        {
            QFile f(root.filePath(QStringLiteral("grow")));
            QVERIFY(f.open(QIODevice::Append));
            f.write("more");
        }
        QCOMPARE(snapshot->read(indexOf("grow"), 0, 16).status, ReadStatus::Changed);
        QVERIFY(QFile(root.filePath(QStringLiteral("shrink"))).resize(100));
        QCOMPARE(snapshot->read(indexOf("shrink"), 0, 16).status, ReadStatus::Changed);
        {
            struct timeval times[2] = {{1000000000, 0}, {1000000000, 0}};
            QCOMPARE(::utimes(QFile::encodeName(root.filePath(QStringLiteral("touch"))).constData(), times), 0);
        }
        QCOMPARE(snapshot->read(indexOf("touch"), 0, 16).status, ReadStatus::Changed);
        QVERIFY(QFile::remove(root.filePath(QStringLiteral("vanish"))));
        QCOMPARE(snapshot->read(indexOf("vanish"), 0, 16).status, ReadStatus::Gone); // held open but unlinked
        // Replaced by another file of the same size: a different inode.
        QVERIFY(QFile::remove(root.filePath(QStringLiteral("replace"))));
        make(QStringLiteral("replace"), QByteArray(4096, 'y'));
        QCOMPARE(snapshot->read(indexOf("replace"), 0, 16).status, ReadStatus::Gone); // the held inode is unlinked
        // Replaced by a symlink to a file with the same content: a re-open must not follow it.
        QVERIFY(QFile::remove(root.filePath(QStringLiteral("symlink"))));
        QVERIFY(QFile::link(root.filePath(QStringLiteral("same")), root.filePath(QStringLiteral("symlink"))));
        QCOMPARE(snapshot->read(indexOf("symlink"), 0, 16).status, ReadStatus::Gone);
        QCOMPARE(snapshot->read(indexOf("symlink"), 0, 16).status, ReadStatus::Changed); // re-opened now: O_NOFOLLOW refuses the link
        QCOMPARE(snapshot->read(indexOf("replace"), 0, 16).status, ReadStatus::Changed); // re-opened now: a new inode
        QCOMPARE(snapshot->read(indexOf("vanish"), 0, 16).status, ReadStatus::Gone); // no such file
        // A file that is unreadable now.
        QCOMPARE(::chmod(QFile::encodeName(root.filePath(QStringLiteral("mode"))).constData(), 0), 0);
        QCOMPARE(snapshot->read(indexOf("mode"), 0, 16).status, ReadStatus::Ok); // still held open from the earlier read
        QCOMPARE(snapshot->read(indexOf("same"), 0, 16).status, ReadStatus::Ok);
    }

    void readsAreBoundedAndCacheFewDescriptors()
    {
        QTemporaryDir tmp;
        const QDir root(tmp.path());
        QList<QByteArray> selection;
        for (int i = 0; i < 20; ++i) {
            write(root.filePath(QStringLiteral("f%1").arg(i, 2, 10, QLatin1Char('0'))), QByteArray(100, char('a' + i)));
            selection.append(pathOf(root, QStringLiteral("f%1").arg(i, 2, 10, QLatin1Char('0'))));
        }
        const auto result = walk(selection);
        QCOMPARE(result.status, WalkStatus::Ok);
        for (int round = 0; round < 2; ++round) {
            for (quint32 i = 0; i < 20; ++i) {
                QCOMPARE(result.snapshot->read(i, 10, 20).data, QByteArray(20, char('a' + int(i))));
                QVERIFY(result.snapshot->openCount() <= Snapshot::MaxOpen);
            }
        }
        QCOMPARE(result.snapshot->read(0, 0, 0).status, ReadStatus::Denied); // no zero-length reads
        QCOMPARE(result.snapshot->read(0, 90, 20).status, ReadStatus::Denied); // past the end
        QCOMPARE(result.snapshot->read(0, ~quint64(0), 10).status, ReadStatus::Denied); // overflow
        QCOMPARE(result.snapshot->read(20, 0, 1).status, ReadStatus::Denied); // no such index
        QCOMPARE(result.snapshot->read(0, 0, ClipboardFileLimits::MaxRangeBytes + 1).status, ReadStatus::Denied);
        QCOMPARE(result.snapshot->read(0, 0, 100).data, QByteArray(100, 'a'));
    }

    void anIoErrorFailsTheReadAndTheNextOneRecovers()
    {
        QTemporaryDir tmp;
        const QDir root(tmp.path());
        write(root.filePath(QStringLiteral("f")), QByteArray(1000, 'q'));
        const auto result = walk({pathOf(root, QStringLiteral("f"))});
        QCOMPARE(result.status, WalkStatus::Ok);
        g_failPread = EIO;
        QCOMPARE(result.snapshot->read(0, 0, 100).status, ReadStatus::IoError);
        QCOMPARE(result.snapshot->read(0, 0, 100).status, ReadStatus::Ok); // re-opened
    }

    void aSelectionOfOnlyUnreadableThingsIsEmpty()
    {
        QTemporaryDir tmp;
        const QDir root(tmp.path());
        QCOMPARE(walk({}).status, WalkStatus::Empty);
        QCOMPARE(walk({pathOf(root, QStringLiteral("gone"))}).status, WalkStatus::Empty);
    }
};

QTEST_GUILESS_MAIN(HostFilesTest)
#include "HostFilesTest.moc"
