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
#include <sys/file.h>
#include <cstdarg>
#include <functional>

namespace { int failSync = 0; bool shortWrite = false, failRename = false; }
namespace { int ofdError = 0, ofdFailureCountdown = 0; std::function<void()> atGate; }
extern "C" int __real_fcntl(int, int, ...);
extern "C" int __wrap_fcntl(int fd, int command, ...) {
    if (command == F_GETFD) return __real_fcntl(fd, command);
    va_list args; va_start(args, command); auto *lock = va_arg(args, struct flock *); va_end(args);
    if (command == F_OFD_SETLK && ofdError && --ofdFailureCountdown == 0) { errno = ofdError; return -1; }
    return __real_fcntl(fd, command, lock);
}
extern "C" int __real_flock(int, int);
extern "C" int __wrap_flock(int fd, int operation) {
    if (atGate) { auto callback = std::move(atGate); atGate = {}; callback(); }
    return __real_flock(fd, operation);
}
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
    bool packages(const QString &path) {
        return put(path + QStringLiteral("/lock-frontend"), {}) && put(path + QStringLiteral("/lock"), {})
            && !chmod(QFile::encodeName(path + QStringLiteral("/lock-frontend")).constData(), 0640)
            && !chmod(QFile::encodeName(path + QStringLiteral("/lock")).constData(), 0640);
    }
    auto compound(const QString &guard, const QString &package) {
        return G::admissionAt(guard, package, getuid(), boot, digest, true, nullptr);
    }
    static bool writer(const QString &path) {
        // Always a separate process: POSIX close-any-fd semantics must not
        // contaminate the process whose OFD lease is being tested.
        const pid_t child = fork();
        if (!child) {
            int fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CLOEXEC);
            struct flock lock{}; lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET;
            _exit(fd >= 0 && __real_fcntl(fd, F_SETLK, &lock) == 0 ? 0 : 1);
        }
        int status = 0;
        return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
