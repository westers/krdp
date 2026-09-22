// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionProvisioningReceipt.h"
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

static int syncFailure = 0;
static std::function<void()> onSync;
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (onSync) { auto callback = std::move(onSync); onSync = {}; callback(); }
    if (syncFailure > 0 && --syncFailure == 0) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
namespace KRdp {
class VirtualSessionProvisioningReceiptTest : public QObject {
    Q_OBJECT
    using R = VirtualSessionMaintenanceRecord;
    static QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    static R initial() {
        R r; r.phase = R::Phase::InitialBlocked;
        r.installation = id(); r.transaction = id(); r.generation = id();
        r.boot = id(); r.profile = QString(64, QLatin1Char('a')); r.attestation = id();
        return r;
    }
    static QByteArray bytes(const R &r) {
        QByteArray b("KRDP-FIRST-PROVISIONING-1\n");
        for (const auto &s : {r.installation, r.transaction, r.generation, r.boot, r.profile, r.attestation}) b += s.toLatin1() + '\n';
        return b;
    }
    static QString name(const QTemporaryDir &d) { return d.filePath(QStringLiteral("first-provisioning-receipt")); }
    static bool put(const QString &path, const QByteArray &b) {
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
            && f.write(b) == b.size() && f.flush();
    }
    static bool check(const QTemporaryDir &d, const R &r, const QString &boot = {}, const QString &profile = {}) {
        return VirtualSessionProvisioningReceipt::validateAt(d.path(), getuid(), true,
            boot.isEmpty() ? r.boot : boot, profile.isEmpty() ? r.profile : profile, r, nullptr);
    }
private Q_SLOTS:
    void validReceiptDoesNotCreateState() {
        QTemporaryDir d; const auto r = initial(); QVERIFY(put(name(d), bytes(r)));
        QVERIFY(check(d, r)); QVERIFY(check(d, r));
        QVERIFY(!QFile::exists(d.filePath(QStringLiteral("state"))));
        QFile f(name(d)); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), bytes(r));
    }
    void mismatchedBinding_data() {
        QTest::addColumn<int>("field");
        for (int i = 0; i < 6; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void mismatchedBinding() {
        QFETCH(int, field); QTemporaryDir d; auto r = initial(); QVERIFY(put(name(d), bytes(r)));
        QString *fields[] = {&r.installation, &r.transaction, &r.generation, &r.boot, &r.profile, &r.attestation};
        *fields[field] = field == 4 ? QString(64, QLatin1Char('b')) : id();
        QVERIFY(!check(d, r)); QVERIFY(QFile::exists(name(d)));
    }
    void unsafe_data() {
        QTest::addColumn<QString>("kind");
        for (const char *s : {"missing", "mode", "hardlink", "symlink", "truncated", "oversize", "extra", "domain", "directory", "directory-mode", "invalid-uuid", "nonascii"})
            QTest::newRow(s) << QString::fromLatin1(s);
    }
    void unsafe() {
        QFETCH(QString, kind); QTemporaryDir d; auto r = initial(); auto b = bytes(r);
        if (kind == QStringLiteral("missing")) { QVERIFY(!check(d, r)); return; }
        if (kind == QStringLiteral("truncated")) b.chop(1);
        if (kind == QStringLiteral("oversize")) b += QByteArray(513, 'x');
        if (kind == QStringLiteral("extra")) b += '\n';
        if (kind == QStringLiteral("domain")) b = r.encode();
        if (kind == QStringLiteral("invalid-uuid")) b.replace(r.installation.toLatin1(), QByteArray(r.installation.toUpper().toLatin1() + ' '));
        if (kind == QStringLiteral("nonascii")) b[30] = char(0xff);
        const auto path = QFile::encodeName(name(d));
        if (kind == QStringLiteral("directory")) QVERIFY(!mkdir(path.constData(), 0600));
        else QVERIFY(put(name(d), b));
        if (kind == QStringLiteral("mode")) QVERIFY(!chmod(path.constData(), 0640));
        if (kind == QStringLiteral("directory-mode")) QVERIFY(!chmod(QFile::encodeName(d.path()).constData(), 0755));
        const auto alias = QFile::encodeName(d.filePath(QStringLiteral("alias")));
        if (kind == QStringLiteral("hardlink")) QVERIFY(!link(path.constData(), alias.constData()));
        if (kind == QStringLiteral("symlink")) {
            QVERIFY(QFile::rename(name(d), QString::fromLocal8Bit(alias)));
            QVERIFY(!symlink("alias", path.constData()));
        }
        QVERIFY(!check(d, r));
        struct stat st{}; QVERIFY(!lstat(path.constData(), &st)); // Never removed on refusal.
    }
    void consumedUnknownAndWrongContextRefuse() {
        QTemporaryDir d; const auto r = initial(); QVERIFY(put(name(d), bytes(r)));
        QVERIFY(!check(d, *r.claim(r.boot, id())));
        QVERIFY(!check(d, *r.invalidate(r.boot)));
        QVERIFY(!check(d, r, id()));
        QVERIFY(!check(d, r, r.boot, QString(64, QLatin1Char('b'))));
        QVERIFY(!VirtualSessionProvisioningReceipt::validateAt(d.path(), getuid() + 1, true, r.boot, r.profile, r, nullptr));
    }
    void directoryAliasRefuses() {
        QTemporaryDir d, links; const auto r = initial(); QVERIFY(put(name(d), bytes(r)));
        const auto alias = links.filePath(QStringLiteral("alias"));
        QVERIFY(!symlink(QFile::encodeName(d.path()).constData(), QFile::encodeName(alias).constData()));
        QVERIFY(!VirtualSessionProvisioningReceipt::validateAt(alias, getuid(), true, r.boot, r.profile, r, nullptr));
    }
    void replacementDuringSyncRefuses() {
        QTemporaryDir d; const auto r = initial(); QVERIFY(put(name(d), bytes(r)));
        bool replaced = false;
        onSync = [&] {
            replaced = QFile::rename(name(d), d.filePath(QStringLiteral("old-receipt"))) && put(name(d), bytes(r));
        };
        QVERIFY(!check(d, r)); QVERIFY(replaced);
        QVERIFY(QFile::exists(name(d))); QVERIFY(QFile::exists(d.filePath(QStringLiteral("old-receipt"))));
    }
    void durabilityFailure_data() { QTest::addColumn<int>("nth"); QTest::newRow("file") << 1; QTest::newRow("directory") << 2; }
    void durabilityFailure() {
        QFETCH(int, nth); QTemporaryDir d; const auto r = initial(); QVERIFY(put(name(d), bytes(r)));
        syncFailure = nth; QVERIFY(!check(d, r)); QCOMPARE(syncFailure, 0);
        QFile f(name(d)); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), bytes(r));
        QVERIFY(check(d, r)); // Retry must establish durability again.
        syncFailure = nth; QVERIFY(!check(d, r)); QCOMPARE(syncFailure, 0);
    }
};
}
QTEST_GUILESS_MAIN(KRdp::VirtualSessionProvisioningReceiptTest)
#include "VirtualSessionProvisioningReceiptTest.moc"
