// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-K3: generation and renewal decisions for the server's own certificate,
// with an injected clock.

#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include "ServerCertificate.h"

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
};

QTEST_GUILESS_MAIN(ServerCertificateTest)
#include "ServerCertificateTest.moc"

