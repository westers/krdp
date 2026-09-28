// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-K3: generation and renewal decisions for the server's own certificate,
// with an injected clock.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include "ServerCertificate.h"

#include <sys/stat.h>
#include <unistd.h>

#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>

using namespace Qt::StringLiterals;

using namespace KRdp::ServerCertificate;

namespace
{
const QDateTime kNow = QDateTime(QDate(2026, 9, 27), QTime(12, 0), QTimeZone::UTC);

Info validInfo(const QDateTime &notAfter)
{
    Info info;
    info.certificateExists = true;
    info.keyExists = true;
    info.certificateReadable = true;
    info.keyReadable = true;
    info.keyMatches = true;
    info.notAfter = notAfter;
    return info;
}
}

class ServerCertificateTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void decisions()
    {
        QCOMPARE(decide(Info{}, kNow), Decision::GenerateMissing);

        auto certOnly = validInfo(kNow.addYears(5));
        certOnly.keyExists = false;
        certOnly.keyReadable = false;
        certOnly.keyMatches = false;
        QCOMPARE(decide(certOnly, kNow), Decision::GenerateMissing);

        auto unreadableKey = validInfo(kNow.addYears(5));
        unreadableKey.keyReadable = false;
        unreadableKey.keyMatches = false;
        QCOMPARE(decide(unreadableKey, kNow), Decision::GenerateUnusable);

        auto mismatched = validInfo(kNow.addYears(5));
        mismatched.keyMatches = false;
        QCOMPARE(decide(mismatched, kNow), Decision::GenerateUnusable);

        QCOMPARE(decide(validInfo(kNow.addSecs(-1)), kNow), Decision::GenerateExpired);
        QCOMPARE(decide(validInfo(kNow), kNow), Decision::GenerateExpired);
        QCOMPARE(decide(validInfo(kNow.addDays(1)), kNow), Decision::GenerateExpiringSoon);
        QCOMPARE(decide(validInfo(kNow.addDays(kRenewBeforeDays)), kNow), Decision::GenerateExpiringSoon);
        QCOMPARE(decide(validInfo(kNow.addDays(kRenewBeforeDays).addSecs(1)), kNow), Decision::UseExisting);
        QCOMPARE(decide(validInfo(kNow.addYears(10)), kNow), Decision::UseExisting);
    }

    void generatesTenYearEcdsa()
    {
        QTemporaryDir dir;
        const Paths paths{dir.filePath(u"sub/krdp.crt"_s), dir.filePath(u"sub/krdp.key"_s)};
        QString error;
        QVERIFY2(generate(paths, u"hal9000"_s, kNow, kValidityDays, &error), qPrintable(error));

        const auto info = inspect(paths);
        QVERIFY(info.usable());
        QCOMPARE(info.algorithm, u"ECDSA P-256"_s);
        QCOMPARE(info.notAfter, kNow.addDays(kValidityDays));
        QVERIFY(info.notBefore <= kNow);
        QVERIFY(QRegularExpression(u"^([0-9A-F]{2}:){31}[0-9A-F]{2}$"_s).match(info.sha256Fingerprint).hasMatch());
        QCOMPARE(QFile(paths.key).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther),
                 QFileDevice::Permissions());

        // Fresh: kept. Close to the end, or after it: replaced.
        QCOMPARE(decide(info, kNow), Decision::UseExisting);
        QCOMPARE(decide(info, kNow.addDays(kValidityDays - kRenewBeforeDays - 1)), Decision::UseExisting);
        QCOMPARE(decide(info, kNow.addDays(kValidityDays - kRenewBeforeDays + 1)), Decision::GenerateExpiringSoon);
        QCOMPARE(decide(info, kNow.addDays(kValidityDays + 1)), Decision::GenerateExpired);
    }

    void ensureKeepsValidAndRenewsExpiring()
    {
        QTemporaryDir dir;
        const Paths paths{dir.filePath(u"krdp.crt"_s), dir.filePath(u"krdp.key"_s)};

        auto first = ensure(paths, u"host"_s, kNow);
        QVERIFY2(first.ok, qPrintable(first.error));
        QVERIFY(first.generated);
        QCOMPARE(first.decision, Decision::GenerateMissing);

        // A day later: same certificate.
        auto again = ensure(paths, u"host"_s, kNow.addDays(1));
        QVERIFY(again.ok);
        QVERIFY(!again.generated);
        QCOMPARE(again.info.sha256Fingerprint, first.info.sha256Fingerprint);

        // 20 days before it ends: a new one.
        const auto later = kNow.addDays(kValidityDays - 20);
        auto renewed = ensure(paths, u"host"_s, later);
        QVERIFY(renewed.ok);
        QVERIFY(renewed.generated);
        QCOMPARE(renewed.decision, Decision::GenerateExpiringSoon);
        QVERIFY(renewed.info.sha256Fingerprint != first.info.sha256Fingerprint);
        QCOMPARE(renewed.info.notAfter, later.addDays(kValidityDays));
    }

    void ensureReplacesGarbageAndOldOneDayCertificate()
    {
        QTemporaryDir dir;
        const Paths paths{dir.filePath(u"krdp.crt"_s), dir.filePath(u"krdp.key"_s)};
        for (const auto &path : {paths.certificate, paths.key}) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("-----BEGIN CERTIFICATE-----\nnot base64\n-----END CERTIFICATE-----\n");
        }
        auto result = ensure(paths, u"host"_s, kNow);
        QVERIFY(result.ok);
        QCOMPARE(result.decision, Decision::GenerateUnusable);

        // What the old KCM wrote: `openssl req -days 1`, long expired.
        QString error;
        QVERIFY(generate(paths, u"host"_s, kNow.addDays(-400), 1, &error));
        result = ensure(paths, u"host"_s, kNow);
        QVERIFY(result.ok);
        QCOMPARE(result.decision, Decision::GenerateExpired);
        QVERIFY(result.generated);
    }

    void freerdpLoadsTheGeneratedPair()
    {
        // The same calls RdpConnection::initialize() makes for every peer; TLS
        // only (no RDP standard security), so an ECDSA key is fine.
        QTemporaryDir dir;
        const Paths paths{dir.filePath(u"krdp.crt"_s), dir.filePath(u"krdp.key"_s)};
        QVERIFY(ensure(paths, u"host"_s, QDateTime::currentDateTimeUtc()).ok);
        auto certificate = freerdp_certificate_new_from_file(paths.certificate.toLocal8Bit().constData());
        QVERIFY(certificate);
        freerdp_certificate_free(certificate);
        auto key = freerdp_key_new_from_file(paths.key.toLocal8Bit().constData());
        QVERIFY(key);
        freerdp_key_free(key);
    }

    void unwritableLocationFails()
    {
        const Paths paths{u"/proc/krdp-test/krdp.crt"_s, u"/proc/krdp-test/krdp.key"_s};
        const auto result = ensure(paths, u"host"_s, kNow);
        QVERIFY(!result.ok);
        QVERIFY(!result.error.isEmpty());
    }

    // AUD-FIX7: the brokers' certificates (/etc/krdp/virtual-host.{crt,key}, console.{crt,key}),
    // as the root hosts manage them: missing -> created (key 0600, certificate 0644, a new
    // directory 0755), valid -> kept, expiring or expired -> renewed, a loose key mode repaired,
    // a symlinked pair left to the administrator.
    void brokerCertificateIsCreatedRenewedAndKept()
    {
        QTemporaryDir dir;
        const Paths paths{dir.filePath(u"etc/krdp/virtual-host.crt"_s), dir.filePath(u"etc/krdp/virtual-host.key"_s)};
        const uint owner = ::geteuid();
        const auto mode = [](const QString &path) {
            struct stat st{};
            return ::stat(QFile::encodeName(path).constData(), &st) == 0 ? int(st.st_mode & 07777) : -1;
        };

        // Missing: created.
        const auto created = ensureSystem(paths, u"ace"_s, kNow, owner);
        QVERIFY2(created.ok, qPrintable(created.error));
        QVERIFY(created.generated);
        QCOMPARE(created.decision, Decision::GenerateMissing);
        QCOMPARE(created.info.algorithm, u"ECDSA P-256"_s);
        QCOMPARE(created.info.notAfter, kNow.addDays(kValidityDays));
        QVERIFY(!created.info.sha256Fingerprint.isEmpty());
        QCOMPARE(mode(paths.key), 0600);
        QCOMPARE(mode(paths.certificate), 0644);
        QCOMPARE(mode(dir.filePath(u"etc/krdp"_s)), 0755);

        // Valid (a day later, or nine years on): kept, never rotated.
        for (const auto &when : {kNow.addDays(1), kNow.addYears(9)}) {
            const auto kept = ensureSystem(paths, u"ace"_s, when, owner);
            QVERIFY(kept.ok);
            QVERIFY(!kept.generated);
            QCOMPARE(kept.decision, Decision::UseExisting);
            QCOMPARE(kept.info.sha256Fingerprint, created.info.sha256Fingerprint);
        }

        // A key others can read: made private, and still the same certificate.
        QVERIFY(QFile::setPermissions(paths.key, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        const auto repaired = ensureSystem(paths, u"ace"_s, kNow.addDays(2), owner);
        QVERIFY(repaired.ok && !repaired.generated);
        QCOMPARE(mode(paths.key), 0600);
        QCOMPARE(repaired.info.sha256Fingerprint, created.info.sha256Fingerprint);
        QVERIFY2(repaired.notes.join(u' ').contains(u"mode 644; now 600"_s), qPrintable(repaired.notes.join(u'|')));

        // Within 30 days of expiry: renewed.
        const auto soon = ensureSystem(paths, u"ace"_s, kNow.addDays(kValidityDays - 10), owner);
        QVERIFY(soon.ok && soon.generated);
        QCOMPARE(soon.decision, Decision::GenerateExpiringSoon);
        QVERIFY(soon.info.sha256Fingerprint != created.info.sha256Fingerprint);
        QCOMPARE(mode(paths.key), 0600);

        // Expired (Sol's :3395 certificate, 2026-09-23): renewed.
        QVERIFY(generate(paths, u"sol"_s, kNow.addDays(-400), 365, nullptr));
        QVERIFY(inspect(paths).notAfter < kNow);
        const auto expired = ensureSystem(paths, u"sol"_s, kNow, owner);
        QVERIFY(expired.ok && expired.generated);
        QCOMPARE(expired.decision, Decision::GenerateExpired);
        QCOMPARE(expired.info.notAfter, kNow.addDays(kValidityDays));
    }

    void brokerCertificateLeavesSymlinksAndRelativePathsAlone()
    {
        QTemporaryDir dir;
        const uint owner = ::geteuid();
        QCOMPARE(ensureSystem({u"krdp.crt"_s, u"krdp.key"_s}, u"x"_s, kNow, owner).ok, false);

        const Paths real{dir.filePath(u"real.crt"_s), dir.filePath(u"real.key"_s)};
        QVERIFY(generate(real, u"admin"_s, kNow.addDays(-3000), 3010, nullptr)); // valid, but within 30 days
        const Paths linked{dir.filePath(u"etc/console.crt"_s), dir.filePath(u"etc/console.key"_s)};
        QVERIFY(QDir().mkpath(dir.filePath(u"etc"_s)));
        QVERIFY(QFile::link(real.certificate, linked.certificate));
        QVERIFY(QFile::link(real.key, linked.key));
        const auto before = inspect(real).sha256Fingerprint;
        const auto result = ensureSystem(linked, u"x"_s, kNow, owner);
        QVERIFY(result.ok);
        QVERIFY(result.administratorManaged);
        QVERIFY(!result.generated);
        QCOMPARE(inspect(real).sha256Fingerprint, before); // never replaced
        QVERIFY(QFileInfo(linked.certificate).isSymLink());
        QVERIFY(result.notes.join(u' ').contains(u"replace it"_s));
        // A dangling one is an error, not something to paper over.
        QVERIFY(QFile::remove(real.key));
        QVERIFY(!ensureSystem(linked, u"x"_s, kNow, owner).ok);
    }

    // AUD-FIX8: a directory others can write lets them replace the key (Sol's
    // /opt/krdp-console/cert). A dedicated one is repaired to 0755; a shared one is refused.
    void brokerCertificateDirectoryIsMadeSafe()
    {
        QTemporaryDir dir;
        const uint owner = ::geteuid();
        const auto mode = [](const QString &path) {
            struct stat st{};
            return ::stat(QFile::encodeName(path).constData(), &st) == 0 ? int(st.st_mode & 07777) : -1;
        };
        // Dedicated, group/other-writable, with an existing pair: repaired, pair kept.
        const QString certDir = dir.filePath(u"opt/krdp-console/cert"_s);
        const Paths paths{certDir + u"/krdp.crt"_s, certDir + u"/krdp.key"_s};
        QVERIFY(ensureSystem(paths, u"sol"_s, kNow, owner).ok);
        const auto before = inspect(paths).sha256Fingerprint;
        QVERIFY(::chmod(QFile::encodeName(certDir).constData(), 0777) == 0);
        const auto repaired = ensureSystem(paths, u"sol"_s, kNow.addDays(1), owner);
        QVERIFY2(repaired.ok, qPrintable(repaired.error));
        QCOMPARE(mode(certDir), 0755);
        QCOMPARE(mode(paths.key), 0600);
        QCOMPARE(repaired.info.sha256Fingerprint, before);
        QVERIFY2(repaired.notes.join(u' ').contains(u"mode 777) could be written by others"_s), qPrintable(repaired.notes.join(u'|')));
        // Safe now: nothing more to say about the directory.
        const auto again = ensureSystem(paths, u"sol"_s, kNow.addDays(2), owner);
        QVERIFY(again.ok);
        QVERIFY(!again.notes.join(u' ').contains(certDir));

        // Shared (other files in it) and writable by others: refused, nothing written.
        const QString shared = dir.filePath(u"shared"_s);
        QVERIFY(QDir().mkpath(shared));
        QFile other(shared + u"/notes.txt"_s);
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.close();
        QVERIFY(::chmod(QFile::encodeName(shared).constData(), 0775) == 0);
        const Paths inShared{shared + u"/krdp.crt"_s, shared + u"/krdp.key"_s};
        const auto refused = ensureSystem(inShared, u"sol"_s, kNow, owner);
        QVERIFY(!refused.ok);
        QVERIFY2(refused.error.contains(u"chown root:root, chmod 0755"_s), qPrintable(refused.error));
        QVERIFY(!QFile::exists(inShared.key));
        QCOMPARE(mode(shared), 0775);

        // A sticky world-writable directory (/tmp-like) is never "ours", even when empty.
        const QString sticky = dir.filePath(u"sticky"_s);
        QVERIFY(QDir().mkpath(sticky));
        QVERIFY(::chmod(QFile::encodeName(sticky).constData(), 01777) == 0);
        QVERIFY(!ensureSystem({sticky + u"/k.crt"_s, sticky + u"/k.key"_s}, u"sol"_s, kNow, owner).ok);

        // A new directory is created 0755 whatever the umask.
        const mode_t previous = ::umask(077);
        const QString fresh = dir.filePath(u"etc/krdp-new"_s);
        const auto created = ensureSystem({fresh + u"/c.crt"_s, fresh + u"/c.key"_s}, u"ace"_s, kNow, owner);
        ::umask(previous);
        QVERIFY2(created.ok, qPrintable(created.error));
        QCOMPARE(mode(fresh), 0755);
        QCOMPARE(mode(fresh + u"/c.key"_s), 0600);
    }
};

QTEST_GUILESS_MAIN(ServerCertificateTest)
#include "ServerCertificateTest.moc"

