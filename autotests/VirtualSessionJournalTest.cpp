// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionJournal.h"
#include "VirtualSessionProcessIdentity.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QUuid>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <limits>
#include <sys/syscall.h>
#include <unistd.h>

namespace { int failSync = 0; }
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
    if (failSync > 0 && --failSync == 0) { errno = EIO; return -1; }
    return __real_fsync(fd);
}

namespace KRdp {
class VirtualSessionJournalTest : public QObject {
    Q_OBJECT
    static QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    static VirtualSessionJournal::Record record() { return {1000, id(), id(), id(), id(), QByteArray(32, 's')}; }
private Q_SLOTS:
    void failedKeeperDurabilityForbidsPamAndReplay_data() {
        QTest::addColumn<int>("which");
        QTest::newRow("file fsync") << 1;
        QTest::newRow("directory fsync") << 2;
    }
    void failedKeeperDurabilityForbidsPamAndReplay() {
        QFETCH(int, which);
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        failSync = which;
        const auto reset = qScopeGuard([] { failSync = 0; });
        QString error;
        QVERIFY(!journal->writeKeeper(r, {12345, 42, 4242}, &error));
        QVERIFY(error.contains(QStringLiteral("PAM must not open")));
        failSync = 0;
        QVERIFY(!journal->writeKeeper(r, {12345, 42, 4242}, nullptr));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral(".keeper-") + r.session)));
        bool missing = true;
        journal->readKeeperRecord(r, &missing, nullptr);
        QVERIFY(!missing); QVERIFY(journal->records());
    }
    void keeperBirthIsDurableImmutableAndSeparate() {
        QTemporaryDir dir;
        auto writer = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr);
        QVERIFY(writer);
        const auto r = record(); QVERIFY(writer->insert(r));
        const VirtualSessionJournal::Keeper birth{12345, std::numeric_limits<quint64>::max(), std::numeric_limits<quint64>::max()};
        QVERIFY(!writer->writeKeeper(r, birth, nullptr)); // Must have consumed launch.
        QVERIFY(writer->claimRecord(r, nullptr));
        bool missing = false;
        QVERIFY(!writer->readKeeperRecord(r, &missing, nullptr)); QVERIFY(missing);
        QVERIFY(!writer->writeKeeper(r, {1, 42}, nullptr));
        QVERIFY(!writer->writeKeeper(r, {12345, 0}, nullptr));
        QVERIFY(writer->writeKeeper(r, birth, nullptr));
        QVERIFY(!writer->writeKeeper(r, birth, nullptr));
        auto reader = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr, false); QVERIFY(reader);
        const auto loaded = reader->readKeeperRecord(r, &missing, nullptr);
        QVERIFY(loaded); QVERIFY(!missing); QVERIFY(*loaded == birth);
        const auto recovered = writer->records(); QVERIFY(recovered); QCOMPARE(recovered->size(), 1);
        auto stale = r; stale.launch = id();
        QVERIFY(!reader->readKeeperRecord(stale, &missing, nullptr)); QVERIFY(!missing);
        QVERIFY(!reader->writeKeeper(stale, birth, nullptr));
        QVERIFY(QFile::remove(dir.filePath(QStringLiteral(".claimed-") + r.session)));
        QVERIFY(!reader->readKeeperRecord(r, &missing, nullptr)); QVERIFY(!missing);
        if (getuid()) {
            QVERIFY(!VirtualSessionJournal::recordKeeper(r));
            QVERIFY(!VirtualSessionJournal::readKeeper(r, &missing)); QVERIFY(!missing);
        }
    }
    void unsafeKeeperRecordIsNeverAnAbsentKeeper_data() {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"empty", "malformed", "symlink", "hardlink", "fifo", "permissions", "launch", "boot", "uid", "pid", "start", "numeric start", "pidInode"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void unsafeKeeperRecordIsNeverAnAbsentKeeper() {
        QFETCH(QString, kind);
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        QVERIFY(journal->writeKeeper(r, {12345, 42, 4242}, nullptr));
        const auto path = dir.filePath(QStringLiteral(".keeper-") + r.session);
        if (kind == QStringLiteral("symlink") || kind == QStringLiteral("fifo")) {
            QVERIFY(QFile::remove(path));
            if (kind == QStringLiteral("symlink")) QVERIFY(QFile::link(QStringLiteral("/etc/passwd"), path));
            else QVERIFY(!mkfifo(QFile::encodeName(path).constData(), 0600));
        } else if (kind == QStringLiteral("hardlink")) {
            QVERIFY(!link(QFile::encodeName(path).constData(), QFile::encodeName(dir.filePath(QStringLiteral(".pending-alias"))).constData()));
        } else if (kind == QStringLiteral("permissions")) {
            QVERIFY(!chmod(QFile::encodeName(path).constData(), 0644));
        } else {
            QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
            auto object = QJsonDocument::fromJson(file.readAll()).object(); file.close();
            if (kind == QStringLiteral("launch") || kind == QStringLiteral("boot")) object[kind] = id();
            else if (kind == QStringLiteral("uid")) object[kind] = 1001;
            else if (kind == QStringLiteral("pid")) object[kind] = QStringLiteral("12345");
            else if (kind == QStringLiteral("start")) object[kind] = QStringLiteral("042");
            else if (kind == QStringLiteral("numeric start")) object[QStringLiteral("start")] = 42;
            else if (kind == QStringLiteral("pidInode")) object[kind] = QStringLiteral("1");
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            if (kind != QStringLiteral("empty")) {
                const auto bytes = kind == QStringLiteral("malformed") ? QByteArray("{") : QJsonDocument(object).toJson(QJsonDocument::Compact);
                QCOMPARE(file.write(bytes), qint64(bytes.size()));
            }
            file.close();
        }
        bool missing = true;
        QVERIFY(!journal->readKeeperRecord(r, &missing, nullptr)); QVERIFY(!missing);
        QVERIFY(!journal->writeKeeper(r, {12345, 42, 4242}, nullptr));
        QVERIFY(journal->records()); // Still recover unrelated desktops.
    }
    void processBirthHandlesCommDelimiters() {
        const auto before = virtualProcessStartTime(getpid()); QVERIFY(before); QVERIFY(*before > 0);
        char name[16]{}; QVERIFY(!prctl(PR_GET_NAME, name));
        const auto restore = qScopeGuard([&] { prctl(PR_SET_NAME, name); });
        QVERIFY(!prctl(PR_SET_NAME, "a) b (\nc) d"));
        QCOMPARE(virtualProcessStartTime(getpid()), before);
        QVERIFY(!virtualProcessStartTime(0)); QVERIFY(!virtualProcessStartTime(1));
        const int fd = int(syscall(SYS_pidfd_open, getpid(), 0)); QVERIFY(fd >= 0);
        const auto identity = virtualPidfdIdentity(fd); close(fd); QVERIFY(identity);
        const int again = int(syscall(SYS_pidfd_open, getpid(), 0)); QVERIFY(again >= 0);
        QCOMPARE(virtualPidfdIdentity(again), identity); close(again);
        QVERIFY(!virtualPidfdIdentity(-1));
    }
    void consumedIntentCannotReplayAfterRuntimeDisappears() {
        QTemporaryDir dir, runtime;
        auto writer = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr);
        QVERIFY(writer);
        const auto r = record(); QVERIFY(writer->insert(r));
        auto entry = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr, false);
        QVERIFY(entry);
        auto different = r; different.incarnation = id();
        QVERIFY(!entry->claimRecord(different, nullptr));
        QVERIFY(entry->claimRecord(r, nullptr));
        QVERIFY(!entry->claimRecord(r, nullptr));
        QVERIFY(QDir(runtime.path()).removeRecursively());
        entry.reset();
        entry = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr, false);
        QVERIFY(entry); QVERIFY(!entry->claimRecord(r, nullptr));
        const auto records = writer->records();
        QVERIFY(records); QCOMPARE(records->size(), 1); QVERIFY(records->first() == r);
        if (getuid()) QVERIFY(!VirtualSessionJournal::claimLaunch(r));
    }
    void interruptedClaimIsNotRelaunchAuthority() {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr);
        QVERIFY(journal);
        const auto r = record(); QVERIFY(journal->insert(r));
        QFile marker(dir.filePath(QStringLiteral(".claimed-") + r.session));
        QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)); marker.close();
        QVERIFY(!journal->claimRecord(r, nullptr));
        QVERIFY(journal->records());
    }
    void independentReaderDoesNotTakeOrReleaseBrokerLease() {
        QTemporaryDir dir;
        QString error;
        auto writer = VirtualSessionJournal::openAt(dir.path(), getuid(), &error);
        QVERIFY(writer);
        const auto r = record();
        QVERIFY(writer->insert(r));
        {
            auto reader = VirtualSessionJournal::openAt(dir.path(), getuid(), &error, false);
            QVERIFY2(reader, qPrintable(error));
            QVERIFY(!reader->insert(record(), &error));
            QVERIFY(!VirtualSessionJournal::openAt(dir.path(), getuid(), &error));
            const auto intent = reader->readRecord(r.session, &error);
            QVERIFY2(intent, qPrintable(error));
            QCOMPARE(intent->token, r.token); QCOMPARE(intent->launch, r.launch);
            QVERIFY(!reader->readRecord(QStringLiteral("../outside"), &error));
            QVERIFY(!reader->readRecord(id(), &error));
            if (getuid()) QVERIFY(!VirtualSessionJournal::readLaunchIntent(r.session, &error));
        }
        QVERIFY(!VirtualSessionJournal::openAt(dir.path(), getuid(), &error));
        QVERIFY(writer->insert(record()));
        writer.reset();
        QVERIFY(VirtualSessionJournal::openAt(dir.path(), getuid(), &error));
    }
    void restartReadsExactIntentAndCannotReplace() {
        QTemporaryDir dir;
        QString error;
        const auto r = record();
        {
            auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), &error);
            QVERIFY2(journal, qPrintable(error));
            QVERIFY(!VirtualSessionJournal::openAt(dir.path(), getuid(), &error));
            QVERIFY(journal->insert(r, &error));
            auto replacement = r; replacement.incarnation = id(); replacement.token.fill('z');
            QVERIFY(!journal->insert(replacement, &error));
        }
        auto next = VirtualSessionJournal::openAt(dir.path(), getuid(), &error);
        QVERIFY(next);
        for (int i = 0; i < 2; ++i) {
            const auto all = next->records(&error);
            QVERIFY2(all, qPrintable(error));
            QCOMPARE(all->size(), 1);
            const auto &loaded = all->first();
            QCOMPARE(loaded.uid, r.uid); QCOMPARE(loaded.session, r.session);
            QCOMPARE(loaded.incarnation, r.incarnation); QCOMPARE(loaded.boot, r.boot);
            QCOMPARE(loaded.token, r.token); QCOMPARE(loaded.launch, r.launch);
            QCOMPARE(loaded.identity().socket, QStringLiteral("/run/user/1000/krdp-virtual/%1/guardian.sock").arg(r.launch));
            QCOMPARE(loaded.workerSocket(), QStringLiteral("/run/user/1000/krdp-virtual/%1/worker.sock").arg(r.launch));
        }
    }
    void refusesUnsafeDirectory() {
        QTemporaryDir dir;
        QString error;
        QVERIFY(!VirtualSessionJournal::openAt(dir.path(), getuid() + 1, &error));
        QVERIFY(!chmod(QFile::encodeName(dir.path()).constData(), 0755));
        QVERIFY(!VirtualSessionJournal::openAt(dir.path(), getuid(), &error));
        QVERIFY(!chmod(QFile::encodeName(dir.path()).constData(), 0700));
        const QString link = dir.path() + QStringLiteral("/alias");
        QVERIFY(QFile::link(dir.path(), link));
        QVERIFY(!VirtualSessionJournal::openAt(link, getuid(), &error));
        if (getuid()) QVERIFY(!VirtualSessionJournal::open(&error));
    }
    void refusesUnsafeRecords_data() {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"symlink", "hardlink", "permissions", "fifo", "oversize", "malformed", "wrong-session"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void refusesUnsafeRecords() {
        QFETCH(QString, kind);
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr);
        QVERIFY(journal);
        const auto r = record();
        const auto path = dir.filePath(r.session + QStringLiteral(".json"));
        QVERIFY(journal->insert(r));
        if (kind == QStringLiteral("symlink") || kind == QStringLiteral("fifo")) {
            QVERIFY(QFile::remove(path));
            if (kind == QStringLiteral("symlink")) QVERIFY(QFile::link(QStringLiteral("/etc/passwd"), path));
            else QVERIFY(!mkfifo(QFile::encodeName(path).constData(), 0600));
        } else if (kind == QStringLiteral("hardlink")) {
            QVERIFY(!::link(QFile::encodeName(path).constData(), QFile::encodeName(dir.filePath(QStringLiteral(".pending-alias"))).constData()));
        } else if (kind == QStringLiteral("permissions")) {
            QVERIFY(!chmod(QFile::encodeName(path).constData(), 0644));
        } else if (kind == QStringLiteral("wrong-session")) {
            QVERIFY(QFile::rename(path, dir.filePath(id() + QStringLiteral(".json"))));
        } else {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QVERIFY(file.write(kind == QStringLiteral("oversize") ? QByteArray(4097, 'x') : QByteArray("{}")) > 0);
            file.close();
        }
        QString error;
        QVERIFY(!journal->records(&error));
        auto reader = VirtualSessionJournal::openAt(dir.path(), getuid(), &error, false);
        QVERIFY(reader);
        QVERIFY(!reader->readRecord(r.session, &error));
        QVERIFY(!error.contains(QString::fromLatin1(r.token.toHex())));
    }
    void invalidIntentAndInterruptedTemporary() {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr);
        QVERIFY(journal);
        auto r = record(); r.uid = 0;
        QVERIFY(!journal->insert(r));
        r = record(); r.token.resize(31);
        QVERIFY(!journal->insert(r));
        r = record(); r.boot = QStringLiteral("not-a-boot-uuid");
        QVERIFY(!journal->insert(r));
        QFile pending(dir.filePath(QStringLiteral(".pending-crash")));
        QVERIFY(pending.open(QIODevice::WriteOnly)); pending.write("partial"); pending.close();
        const auto result = journal->records();
        QVERIFY(result); QVERIFY(result->isEmpty());
    }
    void capacityIsEnforcedBeforePublishing() {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr);
        QVERIFY(journal);
        for (int i = 0; i < 256; ++i) QVERIFY(journal->insert(record()));
        QVERIFY(!journal->insert(record()));
        const auto result = journal->records();
        QVERIFY(result); QCOMPARE(result->size(), 256);
    }
    void restrictiveUmaskStillPublishesReadableRecord() {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr);
        QVERIFY(journal);
        struct RestoreMask { mode_t old; ~RestoreMask() { umask(old); } } restore{umask(0277)};
        const auto r = record();
        QVERIFY(journal->insert(r));
        const auto result = journal->records();
        QVERIFY(result); QCOMPARE(result->size(), 1);
        QCOMPARE(result->first().token, r.token);
    }
};
}
QTEST_GUILESS_MAIN(KRdp::VirtualSessionJournalTest)
#include "VirtualSessionJournalTest.moc"
