// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualUnattendedUpgradeGuard.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
int failSync = 0, execCalls = 0;
QByteArray releaseCheck;
bool releasedBeforeExec = false;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (failSync && --failSync == 0) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
extern "C" int __real_execv(const char *, char *const []);
extern "C" int __wrap_execv(const char *path, char *const args[]) {
    ++execCalls;
    if (!releaseCheck.isEmpty()) {
        const int fd = open(releaseCheck.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        releasedBeforeExec = fd >= 0 && !flock(fd, LOCK_EX | LOCK_NB);
        if (fd >= 0) close(fd);
        if (!releasedBeforeExec) { errno = EBUSY; return -1; }
    }
    return __real_execv(path, args);
}
namespace KRdp {
class VirtualUnattendedUpgradeGuardTest : public QObject {
    Q_OBJECT
    using G = VirtualSessionMaintenanceGuard;
    using R = VirtualSessionMaintenanceRecord;
    using W = VirtualUnattendedUpgradeGuard;
    const QString boot = QStringLiteral("12345678-1234-1234-1234-123456789abc");
    static QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    static bool put(const QString &path, const QByteArray &bytes) {
        QFile f(path); return f.open(QIODevice::WriteOnly) && f.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
            && f.write(bytes) == bytes.size() && f.flush();
    }
    R initialize(const QTemporaryDir &d) {
        R r; r.phase = R::Phase::Clean; r.installation = id(); r.transaction = id(); r.generation = id();
        r.boot = boot; r.profile = QString(64, QLatin1Char('a')); r.invocation = id(); r.attestation = id(); r.epoch = id();
        if (!put(d.filePath(QStringLiteral("lock")), {}) || !put(d.filePath(QStringLiteral("state")), r.encode())) return {};
        return r;
    }
    auto lease(const QTemporaryDir &d) { return G::acquire(d.path(), getuid(), boot, true, true, nullptr); }
    auto record(const QTemporaryDir &d) {
        QFile f(d.filePath(QStringLiteral("state")));
        return f.open(QIODevice::ReadOnly) ? R::decode(f.readAll()) : std::nullopt;
    }
private Q_SLOTS:
    void init() { failSync = execCalls = 0; releaseCheck.clear(); releasedBeforeExec = false; }
    void nonrootRefusesBeforeGateOrExec() {
        uid_t r, e, s; QVERIFY(!getresuid(&r, &e, &s));
        if (!r && !e && !s) QSKIP("Ordinary-user test only; never exercise production root path");
        char arg[] = "guard"; char *args[] = {arg, nullptr}; QString error;
        QVERIFY(W::run(1, args, &error) != 0); QVERIFY(error.contains(QStringLiteral("root"))); QCOMPARE(execCalls, 0);
    }
    void failureDoesNotExec_data() {
        QTest::addColumn<QString>("kind");
        for (const char *s : {"missing-lease", "malformed-state", "file-sync", "directory-sync", "invalid-argv"})
            QTest::newRow(s) << QString::fromLatin1(s);
    }
    void failureDoesNotExec() {
        QFETCH(QString, kind); QTemporaryDir d; const auto before = initialize(d); QVERIFY(before.valid());
        auto l = lease(d); QVERIFY(l);
        if (kind == QStringLiteral("missing-lease")) l.reset();
        if (kind == QStringLiteral("malformed-state")) QVERIFY(put(d.filePath(QStringLiteral("state")), QByteArray("invalid\n")));
        if (kind == QStringLiteral("file-sync")) failSync = 1;
        if (kind == QStringLiteral("directory-sync")) failSync = 2;
        char arg[] = "guard"; char *args[] = {arg, nullptr};
        QVERIFY(W::runWithLease(kind == QStringLiteral("invalid-argv") ? 0 : 1, args, std::move(l), QByteArray("/no/backend"), nullptr) != 0);
        QCOMPARE(execCalls, 0); QVERIFY(lease(d));
        const auto after = record(d);
        if (kind == QStringLiteral("directory-sync")) { QVERIFY(after); QCOMPARE(after->phase, R::Phase::ExternalUnknown); }
        if (kind == QStringLiteral("file-sync")) { QVERIFY(after); QCOMPARE(*after, before); }
    }
    void failedExecLeavesBlockedAndReleasesBeforeCall() {
        QTemporaryDir d; const auto before = initialize(d); QVERIFY(before.valid());
        releaseCheck = QFile::encodeName(d.filePath(QStringLiteral("lock")));
        char arg[] = "guard"; char *args[] = {arg, nullptr};
        QVERIFY(W::runWithLease(1, args, lease(d), QFile::encodeName(d.filePath(QStringLiteral("absent-backend"))), nullptr) != 0);
        QCOMPARE(execCalls, 1); QVERIFY(releasedBeforeExec); QVERIFY(lease(d));
        const auto after = record(d); QVERIFY(after); QCOMPARE(after->phase, R::Phase::ExternalUnknown);
        QVERIFY(after->generation != before.generation); QVERIFY(after->profile.isEmpty());
    }
    void successfulBackendPreservesArgumentsEnvironmentAndRemainsBlocked() {
        QTemporaryDir d; const auto before = initialize(d); QVERIFY(before.valid());
        const QByteArray backend = QFile::encodeName(QFile::symLinkTarget(QStringLiteral("/proc/self/exe")));
        QVERIFY(!backend.isEmpty());
        const pid_t pid = fork(); QVERIFY(pid >= 0);
        if (pid == 0) {
            alarm(5);
            releaseCheck = QFile::encodeName(d.filePath(QStringLiteral("lock")));
            if (setenv("KRDP_GUARD_FIXTURE_ENV", " value with spaces=$literal; ", 1)
                || setenv("KRDP_GUARD_FIXTURE_ARGV0", backend.constData(), 1)) _exit(90);
            char original[] = "original-wrapper-name", flag[] = "--fixture-backend", empty[] = "", spaced[] = " a b ", option[] = "--option=$literal;*";
            char *args[] = {original, flag, empty, spaced, option, nullptr};
            const int rc = W::runWithLease(5, args, lease(d), backend, nullptr);
            _exit(rc ? 91 : 92);
        }
        int status; QCOMPARE(waitpid(pid, &status, 0), pid); QVERIFY(WIFEXITED(status)); QCOMPARE(WEXITSTATUS(status), 0);
        const auto after = record(d); QVERIFY(after); QCOMPARE(after->phase, R::Phase::ExternalUnknown);
        QVERIFY(after->generation != before.generation); QCOMPARE(after->installation, before.installation);
        QVERIFY(after->profile.isEmpty()); QVERIFY(after->transaction.isEmpty()); QVERIFY(lease(d));
    }
};
}
int main(int argc, char **argv) {
    // Test-only exec target, reached before Qt can interpret/rewrite arguments.
    if (argc > 1 && !strcmp(argv[1], "--fixture-backend")) {
        const char *env = getenv("KRDP_GUARD_FIXTURE_ENV"), *zero = getenv("KRDP_GUARD_FIXTURE_ARGV0");
        return argc == 5 && zero && !strcmp(argv[0], zero) && !strcmp(argv[2], "") && !strcmp(argv[3], " a b ")
            && !strcmp(argv[4], "--option=$literal;*") && env && !strcmp(env, " value with spaces=$literal; ") ? 0 : 93;
    }
    QCoreApplication app(argc, argv);
    KRdp::VirtualUnattendedUpgradeGuardTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "VirtualUnattendedUpgradeGuardTest.moc"
