// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionMaintenanceRecord.h"
#include <QTest>

using R = KRdp::VirtualSessionMaintenanceRecord;
class VirtualSessionMaintenanceRecordTest : public QObject {
    Q_OBJECT
    static QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    static R initial() {
        R r;
        r.phase = R::Phase::InitialBlocked;
        r.installation = id(); r.transaction = id(); r.generation = id(); r.boot = id();
        r.profile = QString(64, QLatin1Char('a')); r.attestation = id();
        return r;
    }
private Q_SLOTS:
    void claimConsumesInitialIdentity() {
        const auto r = initial(); QVERIFY(r.valid());
        const auto invocation = id();
        const auto claimed = r.claim(r.boot, invocation); QVERIFY(claimed);
        QCOMPARE(claimed->phase, R::Phase::Validating);
        QCOMPARE(claimed->installation, r.installation);
        QCOMPARE(claimed->transaction, r.transaction);
        QCOMPARE(claimed->generation, r.generation);
        QCOMPARE(claimed->profile, r.profile);
        QCOMPARE(claimed->attestation, r.attestation);
        QCOMPARE(claimed->invocation, invocation);
        QVERIFY(!claimed->claim(r.boot, invocation));
        QVERIFY(!claimed->claim(r.boot, id()));
        QVERIFY(!r.claim(id(), invocation));
        QVERIFY(!r.claim(r.boot, QStringLiteral("caller-supplied-nonidentity")));
    }
    void repeatedInvalidationCannotRecoverBootstrap() {
        const auto r = initial(); const auto claimed = r.claim(r.boot, id()); QVERIFY(claimed);
        const auto invalidated = claimed->invalidate(r.boot); QVERIFY(invalidated);
        QCOMPARE(invalidated->phase, R::Phase::ExternalUnknown);
        QCOMPARE(invalidated->installation, r.installation);
        QVERIFY(invalidated->generation != claimed->generation);
        QVERIFY(invalidated->transaction.isEmpty()); QVERIFY(invalidated->invocation.isEmpty());
        QVERIFY(invalidated->attestation.isEmpty()); QVERIFY(invalidated->profile.isEmpty());
        QVERIFY(!invalidated->claim(r.boot, id()));
        const auto again = invalidated->invalidate(r.boot); QVERIFY(again);
        QVERIFY(again->generation != invalidated->generation);
        const auto newBoot = id(); const auto rebooted = again->invalidate(newBoot); QVERIFY(rebooted);
        QCOMPARE(rebooted->boot, newBoot); QVERIFY(!rebooted->claim(newBoot, id()));
    }
    void strictEncodingForEachPhase() {
        auto r = initial();
        for (const auto phase : {R::Phase::InitialBlocked, R::Phase::Validating, R::Phase::Clean, R::Phase::ExternalUnknown}) {
            if (phase == R::Phase::Validating) r = *r.claim(r.boot, id());
            if (phase == R::Phase::Clean) { r.phase = phase; r.epoch = id(); }
            if (phase == R::Phase::ExternalUnknown) r = *r.invalidate(r.boot);
            const auto bytes = r.encode(); QVERIFY(!bytes.isEmpty()); QVERIFY(bytes.size() <= 512);
            const auto decoded = R::decode(bytes); QVERIFY(decoded); QCOMPARE(*decoded, r);
            QVERIFY(!R::decode(bytes + '\n')); QVERIFY(!R::decode(bytes.chopped(1)));
            QVERIFY(!R::decode(bytes + bytes));
            QVERIFY(!R::decode(QByteArray(" ") + bytes));
            auto crlf = bytes; crlf.replace("\n", "\r\n"); QVERIFY(!R::decode(crlf));
        }
        QVERIFY(!R::decode({})); QVERIFY(!R::decode(QByteArray(513, 'x')));
        QVERIFY(!R::decode("KRDP-MAINTENANCE-1\nblocked\n"));
    }
    void inconsistentFieldsCannotEncode() {
        const auto base = initial();
        auto r = base; r.invocation = id(); QVERIFY(!r.valid()); QVERIFY(r.encode().isEmpty());
        r = base; r.epoch = id(); QVERIFY(!r.valid());
        r = base; r.transaction.clear(); QVERIFY(!r.valid());
        r = base; r.installation.clear(); QVERIFY(!r.valid());
        r = base; r.profile = QString(64, QLatin1Char('A')); QVERIFY(!r.valid());
        r = base; r.attestation.clear(); QVERIFY(!r.valid());
        r = base; r.boot = QUuid(r.boot).toString(); QVERIFY(!r.valid());
        r = base; r.phase = R::Phase::Clean; QVERIFY(!r.valid());
        r = base; r.phase = static_cast<R::Phase>(99); QVERIFY(!r.valid());
        r = *base.invalidate(base.boot); r.profile = base.profile; QVERIFY(!r.valid());
    }
};
QTEST_GUILESS_MAIN(VirtualSessionMaintenanceRecordTest)
#include "VirtualSessionMaintenanceRecordTest.moc"
