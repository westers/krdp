// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "LegacySettingsMigration.h"
#include "BrokerUserSettings.h"
#include "krdpserversettings.h"
#include <KConfigGroup>
#include <KSharedConfig>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
using namespace Qt::StringLiterals;
using namespace KRdp;
namespace Migration = LegacySettingsMigration;

class LegacySettingsMigrationTest : public QObject {
    Q_OBJECT
    const Migration::Owner owner{u"original"_s, 1000};
    const Migration::Resolver resolver = [](const QString &account) -> std::optional<quint32> {
        if (account == u"original") return 1000;
        if (account == u"another") return 1001;
        return {};
    };
    static QVariantMap brokerValues(const QByteArray &bytes) {
        const auto parsed = BrokerUserSettings::parse(bytes);
        return parsed.error.isEmpty() ? BrokerUserSettings::publicValues(parsed.preferences, BrokerUserSettings::fields(bytes)) : QVariantMap{};
    }
    static QVariant legacyValue(const QByteArray &bytes, const QString &key) {
        QTemporaryDir temporary; QFile file(temporary.filePath(u"legacyrc"_s));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return {};
        file.close();
        KRDPServerSettings settings;
        settings.setSharedConfig(KSharedConfig::openConfig(file.fileName(), KConfig::SimpleConfig));
        settings.load();
        return settings.property((key.left(1).toLower() + key.mid(1)).toUtf8().constData());
    }
private Q_SLOTS:
    void omittedDefaultsMatchActualGeneratedLegacySettings() {
        const auto snapshot = Migration::inspect({}, owner, resolver); QVERIFY2(snapshot.error.isEmpty(), qPrintable(snapshot.error));
        QCOMPARE(snapshot.preferences.size(), 17); QCOMPARE(snapshot.defaultedKeys.size(), 17);
        for (auto it = snapshot.preferences.cbegin(); it != snapshot.preferences.cend(); ++it)
            QCOMPARE(it.value().toString(), legacyValue({}, it.key()).toString());
        for (auto it = snapshot.hostFacts.cbegin(); it != snapshot.hostFacts.cend(); ++it) {
            const auto expected = legacyValue({}, it.key()).toString();
            if (it.key() == u"ListenAddress") QCOMPARE(it.value().toString(), u"0.0.0.0"_s);
            else if (it.key() == u"CameraLoopbackDevice") QCOMPARE(it.value().toString(), u"none"_s);
            else QCOMPARE(it.value().toString(), expected);
        }
        QCOMPARE(snapshot.hostFacts.size(), 7); QVERIFY(!snapshot.ownerPam); QVERIFY(snapshot.aliases.isEmpty());
        QCOMPARE(snapshot.ownerPam, legacyValue({}, u"SystemUserEnabled"_s).toBool());
        const auto plan = Migration::prepare({}, std::nullopt, owner, resolver); QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QVERIFY(plan.changed); QCOMPARE(brokerValues(plan.document), snapshot.preferences);
        QCOMPARE(snapshot.preferences[u"Quality"_s].toString(), u"75"_s);
        QCOMPARE(snapshot.preferences[u"AdaptiveQuality"_s].toString(), u"true"_s);
        QCOMPARE(snapshot.hostFacts[u"ListenPort"_s].toString(), u"3389"_s); // Fact, never new listener/trust authorization.
    }
    void completeSettingsAndHostFactsAgreeWithRealLegacyAndBrokerReaders() {
        const QByteArray source("[General]\nQuality=64\nAdaptiveQuality=false\nPreferAudioQuality=true\nCodec=avc444\n"
            "SoftwareEncoding=never\nAv1Tiles=4\nAvc444MotionGapMs=80\nAvc444RestMs=160\nAvc444MaxGapMs=1200\n"
            "MonitorMode=specific\nMonitorIndex=2\nVirtualMonitorPolicy=extend\nVirtualMonitorLayout=physical\n"
            "VirtualMonitorFallbackSize=1600x900\nWakeDisplayOnConnect=false\nStandardClientMedia=false\n"
            "VirtualStockClientPolicy=refuse\nListenPort=4489\nListenAddress=::1\nAutogenerateCertificates=false\n"
            "Certificate=/tmp/a\\sb.crt\nCertificateKey=/tmp/key.pem\nVaapiDriverMode=radeonsi\nCameraLoopbackDevice=/dev/video10\n"
            "SystemUserEnabled=true\nUsers=another,ALIAS\n");
        const auto plan = Migration::prepare(source, source, owner, resolver, Migration::DestinationMode::SameLegacySnapshot); QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QCOMPARE(plan.legacy.defaultedKeys.size(), 0);
        for (auto it = plan.legacy.preferences.cbegin(); it != plan.legacy.preferences.cend(); ++it)
            QCOMPARE(it.value().toString(), legacyValue(source, it.key()).toString());
        for (auto it = plan.legacy.hostFacts.cbegin(); it != plan.legacy.hostFacts.cend(); ++it)
            QCOMPARE(it.value().toString(), legacyValue(source, it.key()).toString());
        QVERIFY(plan.legacy.ownerPam); QCOMPARE(plan.legacy.owner.uid, quint32(1000));
        QCOMPARE(plan.legacy.aliases, QStringList({u"another"_s, u"ALIAS"_s}));
        QCOMPARE(brokerValues(plan.document), plan.legacy.preferences);
        QVERIFY(plan.document.contains("Certificate=/tmp/a\\sb.crt\n"));
        QVERIFY(plan.document.contains("Users=another,ALIAS\n"));
    }
    void escapedListsAndAliasesNeverResolveAsTheirOwner() {
        QTemporaryDir temporary; const auto path = temporary.filePath(u"legacyrc"_s);
        const QStringList aliases{u"another"_s, u"Alias,comma"_s, u"Case"_s, u"case"_s, u"slash\\alias"_s};
        { KConfig config(path, KConfig::SimpleConfig); KConfigGroup group(&config, u"General"_s); group.writeEntry(u"Users"_s, aliases); QVERIFY(config.sync()); }
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto source = file.readAll();
        QStringList resolved;
        const auto snapshot = Migration::inspect(source, owner, [&](const QString &account) { resolved.append(account); return resolver(account); });
        QVERIFY2(snapshot.error.isEmpty(), qPrintable(snapshot.error)); QCOMPARE(snapshot.aliases, aliases);
        QCOMPARE(resolved, QStringList{u"original"_s}); QCOMPARE(snapshot.owner.uid, quint32(1000));
    }
    void sameFileMaterializesPartialDefaultsAndPreservesPrivateBytes() {
        const QByteArray source("# untouched\r\n[Other]\r\nsecret=PRIVATE-CONTENT\r\n[General][Nested]\r\nQuality=99\r\n[General]\r\n"
            "Quality=0068\r\nAvc444MotionGapMs=90\r\nUsers=another\r\nSystemUserEnabled=true\r\n"
            "ListenPort=4490\r\nUnknown[$e]=$PRIVATE_VALUE\r\n# keep comment\r\n");
        const auto plan = Migration::prepare(source, source, owner, resolver, Migration::DestinationMode::SameLegacySnapshot); QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QCOMPARE(plan.legacy.preferences[u"Quality"_s].toString(), u"68"_s);
        QCOMPARE(plan.legacy.preferences[u"Avc444RestMs"_s].toString(), u"150"_s);
        QVERIFY(plan.document.startsWith("# untouched\r\n[Other]\r\nsecret=PRIVATE-CONTENT\r\n[General][Nested]\r\nQuality=99\r\n[General]\r\n"));
        QVERIFY(plan.document.contains("Users=another\r\nSystemUserEnabled=true\r\nListenPort=4490\r\nUnknown[$e]=$PRIVATE_VALUE\r\n# keep comment\r\n"));
        QCOMPARE(brokerValues(plan.document), plan.legacy.preferences);
        const auto again = Migration::prepare(source, plan.document, owner, resolver); QVERIFY2(again.error.isEmpty(), qPrintable(again.error));
        QVERIFY(!again.changed); QCOMPARE(again.document, plan.document);
        const auto publicBytes = QJsonDocument(Migration::manifest(plan)).toJson();
        for (const auto &privateValue : {"PRIVATE-CONTENT", "$PRIVATE_VALUE", "\"another\"", "\"original\"", "\"4490\"", "Users", "Certificate"})
            QVERIFY2(!publicBytes.contains(privateValue), privateValue);
        QCOMPARE(Migration::manifest(plan)[u"pamScope"_s].toString(), u"original-owner-only"_s);
        QVERIFY(Migration::manifest(plan)[u"pendingActions"_s].toArray().size() >= 7);
    }
    void duplicateAndEscapedValuesUseActualKConfigSemantics() {
        for (const auto &source : {QByteArray("[General]\nQuality=\\x37\\x35\nAdaptiveQuality=FALSE\n"),
            QByteArray("[General]\nQuality=90\n[General]\nQuality=70\nAdaptiveQuality=0\nWakeDisplayOnConnect=1\nSystemUserEnabled=1\n")}) {
            const auto plan = Migration::prepare(source, source, owner, resolver, Migration::DestinationMode::SameLegacySnapshot); QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
            for (auto it = plan.legacy.preferences.cbegin(); it != plan.legacy.preferences.cend(); ++it)
                QCOMPARE(it.value().toString(), legacyValue(source, it.key()).toString());
            QCOMPARE(plan.legacy.ownerPam, legacyValue(source, u"SystemUserEnabled"_s).toBool());
            QCOMPARE(brokerValues(plan.document), plan.legacy.preferences);
        }
    }
    void locksArePreservedForSameFileAndSeparateDestinations() {
        for (const auto &source : {QByteArray("[General]\nQuality[$i]=80\n"), QByteArray("[General][$i]\nQuality=80\n")}) {
            const auto same = Migration::prepare(source, source, owner, resolver, Migration::DestinationMode::SameLegacySnapshot); QVERIFY2(same.error.isEmpty(), qPrintable(same.error));
            QCOMPARE(brokerValues(same.document), same.legacy.preferences);
            QCOMPARE(same.legacy.lockedPreferences.size(), source.startsWith("[General][$i]") ? 17 : 1);
            QCOMPARE(same.legacy.lockedHostFacts.size(), source.startsWith("[General][$i]") ? 7 : 0);
            QCOMPARE(same.legacy.lockedAdmissionKeys.size(), source.startsWith("[General][$i]") ? 2 : 0);
            for (auto it = same.legacy.preferences.cbegin(); it != same.legacy.preferences.cend(); ++it)
                QCOMPARE(it.value().toString(), legacyValue(same.document, it.key()).toString());
            const auto destination = Migration::prepare(source, QByteArray("[Other]\nPrivate=keep\n"), owner, resolver);
            QVERIFY2(destination.error.isEmpty(), qPrintable(destination.error));
            const auto locked = BrokerUserSettings::fields(destination.document).immutable;
            for (const auto &key : same.legacy.lockedPreferences) QVERIFY(locked.contains(key));
            for (const auto &key : same.legacy.lockedPreferences) {
                auto changed = same.legacy.preferences; changed.remove(key);
                QVERIFY(!BrokerUserSettings::edit(destination.document, changed).error.isEmpty());
            }
            const auto rerun = Migration::prepare(source, destination.document, owner, resolver);
            QVERIFY2(rerun.error.isEmpty(), qPrintable(rerun.error)); QVERIFY(!rerun.changed); QCOMPARE(rerun.document, destination.document);
        }
    }
    void conflictingOrLockedDestinationFailsWholePlan() {
        const auto mismatch = Migration::prepare({}, QByteArray("[General]\nQuality=95\n[Other]\nsecret=do-not-report\n"), owner, resolver);
        QVERIFY(!mismatch.error.isEmpty()); QCOMPARE(mismatch.conflicts, QStringList{u"Quality"_s}); QVERIFY(mismatch.document.isEmpty());
        const auto locked = Migration::prepare({}, QByteArray("[General][$i]\n"), owner, resolver);
        QVERIFY(!locked.error.isEmpty()); QCOMPARE(locked.conflicts.size(), 17); QVERIFY(locked.document.isEmpty());
        const QByteArray sameBytes("[General][$i]\n");
        QVERIFY(!Migration::prepare(sameBytes, sameBytes, owner, resolver).error.isEmpty());
        QVERIFY(Migration::prepare(sameBytes, sameBytes, owner, resolver, Migration::DestinationMode::SameLegacySnapshot).error.isEmpty());
        QVERIFY(!Migration::prepare(sameBytes, QByteArray{}, owner, resolver, Migration::DestinationMode::SameLegacySnapshot).error.isEmpty());
        QVERIFY(!Migration::prepare({}, std::nullopt, owner, resolver, Migration::DestinationMode::SameLegacySnapshot).error.isEmpty());
        QVERIFY(!QJsonDocument(Migration::manifest(mismatch)).toJson().contains("do-not-report"));
    }
    void identityPresenceAndCompleteRevisionAreBound() {
        const auto absent = Migration::prepare({}, std::nullopt, owner, resolver);
        const auto empty = Migration::prepare({}, QByteArray{}, owner, resolver);
        QVERIFY(absent.error.isEmpty()); QVERIFY(empty.error.isEmpty()); QVERIFY(absent.revision != empty.revision);
        const auto changed = Migration::prepare({}, QByteArray("# independent change\n"), owner, resolver);
        QVERIFY(changed.revision != empty.revision);
        const auto other = Migration::prepare({}, std::nullopt, {u"another"_s, 1001}, resolver); QVERIFY(other.error.isEmpty()); QVERIFY(other.revision != absent.revision);
        QVERIFY(!Migration::inspect({}, {u"original"_s, 0}, resolver).error.isEmpty());
        QVERIFY(!Migration::inspect({}, {u"original"_s, 1001}, resolver).error.isEmpty());
        QVERIFY(!Migration::inspect({}, {u"unknown"_s, 1000}, resolver).error.isEmpty());
    }
    void invalidSourcesDoNotBecomeDefaults_data() {
        QTest::addColumn<QByteArray>("source");
        for (const auto &entry : {"Quality=101", "Codec=hevc", "AdaptiveQuality=unexpected", "SystemUserEnabled=unexpected",
            "Avc444MotionGapMs=500", "ListenPort=0", "ListenAddress=hostname", "VaapiDriverMode=nvenc",
            "CameraLoopbackDevice=/tmp/not-a-device", "Quality[$e]=$SENSITIVE", "Quality[fr]=99", "Quality[$d]=1",
            "Qu\\x61lity=99", "Users=a,a", "Users=a,,b", "Certificate=/tmp/line\\nsecret"})
            QTest::newRow(entry) << QByteArray("[General]\n") + entry + '\n';
        QTest::newRow("group-marker") << QByteArray("[General][$e]\nQuality=99\n");
        QTest::newRow("invalid-utf8") << QByteArray("[General]\nQuality=75\nOther=") + char(0xff);
        QTest::newRow("truncated-utf8") << QByteArray("[General]\nOther=") + char(0xc3);
        QTest::newRow("malformed-group") << QByteArray("[Other\nprivate=do-not-log\n");
        QTest::newRow("NUL") << QByteArray("[General]\nQuality=75") + QByteArray(1, '\0') + "junk";
        QTest::newRow("oversize") << QByteArray(256*1024+1, '#');
    }
    void invalidSourcesDoNotBecomeDefaults() {
        QFETCH(QByteArray, source);
        const auto plan = Migration::prepare(source, std::nullopt, owner, resolver);
        QVERIFY(!plan.error.isEmpty()); QVERIFY(plan.document.isEmpty()); QVERIFY(plan.legacy.preferences.isEmpty());
        QVERIFY(!plan.error.contains(u"SENSITIVE")); QVERIFY(!Migration::manifest(plan)[u"prepared"_s].toBool());
    }
};
int main(int argc, char **argv)
{
    QTemporaryDir directory; if (!directory.isValid()) return 2;
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-farside-migration-test-bus");
    QCoreApplication app(argc, argv); LegacySettingsMigrationTest test; return QTest::qExec(&test, argc, argv);
}
#include "LegacySettingsMigrationTest.moc"