private Q_SLOTS:
    void init() { failSync = ofdError = ofdFailureCountdown = 0; shortWrite = failRename = false; atGate = {}; }
    void compoundLifetimeAndUnrelatedClose() {
        QTemporaryDir guard, pkg; QVERIFY(initialize(guard.path())); QVERIFY(packages(pkg.path()));
        auto a = compound(guard.path(), pkg.path()), b = compound(guard.path(), pkg.path()); QVERIFY(a && b);
        for (const auto *name : {"lock-frontend", "lock"}) {
            const auto path = pkg.filePath(QString::fromLatin1(name));
            QVERIFY(!writer(path));
            const int alias = ::open(QFile::encodeName(path).constData(), O_RDONLY); QVERIFY(alias >= 0); ::close(alias);
            QVERIFY(!writer(path));
        }
        G::Lease moved(std::move(*a)); a.reset(); b.reset();
        QVERIFY(!writer(pkg.filePath(QStringLiteral("lock"))));
        QTemporaryDir otherGuard, otherPkg; QVERIFY(initialize(otherGuard.path())); QVERIFY(packages(otherPkg.path()));
        auto other = compound(otherGuard.path(), otherPkg.path()); QVERIFY(other);
        moved = std::move(*other); other.reset();
        QVERIFY(writer(pkg.filePath(QStringLiteral("lock-frontend")))); QVERIFY(writer(pkg.filePath(QStringLiteral("lock"))));
        QVERIFY(!writer(otherPkg.filePath(QStringLiteral("lock")))); moved.close();
        QVERIFY(writer(otherPkg.filePath(QStringLiteral("lock-frontend")))); QVERIFY(writer(otherPkg.filePath(QStringLiteral("lock"))));
    }
    void packageBusyAndGateOnlyInvalidation_data() {
        QTest::addColumn<QString>("name"); QTest::newRow("frontend") << QStringLiteral("lock-frontend"); QTest::newRow("backend") << QStringLiteral("lock");
    }
    void packageBusyAndGateOnlyInvalidation() {
        QFETCH(QString, name); QTemporaryDir guard, pkg; QVERIFY(initialize(guard.path())); QVERIFY(packages(pkg.path()));
        int ready[2], done[2]; QVERIFY(!pipe(ready)); QVERIFY(!pipe(done));
        pid_t child = fork(); QVERIFY(child >= 0);
        if (!child) {
            ::close(ready[0]); ::close(done[1]);
            int fd = ::open(QFile::encodeName(pkg.filePath(name)).constData(), O_RDWR);
            struct flock lock{}; lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET;
            char result = fd >= 0 && __real_fcntl(fd, F_SETLK, &lock) == 0 ? 'y' : 'n';
            __real_write(ready[1], &result, 1); char c; _exit(::read(done[0], &c, 1) == 1 ? 0 : 1);
        }
        ::close(ready[1]); ::close(done[0]); char result;
        QCOMPARE(::read(ready[0], &result, 1), ssize_t(1)); QCOMPARE(result, 'y');
        const bool refused = !compound(guard.path(), pkg.path());
        const bool unwound = name != QStringLiteral("lock") || writer(pkg.filePath(QStringLiteral("lock-frontend")));
        auto gate = lease(guard.path()); QString txn;
        const bool invalidated = gate && gate->block(&txn) == P::Durable;
        QCOMPARE(__real_write(done[1], "x", 1), ssize_t(1)); ::close(done[1]); ::close(ready[0]);
        int status; QCOMPARE(waitpid(child, &status, 0), child);
        QVERIFY(refused); QVERIFY(unwound); QVERIFY(invalidated); gate.reset();
        QVERIFY(writer(pkg.filePath(QStringLiteral("lock-frontend")))); QVERIFY(writer(pkg.filePath(QStringLiteral("lock"))));
    }
    void compoundFailureUnwinds_data() {
        QTest::addColumn<QString>("kind");
        for (auto kind : {"gate", "state", "sync", "unsupported", "error", "unsupported-backend", "error-backend", "replacement"}) QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void compoundFailureUnwinds() {
        QFETCH(QString, kind); QTemporaryDir guard, pkg; QVERIFY(initialize(guard.path())); QVERIFY(packages(pkg.path()));
        std::optional<G::Lease> held;
        if (kind == QStringLiteral("gate")) { held = lease(guard.path()); QVERIFY(held); }
        if (kind == QStringLiteral("state")) QVERIFY(put(guard.filePath(QStringLiteral("state")), "bad"));
        if (kind == QStringLiteral("sync")) failSync = 1;
        if (kind.startsWith(QStringLiteral("unsupported"))) ofdError = EINVAL;
        if (kind.startsWith(QStringLiteral("error"))) ofdError = EIO;
        if (ofdError) ofdFailureCountdown = kind.endsWith(QStringLiteral("-backend")) ? 2 : 1;
        bool replaced = false;
        if (kind == QStringLiteral("replacement")) atGate = [&] {
            const auto name = pkg.filePath(QStringLiteral("lock"));
            replaced = QFile::rename(name, name + QStringLiteral("-old")) && put(name, {});
        };
        QVERIFY(!compound(guard.path(), pkg.path()));
        if (ofdError) QCOMPARE(ofdFailureCountdown, 0);
        if (kind == QStringLiteral("replacement")) QVERIFY(replaced);
        ofdError = 0;
        QVERIFY(writer(pkg.filePath(QStringLiteral("lock-frontend")))); QVERIFY(writer(pkg.filePath(QStringLiteral("lock"))));
        if (replaced) QVERIFY(writer(pkg.filePath(QStringLiteral("lock-old"))));
    }
    void packageUnsafe_data() {
        QTest::addColumn<QString>("kind");
        for (auto kind : {"missing", "symlink", "hardlink", "mode", "group-write", "fifo", "directory", "ancestry", "alias"}) QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void packageUnsafe() {
        QFETCH(QString, kind); QTemporaryDir guard, pkg; QVERIFY(initialize(guard.path())); QVERIFY(packages(pkg.path()));
        const QString path = pkg.filePath(QStringLiteral("lock")), old = path + QStringLiteral("-old");
        if (kind == QStringLiteral("mode")) QVERIFY(!chmod(QFile::encodeName(path).constData(), 0602));
        else if (kind == QStringLiteral("group-write")) QVERIFY(!chmod(QFile::encodeName(path).constData(), 0660));
        else if (kind == QStringLiteral("ancestry")) QVERIFY(!chmod(QFile::encodeName(pkg.path()).constData(), 0777));
        else if (kind == QStringLiteral("hardlink")) QVERIFY(!link(QFile::encodeName(path).constData(), QFile::encodeName(old).constData()));
        else {
            QVERIFY(QFile::rename(path, old));
            if (kind == QStringLiteral("symlink")) QVERIFY(QFile::link(old, path));
            if (kind == QStringLiteral("fifo")) QVERIFY(!mkfifo(QFile::encodeName(path).constData(), 0600));
            if (kind == QStringLiteral("directory")) QVERIFY(QDir().mkdir(path));
            if (kind == QStringLiteral("alias")) QVERIFY(!link(QFile::encodeName(pkg.filePath(QStringLiteral("lock-frontend"))).constData(), QFile::encodeName(path).constData()));
        }
        QVERIFY(!compound(guard.path(), pkg.path()));
        QVERIFY(writer(pkg.filePath(QStringLiteral("lock-frontend"))));
    }
    void compoundForkExec() {
        QTemporaryDir guard, pkg; QVERIFY(initialize(guard.path())); QVERIFY(packages(pkg.path()));
        auto parent = compound(guard.path(), pkg.path()); QVERIFY(parent);
        const pid_t closer = fork(); QVERIFY(closer >= 0);
        if (!closer) { parent.reset(); _exit(0); }
        int closeStatus; QCOMPARE(waitpid(closer, &closeStatus, 0), closer);
        QVERIFY(WIFEXITED(closeStatus)); QCOMPARE(WEXITSTATUS(closeStatus), 0);
        // Child destruction must close only, never explicitly unlock the
        // parent's shared open file descriptions (OFD locks or guard flock).
        QVERIFY(!writer(pkg.filePath(QStringLiteral("lock-frontend"))));
        QVERIFY(!writer(pkg.filePath(QStringLiteral("lock")))); QVERIFY(!lease(guard.path()));
        QByteArray command("true");
        for (int fd : {parent->m_lock, parent->m_directory, parent->m_packages->m_frontend, parent->m_packages->m_backend, parent->m_packages->m_directory}) {
            QCOMPARE(fcntl(fd, F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
            command += " && test ! -e /proc/self/fd/" + QByteArray::number(fd);
        }
        int proceed[2]; QVERIFY(!pipe(proceed)); pid_t child = fork(); QVERIFY(child >= 0);
        if (!child) {
            ::close(proceed[1]);
            if (parent->read() || parent->m_packages->associated()) _exit(2);
            char c; if (::read(proceed[0], &c, 1) != 1) _exit(3); ::close(proceed[0]);
            execl("/bin/sh", "sh", "-c", command.constData(), static_cast<char *>(nullptr)); _exit(4);
        }
        ::close(proceed[0]); parent.reset();
        bool pinned = !writer(pkg.filePath(QStringLiteral("lock-frontend"))) && !writer(pkg.filePath(QStringLiteral("lock"))) && !lease(guard.path());
        QCOMPARE(__real_write(proceed[1], "x", 1), ssize_t(1)); ::close(proceed[1]); int status;
        QCOMPARE(waitpid(child, &status, 0), child); QVERIFY(WIFEXITED(status)); QCOMPARE(WEXITSTATUS(status), 0); QVERIFY(pinned);
        QVERIFY(writer(pkg.filePath(QStringLiteral("lock-frontend")))); QVERIFY(writer(pkg.filePath(QStringLiteral("lock")))); QVERIFY(lease(guard.path()));
    }
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
