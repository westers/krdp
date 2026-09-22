// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionMaintenanceGuard.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QUuid>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>

namespace { int failSync = 0; bool shortWrite = false, failRename = false; }
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (failSync && --failSync == 0) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" ssize_t __wrap_write(int fd, const void *p, size_t size) {
    if (shortWrite) { shortWrite = false; return __real_write(fd, p, size / 2); }
    return __real_write(fd, p, size);
}
extern "C" int __real_renameat(int, const char *, int, const char *);
extern "C" int __wrap_renameat(int a, const char *b, int c, const char *d) {
    if (failRename) { failRename = false; errno = EIO; return -1; }
    return __real_renameat(a,b,c,d);
}
namespace KRdp {
class VirtualSessionMaintenanceGuardTest : public QObject {
    Q_OBJECT
    using G = VirtualSessionMaintenanceGuard;
    using P = G::Publication;
    const QString boot = QStringLiteral("12345678-1234-1234-1234-123456789abc");
    const QString digest = QString(64, QLatin1Char('a'));
    static bool put(const QString &path, const QByteArray &data) {
        QFile f(path);
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
            && f.write(data) == data.size();
    }
    auto lease(const QString &path, bool exclusive = true) {
        return G::acquire(path, getuid(), boot, exclusive, true, nullptr);
    }
    bool initialize(const QString &path) {
        if (!put(path + QStringLiteral("/lock"), {})) return false;
        auto l = lease(path); QString txn;
        return l && l->block(&txn) == P::Durable && l->publishClean(txn, digest) == P::Durable;
    }
private Q_SLOTS:
    void init() { failSync = 0; shortWrite = failRename = false; }
    void missingAndTransactions() {
        QTemporaryDir dir;
        QVERIFY(!lease(dir.path()));
        QVERIFY(put(dir.filePath(QStringLiteral("lock")), {}));
        auto l = lease(dir.path()); QVERIFY(l);
        QVERIFY(!l->read()); QVERIFY(!l->admit(digest, nullptr));
        QVERIFY(l->publishClean(boot, digest) != P::Durable);
        QString first, second;
        QCOMPARE(l->block(&first), P::Durable);
        QCOMPARE(l->block(&second), P::Durable); QVERIFY(first != second);
        QCOMPARE(l->publishClean(first, digest), P::FailedBeforeRename);
        QCOMPARE(l->read()->epoch, second);
        QCOMPARE(l->publishClean(second, digest), P::Durable);
        QVERIFY(l->read()->epoch != second);
        QVERIFY(l->admit(digest, nullptr));
        QVERIFY(!l->admit(QString(64, QLatin1Char('b')), nullptr));
        l->m_boot = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(!l->admit(digest, nullptr));
    }
    void sharedExclusiveMove() {
        QTemporaryDir dir; QVERIFY(initialize(dir.path()));
        auto a = lease(dir.path(), false), b = lease(dir.path(), false);
        QVERIFY(a && b); QVERIFY(a->admit(digest, nullptr)); QVERIFY(!lease(dir.path()));
        QCOMPARE(fcntl(a->m_lock, F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
        QCOMPARE(fcntl(a->m_directory, F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
        G::Lease moved(std::move(*a)); QVERIFY(!a->read()); QVERIFY(moved.read());
        QString txn; QCOMPARE(moved.block(&txn), P::FailedBeforeRename);
        moved = std::move(*b); QVERIFY(!b->read()); QVERIFY(!lease(dir.path()));
        moved.close(); QVERIFY(lease(dir.path()));
    }
    void inheritedDescriptionRetainsLockUntilExec() {
        QTemporaryDir dir; QVERIFY(initialize(dir.path()));
        auto parent = lease(dir.path()); QVERIFY(parent);
        const QByteArray command = QByteArray("test ! -e /proc/self/fd/") + QByteArray::number(parent->m_lock)
            + " && test ! -e /proc/self/fd/" + QByteArray::number(parent->m_directory);
        int proceed[2]; QVERIFY(!pipe(proceed));
        const pid_t pid = fork(); QVERIFY(pid >= 0);
        if (!pid) {
            ::close(proceed[1]);
            char c;
            if (::read(proceed[0], &c, 1) != 1) _exit(4);
            ::close(proceed[0]);
            execl("/bin/sh", "sh", "-c", command.constData(), static_cast<char *>(nullptr));
            _exit(5);
        }
        ::close(proceed[0]); parent.reset();
        // The inherited open file description still holds the lock, even
        // though inherited API use is forbidden. CLOEXEC must finally drop it.
        const bool conflict = !lease(dir.path());
        QCOMPARE(__real_write(proceed[1], "x", 1), ssize_t(1)); ::close(proceed[1]);
        int status; QCOMPARE(waitpid(pid, &status, 0), pid);
        QVERIFY(conflict); QVERIFY(WIFEXITED(status)); QCOMPARE(WEXITSTATUS(status), 0);
        QVERIFY(lease(dir.path()));
    }
    void invalidPublicationAndWrongBootTransaction() {
        QTemporaryDir dir; QVERIFY(initialize(dir.path())); auto l = lease(dir.path()); QVERIFY(l);
        const auto before = l->read(); QVERIFY(before);
        QCOMPARE(l->publish({false, boot, boot, digest}, nullptr), P::FailedBeforeRename);
        QCOMPARE(*l->read(), *before);
        QString txn; QCOMPARE(l->block(&txn), P::Durable);
        l->m_boot = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(l->publishClean(txn, digest), P::FailedBeforeRename);
        QVERIFY(!l->read()->clean);
    }
    void processContentionCrashAndInheritedFd() {
        QTemporaryDir dir; QVERIFY(initialize(dir.path()));
        int ready[2], finish[2]; QVERIFY(!pipe(ready)); QVERIFY(!pipe(finish));
        pid_t pid = fork(); QVERIFY(pid >= 0);
        if (!pid) {
            ::close(ready[0]); ::close(finish[1]);
            auto l = lease(dir.path());
            char result = l ? 'y' : 'n';
            if (l) { QString txn; if (l->block(&txn) != P::Durable) result = 'n'; }
            __real_write(ready[1], &result, 1);
            char c; _exit(::read(finish[0], &c, 1) == 1 ? 0 : 4);
        }
        ::close(ready[1]); ::close(finish[0]); char c = 0;
        QCOMPARE(::read(ready[0], &c, 1), ssize_t(1)); QCOMPARE(c, 'y');
        QVERIFY(!lease(dir.path(), false)); QVERIFY(!kill(pid, SIGKILL));
        int status; QCOMPARE(waitpid(pid, &status, 0), pid); QVERIFY(WIFSIGNALED(status));
        ::close(ready[0]); ::close(finish[1]);
        auto parent = lease(dir.path()); QVERIFY(parent); QVERIFY(!parent->admit(digest, nullptr));
        pid = fork(); QVERIFY(pid >= 0);
        if (!pid) {
            if (parent->read()) _exit(2);
            parent.reset(); // Must close, not LOCK_UN the shared description.
            _exit(lease(dir.path()) ? 3 : 0);
        }
        QCOMPARE(waitpid(pid, &status, 0), pid); QVERIFY(WIFEXITED(status)); QCOMPARE(WEXITSTATUS(status), 0);
        QVERIFY(!lease(dir.path())); parent.reset(); QVERIFY(lease(dir.path()));
    }
    void unsafe_data() {
        QTest::addColumn<QString>("object"); QTest::addColumn<QString>("kind");
        for (const auto *obj : {"state", "lock"}) for (const auto *kind : {"symlink", "hardlink", "mode", "fifo", "directory", "owner"})
            QTest::newRow(qPrintable(QString::fromLatin1(obj) + QLatin1Char('-') + QString::fromLatin1(kind))) << QString::fromLatin1(obj) << QString::fromLatin1(kind);
    }
    void unsafe() {
        QFETCH(QString, object); QFETCH(QString, kind);
        QTemporaryDir dir; QVERIFY(initialize(dir.path()));
        auto l = lease(dir.path()); QVERIFY(l);
        const QString path = dir.filePath(object), backup = path + QStringLiteral("-backup");
        if (kind == QStringLiteral("owner")) {
            l->m_owner = getuid() + 1; QVERIFY(!l->read()); return;
        }
        if (kind == QStringLiteral("mode")) QVERIFY(!chmod(QFile::encodeName(path).constData(), 0640));
        else if (kind == QStringLiteral("hardlink")) QVERIFY(!link(QFile::encodeName(path).constData(), QFile::encodeName(backup).constData()));
        else {
            QVERIFY(QFile::rename(path, backup));
            if (kind == QStringLiteral("symlink")) QVERIFY(QFile::link(backup, path));
            if (kind == QStringLiteral("fifo")) QVERIFY(!mkfifo(QFile::encodeName(path).constData(), 0600));
            if (kind == QStringLiteral("directory")) QVERIFY(QDir().mkdir(path));
        }
        QVERIFY(!l->read()); QVERIFY(!l->admit(digest, nullptr));
        QString txn; QCOMPARE(l->block(&txn), P::FailedBeforeRename);
    }
    void replacementAndAncestry() {
        QTemporaryDir dir; const auto path = dir.filePath(QStringLiteral("guard"));
        QVERIFY(QDir().mkdir(path)); QVERIFY(!chmod(QFile::encodeName(path).constData(), 0700)); QVERIFY(initialize(path));
        auto l = lease(path); QVERIFY(l);
        QVERIFY(QFile::rename(path + QStringLiteral("/lock"), path + QStringLiteral("/old-lock")));
        QVERIFY(put(path + QStringLiteral("/lock"), {})); QVERIFY(!l->read());
        l.reset(); l = lease(path); QVERIFY(l);
        QVERIFY(QDir().rename(path, path + QStringLiteral("-old")));
        QVERIFY(QDir().mkdir(path)); QVERIFY(!chmod(QFile::encodeName(path).constData(), 0700)); QVERIFY(initialize(path));
        QVERIFY(!l->read());
        QVERIFY(QFile::link(path, dir.filePath(QStringLiteral("alias")))); QVERIFY(!lease(dir.filePath(QStringLiteral("alias"))));
        QVERIFY(!chmod(QFile::encodeName(path).constData(), 0755)); QVERIFY(!lease(path));
    }
    void schema_data() {
        QTest::addColumn<QByteArray>("data");
        const QByteArray prefix = "KRDP-MAINTENANCE-1\nclean\n12345678-1234-1234-1234-123456789abc\n12345678-1234-1234-1234-123456789abc\n";
        const QByteArray valid = prefix + QByteArray(64, 'a') + '\n';
        QTest::newRow("empty") << QByteArray(); QTest::newRow("truncated") << valid.chopped(1);
        QTest::newRow("extra") << valid + "extra\n"; QTest::newRow("oversize") << QByteArray(258, 'a');
        QTest::newRow("digest") << prefix + QByteArray(64, 'A') + '\n';
        auto invalid = valid; invalid.replace("clean", "unknown"); QTest::newRow("domain") << invalid;
        invalid = valid; invalid.replace("MAINTENANCE-1", "MAINTENANCE-2"); QTest::newRow("version") << invalid;
        invalid = valid; invalid.replace("12345678-1234", "12345678_1234"); QTest::newRow("uuid") << invalid;
        invalid = valid; invalid.replace("clean", "blocked"); QTest::newRow("blocked-with-profile") << invalid;
    }
    void schema() {
        QFETCH(QByteArray, data); QTemporaryDir dir; QVERIFY(initialize(dir.path()));
        QVERIFY(put(dir.filePath(QStringLiteral("state")), data)); auto l = lease(dir.path()); QVERIFY(l);
        QVERIFY(!l->read()); QVERIFY(!l->admit(digest, nullptr));
        QString txn; QCOMPARE(l->block(&txn), P::FailedBeforeRename);
    }
    void publicationFaults_data() {
        QTest::addColumn<int>("fault"); for (int i = 0; i < 4; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void publicationFaults() {
        QFETCH(int, fault); QTemporaryDir dir; QVERIFY(initialize(dir.path())); auto l = lease(dir.path()); QVERIFY(l);
        const auto before = l->read(); QVERIFY(before);
        shortWrite = fault == 0; failSync = fault == 1 ? 1 : fault == 3 ? 2 : 0; failRename = fault == 2;
        QString txn; const auto result = l->block(&txn);
        QCOMPARE(result, fault == 3 ? P::UncertainAfterRename : P::FailedBeforeRename);
        if (fault == 3) { QVERIFY(!l->read()->clean); QVERIFY(!l->admit(digest, nullptr)); }
        else { QCOMPARE(*l->read(), *before); QVERIFY(!QDir(dir.path()).entryList({QStringLiteral(".state-*")}, QDir::Files | QDir::Hidden).isEmpty()); }
        QCOMPARE(l->block(&txn), P::Durable);
        failSync = 2; QCOMPARE(l->publishClean(txn, digest), P::UncertainAfterRename);
        QVERIFY(l->read()->clean);
        // Every admission retries both syncs on that same complete clean record.
        for (int sync : {1, 2}) { failSync = sync; QVERIFY(!l->admit(digest, nullptr)); }
        QVERIFY(l->admit(digest, nullptr));
    }
};
}
QTEST_GUILESS_MAIN(KRdp::VirtualSessionMaintenanceGuardTest)
#include "VirtualSessionMaintenanceGuardTest.moc"
