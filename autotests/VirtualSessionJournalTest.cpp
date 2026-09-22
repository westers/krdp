// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionJournal.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QUuid>
#include <sys/stat.h>
#include <unistd.h>

namespace KRdp {
class VirtualSessionJournalTest : public QObject {
    Q_OBJECT
    static QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    static VirtualSessionJournal::Record record() { return {1000, id(), id(), id(), id(), QByteArray(32, 's')}; }
private Q_SLOTS:
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
