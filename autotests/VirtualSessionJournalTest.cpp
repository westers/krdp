// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionJournal.h"
#include "VirtualSessionProcessIdentity.h"
#include "VirtualSessionCleanupBirth.h"
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
    void orderedAndReconciledAreIndependent() {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto ordered = record(), reconciled = record();
        for (const auto &r : {ordered, reconciled}) {
            QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        }
        QVERIFY(journal->writeOrderedExit(ordered, nullptr));
        QVERIFY(journal->writeReconciled(reconciled, nullptr));
        QCOMPARE(journal->orderedExit(ordered), std::optional<bool>(true));
        QCOMPARE(journal->reconciled(ordered), std::optional<bool>(false));
        QCOMPARE(journal->orderedExit(reconciled), std::optional<bool>(false));
        QCOMPARE(journal->reconciled(reconciled), std::optional<bool>(true));
        // Even copying intact evidence cannot change its meaning/domain.
        QVERIFY(QFile::copy(dir.filePath(QStringLiteral(".reconciled-") + reconciled.session),
            dir.filePath(QStringLiteral(".ordered-") + reconciled.session)));
        QVERIFY(!journal->orderedExit(reconciled));
        QVERIFY(QFile::copy(dir.filePath(QStringLiteral(".ordered-") + ordered.session),
            dir.filePath(QStringLiteral(".reconciled-") + ordered.session)));
        QVERIFY(!journal->reconciled(ordered));
        const auto records = journal->records(); QVERIFY(records); QCOMPARE(records->size(), 2);
    }
    void orderedExitIsSeparateDurableAndNeverReplayAuthority() {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record();
        QVERIFY(journal->insert(r));
        QCOMPARE(journal->orderedExit(r), std::optional<bool>());
        QVERIFY(!journal->writeOrderedExit(r, nullptr)); // No consumed launch.
        QVERIFY(journal->claimRecord(r, nullptr));
        const VirtualSessionJournal::Keeper birth{12345, 42, 4242};
        QVERIFY(journal->writeKeeper(r, birth, nullptr));
        QVERIFY(journal->writeKeeperClosed(r, birth, nullptr));
        QCOMPARE(journal->orderedExit(r), std::optional<bool>(false)); // PAM close alone.
        QVERIFY(journal->writeOrderedExit(r, nullptr));
        QVERIFY(!journal->writeOrderedExit(r, nullptr));
        QVERIFY(!journal->claimRecord(r, nullptr));
        QVERIFY(!journal->insert(r));
        journal.reset();
        journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        QString error = QStringLiteral("stale error");
        QCOMPARE(journal->orderedExit(r, &error), std::optional<bool>(true)); QVERIFY(error.isEmpty());
        const auto all = journal->records(); QVERIFY(all); QCOMPARE(all->size(), 1); QVERIFY(all->first() == r);
        if (getuid()) QVERIFY(!VirtualSessionJournal::recordOrderedExit(r));
    }
    void absentOrderedExitRequiresValidClaim_data() {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"missing", "malformed", "wrong-launch", "wrong-incarnation"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void absentOrderedExitRequiresValidClaim() {
        QFETCH(QString, kind); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); QVERIFY(journal->insert(r));
        if (kind != QStringLiteral("missing")) {
            QVERIFY(journal->claimRecord(r, nullptr));
            QFile claim(dir.filePath(QStringLiteral(".claimed-") + r.session));
            QVERIFY(claim.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray bytes = kind == QStringLiteral("malformed") ? QByteArray("partial")
                : (kind == QStringLiteral("wrong-launch") ? id() : r.launch).toLatin1() + '\n'
                    + (kind == QStringLiteral("wrong-incarnation") ? id() : r.incarnation).toLatin1();
            QCOMPARE(claim.write(bytes), qint64(bytes.size()));
        }
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral(".ordered-") + r.session)));
        QString error;
        QCOMPARE(journal->orderedExit(r, &error), std::optional<bool>());
        QVERIFY(!error.isEmpty());
        QVERIFY(!journal->writeOrderedExit(r, nullptr));
        QVERIFY(journal->records());
    }
    void orderedExitBindsEveryIdentityField_data() {
        QTest::addColumn<QString>("field");
        for (const auto *field : {"uid", "session", "launch", "incarnation", "boot", "token"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void orderedExitBindsEveryIdentityField() {
        QFETCH(QString, field); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); auto other = r;
        if (field == QStringLiteral("uid")) ++other.uid;
        else if (field == QStringLiteral("session")) other.session = id();
        else if (field == QStringLiteral("launch")) other.launch = id();
        else if (field == QStringLiteral("incarnation")) other.incarnation = id();
        else if (field == QStringLiteral("boot")) other.boot = id();
        else other.token.fill('t');
        QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        QVERIFY(!journal->writeOrderedExit(other, nullptr));
        QVERIFY(!journal->orderedExit(other));
        QVERIFY(journal->writeOrderedExit(r, nullptr));
        // Move valid evidence to a different, independently valid launch intent.
        // This exercises the marker binding, beyond the expected-vs-intent check.
        QTemporaryDir second;
        auto next = VirtualSessionJournal::openAt(second.path(), getuid(), nullptr); QVERIFY(next);
        QVERIFY(next->insert(other)); QVERIFY(next->claimRecord(other, nullptr));
        QVERIFY(QFile::copy(dir.filePath(QStringLiteral(".ordered-") + r.session),
            second.filePath(QStringLiteral(".ordered-") + other.session)));
        QString error;
        QVERIFY(!next->orderedExit(other, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!error.contains(QString::fromLatin1(r.token.toHex())));
    }
    void unsafeOrderedExitRefused_data() {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"empty", "truncated", "extra", "permissions", "symlink", "hardlink", "fifo", "intent-owner"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void unsafeOrderedExitRefused() {
        QFETCH(QString, kind); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        QVERIFY(journal->writeOrderedExit(r, nullptr));
        const auto path = dir.filePath(QStringLiteral(".ordered-") + r.session);
        if (kind == QStringLiteral("symlink") || kind == QStringLiteral("fifo")) {
            QVERIFY(QFile::remove(path));
            if (kind == QStringLiteral("symlink")) QVERIFY(QFile::link(QStringLiteral("/etc/passwd"), path));
            else QVERIFY(!mkfifo(QFile::encodeName(path).constData(), 0600));
        } else if (kind == QStringLiteral("hardlink")) {
            QVERIFY(!link(QFile::encodeName(path).constData(), QFile::encodeName(dir.filePath(QStringLiteral(".pending-alias"))).constData()));
        } else if (kind == QStringLiteral("permissions")) {
            QVERIFY(!chmod(QFile::encodeName(path).constData(), 0644));
        } else if (kind == QStringLiteral("intent-owner")) {
            // Changing the trust owner rejects the launch intent first; this
            // unprivileged fixture does not isolate marker-owner validation.
            ++journal->m_owner;
        } else {
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
            if (kind == QStringLiteral("extra")) { QVERIFY(file.seek(file.size())); QCOMPARE(file.write("x"), qint64(1)); }
            else QVERIFY(file.resize(kind == QStringLiteral("empty") ? 0 : file.size() - 1));
        }
        QString error;
        QVERIFY(!journal->orderedExit(r, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!journal->writeOrderedExit(r, nullptr));
        if (kind == QStringLiteral("intent-owner")) --journal->m_owner;
        QVERIFY(journal->records());
    }
    void orderedExitDurabilityFailurePreservesEvidence_data() {
        QTest::addColumn<int>("which");
        QTest::newRow("file") << 1; QTest::newRow("directory") << 2;
    }
    void orderedExitDurabilityFailurePreservesEvidence() {
        QFETCH(int, which); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        failSync = which; const auto reset = qScopeGuard([] { failSync = 0; });
        QString error;
        QVERIFY(!journal->writeOrderedExit(r, &error)); QVERIFY(!error.isEmpty());
        failSync = 0;
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral(".ordered-") + r.session)));
        QVERIFY(!journal->writeOrderedExit(r, nullptr)); QVERIFY(!journal->claimRecord(r, nullptr));
    }
    void noKeeperReconciliationRequiresPostExtinctionRead() {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        int reads = 0;
        const auto read = [&] {
            ++reads; bool missing = false;
            const auto keeper = journal->readKeeperRecord(r, &missing, nullptr);
            return VirtualSessionCleanupBirth{keeper, missing};
        };
        for (const auto empty : {std::optional<bool>(), std::optional<bool>(false)}) {
            const auto evidence = VirtualSessionCleanupBirth::inspect(read, [=] { return empty; });
            QVERIFY(!evidence.absent); QVERIFY(!evidence.keeper);
            QCOMPARE(journal->reconciled(r), std::optional<bool>(false));
        }
        reads = 0;
        const auto evidence = VirtualSessionCleanupBirth::inspect(read, [] { return std::optional<bool>(true); });
        QCOMPARE(reads, 2); QVERIFY(evidence.absent); QVERIFY(!evidence.keeper);
        QVERIFY(journal->writeReconciled(r, nullptr));
        QCOMPARE(journal->reconciled(r), std::optional<bool>(true));
        QVERIFY(!journal->claimRecord(r, nullptr));
    }
    void reconciliationIsSeparateDurableAndNeverReplayAuthority() {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record();
        QVERIFY(journal->insert(r));
        QCOMPARE(journal->reconciled(r), std::optional<bool>());
        QVERIFY(!journal->writeReconciled(r, nullptr)); // No consumed launch.
        QVERIFY(journal->claimRecord(r, nullptr));
        const VirtualSessionJournal::Keeper birth{12345, 42, 4242};
        QVERIFY(journal->writeKeeper(r, birth, nullptr));
        QVERIFY(journal->writeKeeperClosed(r, birth, nullptr));
        QCOMPARE(journal->reconciled(r), std::optional<bool>(false)); // PAM close alone.
        QVERIFY(journal->writeReconciled(r, nullptr));
        QVERIFY(!journal->writeReconciled(r, nullptr));
        QVERIFY(!journal->claimRecord(r, nullptr));
        QVERIFY(!journal->insert(r));
        journal.reset();
        journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        QString error = QStringLiteral("stale error");
        QCOMPARE(journal->reconciled(r, &error), std::optional<bool>(true)); QVERIFY(error.isEmpty());
        const auto all = journal->records(); QVERIFY(all); QCOMPARE(all->size(), 1); QVERIFY(all->first() == r);
        if (getuid()) QVERIFY(!VirtualSessionJournal::recordReconciled(r));
    }
    void absentReconciliationRequiresValidClaim_data() {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"missing", "malformed", "wrong-launch", "wrong-incarnation"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void absentReconciliationRequiresValidClaim() {
        QFETCH(QString, kind); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); QVERIFY(journal->insert(r));
        if (kind != QStringLiteral("missing")) {
            QVERIFY(journal->claimRecord(r, nullptr));
            QFile claim(dir.filePath(QStringLiteral(".claimed-") + r.session));
            QVERIFY(claim.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray bytes = kind == QStringLiteral("malformed") ? QByteArray("partial")
                : (kind == QStringLiteral("wrong-launch") ? id() : r.launch).toLatin1() + '\n'
                    + (kind == QStringLiteral("wrong-incarnation") ? id() : r.incarnation).toLatin1();
            QCOMPARE(claim.write(bytes), qint64(bytes.size()));
        }
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral(".reconciled-") + r.session)));
        QString error;
        QCOMPARE(journal->reconciled(r, &error), std::optional<bool>());
        QVERIFY(!error.isEmpty());
        QVERIFY(!journal->writeReconciled(r, nullptr));
        QVERIFY(journal->records());
    }
    void reconciliationBindsEveryIdentityField_data() {
        QTest::addColumn<QString>("field");
        for (const auto *field : {"uid", "session", "launch", "incarnation", "boot", "token"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void reconciliationBindsEveryIdentityField() {
        QFETCH(QString, field); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); auto other = r;
        if (field == QStringLiteral("uid")) ++other.uid;
        else if (field == QStringLiteral("session")) other.session = id();
        else if (field == QStringLiteral("launch")) other.launch = id();
        else if (field == QStringLiteral("incarnation")) other.incarnation = id();
        else if (field == QStringLiteral("boot")) other.boot = id();
        else other.token.fill('t');
        QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        QVERIFY(!journal->writeReconciled(other, nullptr));
        QVERIFY(!journal->reconciled(other));
        QVERIFY(journal->writeReconciled(r, nullptr));
        // Move valid evidence to a different, independently valid launch intent.
        // This exercises the marker binding, beyond the expected-vs-intent check.
        QTemporaryDir second;
        auto next = VirtualSessionJournal::openAt(second.path(), getuid(), nullptr); QVERIFY(next);
        QVERIFY(next->insert(other)); QVERIFY(next->claimRecord(other, nullptr));
        QVERIFY(QFile::copy(dir.filePath(QStringLiteral(".reconciled-") + r.session),
            second.filePath(QStringLiteral(".reconciled-") + other.session)));
        QString error;
        QVERIFY(!next->reconciled(other, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!error.contains(QString::fromLatin1(r.token.toHex())));
    }
    void unsafeReconciliationRefused_data() {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"empty", "truncated", "extra", "permissions", "symlink", "hardlink", "fifo", "intent-owner"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void unsafeReconciliationRefused() {
        QFETCH(QString, kind); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        QVERIFY(journal->writeReconciled(r, nullptr));
        const auto path = dir.filePath(QStringLiteral(".reconciled-") + r.session);
        if (kind == QStringLiteral("symlink") || kind == QStringLiteral("fifo")) {
            QVERIFY(QFile::remove(path));
            if (kind == QStringLiteral("symlink")) QVERIFY(QFile::link(QStringLiteral("/etc/passwd"), path));
            else QVERIFY(!mkfifo(QFile::encodeName(path).constData(), 0600));
        } else if (kind == QStringLiteral("hardlink")) {
            QVERIFY(!link(QFile::encodeName(path).constData(), QFile::encodeName(dir.filePath(QStringLiteral(".pending-alias"))).constData()));
        } else if (kind == QStringLiteral("permissions")) {
            QVERIFY(!chmod(QFile::encodeName(path).constData(), 0644));
        } else if (kind == QStringLiteral("intent-owner")) {
            // Changing the trust owner rejects the launch intent first; this
            // unprivileged fixture does not isolate marker-owner validation.
            ++journal->m_owner;
        } else {
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
            if (kind == QStringLiteral("extra")) { QVERIFY(file.seek(file.size())); QCOMPARE(file.write("x"), qint64(1)); }
            else QVERIFY(file.resize(kind == QStringLiteral("empty") ? 0 : file.size() - 1));
        }
        QString error;
        QVERIFY(!journal->reconciled(r, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!journal->writeReconciled(r, nullptr));
        if (kind == QStringLiteral("intent-owner")) --journal->m_owner;
        QVERIFY(journal->records());
    }
    void reconciliationDurabilityFailurePreservesEvidence_data() {
        QTest::addColumn<int>("which");
        QTest::newRow("file") << 1; QTest::newRow("directory") << 2;
    }
    void reconciliationDurabilityFailurePreservesEvidence() {
        QFETCH(int, which); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        failSync = which; const auto reset = qScopeGuard([] { failSync = 0; });
        QString error;
        QVERIFY(!journal->writeReconciled(r, &error)); QVERIFY(!error.isEmpty());
        failSync = 0;
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral(".reconciled-") + r.session)));
        QVERIFY(!journal->writeReconciled(r, nullptr)); QVERIFY(!journal->claimRecord(r, nullptr));
    }
    void cleanupRereadsBirthAfterServiceExtinction() {
        std::optional<VirtualSessionJournal::Keeper> published;
        int reads = 0;
        const auto read = [&] { ++reads; return VirtualSessionCleanupBirth{published, !published}; };
        const VirtualSessionJournal::Keeper birth{12345, 42, 4242};
        const auto result = VirtualSessionCleanupBirth::inspect(read, [&]() -> std::optional<bool> {
            // Deterministic missing→publication/migration→empty race.
            published = birth; return true;
        });
        QCOMPARE(reads, 2); QVERIFY(result.keeper); QVERIFY(!result.absent); QVERIFY(*result.keeper == birth);
        published.reset(); reads = 0;
        const auto missing = VirtualSessionCleanupBirth::inspect(read, [] { return std::optional<bool>(true); });
        QCOMPARE(reads, 2); QVERIFY(!missing.keeper); QVERIFY(missing.absent);
        for (const auto empty : {std::optional<bool>(), std::optional<bool>(false)}) {
            reads = 0;
            const auto uncertain = VirtualSessionCleanupBirth::inspect(read, [=] { return empty; });
            QCOMPARE(reads, 1); QVERIFY(!uncertain.keeper); QVERIFY(!uncertain.absent);
        }
        reads = 0;
        const auto partial = VirtualSessionCleanupBirth::inspect([&] {
            return ++reads == 1 ? VirtualSessionCleanupBirth{{}, true} : VirtualSessionCleanupBirth{};
        }, [] { return std::optional<bool>(true); });
        QVERIFY(!partial.keeper); QVERIFY(!partial.absent); QCOMPARE(reads, 2);
    }
    void successfulCloseEvidenceIsBoundToOriginalKeeper() {
        QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); const VirtualSessionJournal::Keeper birth{12345, 42, 4242};
        QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        QVERIFY(!journal->writeKeeperClosed(r, birth, nullptr));
        QVERIFY(!journal->readKeeperClosed(r, birth, nullptr));
        QVERIFY(journal->writeKeeper(r, birth, nullptr));
        QCOMPARE(journal->readKeeperClosed(r, birth, nullptr), std::optional<bool>(false));
        auto other = birth; ++other.pidInode;
        QVERIFY(!journal->writeKeeperClosed(r, other, nullptr));
        QVERIFY(journal->writeKeeperClosed(r, birth, nullptr));
        QVERIFY(!journal->writeKeeperClosed(r, birth, nullptr));
        QCOMPARE(journal->readKeeperClosed(r, birth, nullptr), std::optional<bool>(true));
        QVERIFY(!journal->readKeeperClosed(r, other, nullptr));
        const auto all = journal->records(); QVERIFY(all); QCOMPARE(all->size(), 1);
        if (getuid()) {
            QVERIFY(!VirtualSessionJournal::recordKeeperClosed(r));
            QVERIFY(!VirtualSessionJournal::keeperClosed(r, birth));
        }
    }
    void unsafeCloseEvidenceRefused_data() {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"empty", "identity", "permissions", "symlink", "hardlink", "fifo"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void unsafeCloseEvidenceRefused() {
        QFETCH(QString, kind); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); const VirtualSessionJournal::Keeper birth{12345, 42, 4242};
        QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr));
        QVERIFY(journal->writeKeeper(r, birth, nullptr)); QVERIFY(journal->writeKeeperClosed(r, birth, nullptr));
        const auto path = dir.filePath(QStringLiteral(".closed-") + r.session);
        if (kind == QStringLiteral("symlink") || kind == QStringLiteral("fifo")) {
            QVERIFY(QFile::remove(path));
            if (kind == QStringLiteral("symlink")) QVERIFY(QFile::link(QStringLiteral("/etc/passwd"), path));
            else QVERIFY(!mkfifo(QFile::encodeName(path).constData(), 0600));
        } else if (kind == QStringLiteral("hardlink")) {
            QVERIFY(!link(QFile::encodeName(path).constData(), QFile::encodeName(dir.filePath(QStringLiteral(".pending-alias"))).constData()));
        } else if (kind == QStringLiteral("permissions")) {
            QVERIFY(!chmod(QFile::encodeName(path).constData(), 0644));
        } else {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            if (kind == QStringLiteral("identity")) QVERIFY(file.write("wrong generation") > 0);
            file.close();
        }
        QVERIFY(!journal->readKeeperClosed(r, birth, nullptr));
        QVERIFY(!journal->writeKeeperClosed(r, birth, nullptr));
        QVERIFY(journal->records());
    }
    void closeEvidenceDurabilityFailureIsReported_data() {
        QTest::addColumn<int>("which");
        QTest::newRow("file") << 1; QTest::newRow("directory") << 2;
    }
    void closeEvidenceDurabilityFailureIsReported() {
        QFETCH(int, which); QTemporaryDir dir;
        auto journal = VirtualSessionJournal::openAt(dir.path(), getuid(), nullptr); QVERIFY(journal);
        const auto r = record(); const VirtualSessionJournal::Keeper birth{12345, 42, 4242};
        QVERIFY(journal->insert(r)); QVERIFY(journal->claimRecord(r, nullptr)); QVERIFY(journal->writeKeeper(r, birth, nullptr));
        failSync = which; const auto reset = qScopeGuard([] { failSync = 0; });
        QVERIFY(!journal->writeKeeperClosed(r, birth, nullptr));
        failSync = 0;
        QVERIFY(!journal->writeKeeperClosed(r, birth, nullptr));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral(".closed-") + r.session)));
    }
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
