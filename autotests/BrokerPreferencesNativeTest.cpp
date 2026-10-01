// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Sol only, under bwrap with a private directory bound over the canonical
// account .config. Never run against a real user's configuration.
#include "brokerpreferences.h"
#include "UserConfiguration.h"
#include <QFile>
#include <QFileInfo>
#include <QTest>
#include <sys/fsuid.h>
#include <unistd.h>
using namespace Qt::StringLiterals;
using namespace KRdp;
class BrokerPreferencesNativeTest : public QObject {
    Q_OBJECT
    QString m_path;
private Q_SLOTS:
    void initTestCase() {
        QVERIFY2(qEnvironmentVariable("FARSIDE_PREFERENCES_PRIVATE_HOME")==u"1"_s,"Sol private home namespace required");
        QVERIFY(getuid()!=0); const auto path=UserConfiguration::userPath(getuid()); QVERIFY(path); m_path=*path;
        QFile marker(QFileInfo(m_path).absolutePath()+u"/.farside-preferences-fixture"_s); QVERIFY(marker.open(QIODevice::ReadOnly));
        QCOMPARE(marker.readAll(),QByteArray("private-farside-preferences-fixture\n"));
        QFile mounts(u"/proc/self/mountinfo"_s); QVERIFY(mounts.open(QIODevice::ReadOnly));
        const QString mount=u" "_s+QFileInfo(m_path).absolutePath()+u" "_s;
        QVERIFY2(mounts.readAll().contains(mount.toUtf8()),"Canonical .config must be a separate private mount");
        QVERIFY(!qEnvironmentVariable("HOME").contains(QFileInfo(m_path).absolutePath()));
    }
    void productionEditorAndAuthenticatedReaderAgree() {
        BrokerPreferences editor; QVERIFY(editor.reload()); QCOMPARE(editor.values()[u"Quality"_s].toString(),u"42"_s);
        const QVariantMap desired{{u"Quality"_s,u"93"_s},{u"AdaptiveQuality"_s,u"true"_s},{u"PreferAudioQuality"_s,u"true"_s},{u"Codec"_s,u"avc420"_s},
            {u"SoftwareEncoding"_s,u"never"_s},{u"Av1Tiles"_s,u"4"_s},{u"Avc444MotionGapMs"_s,u"50"_s},{u"Avc444RestMs"_s,u"75"_s},{u"Avc444MaxGapMs"_s,u"1000"_s},
            {u"MonitorMode"_s,u"virtual"_s},{u"MonitorIndex"_s,u"2"_s},{u"VirtualMonitorPolicy"_s,u"extend"_s},{u"VirtualMonitorLayout"_s,u"client"_s},
            {u"VirtualMonitorFallbackSize"_s,u"1280x720"_s},{u"WakeDisplayOnConnect"_s,u"false"_s},{u"StandardClientMedia"_s,u"false"_s},{u"VirtualStockClientPolicy"_s,u"refuse"_s}};
        for(auto it=desired.cbegin();it!=desired.cend();++it) QVERIFY(editor.setValue(it.key(),it.value().toString()));
        QVERIFY2(editor.save(),qPrintable(editor.error())); QCOMPARE(editor.values(),desired);
        const auto raw=UserConfiguration::readUser(getuid()); QVERIFY(raw);
        const auto parsed=BrokerUserSettings::readUser(getuid()); QVERIFY(parsed.error.isEmpty());
        QCOMPARE(BrokerUserSettings::publicValues(parsed.preferences,BrokerUserSettings::fields(*raw)),desired);
        QCOMPARE(parsed.preferences.quality,std::optional<quint8>(93)); QCOMPARE(parsed.preferences.monitorMode,std::optional(u"virtual"_s));
        QCOMPARE(parsed.preferences.virtualStockClientPolicy,std::optional(u"refuse"_s));
        QVERIFY(raw->contains("Certificate=/root/fixture-only.pem\n")); QVERIFY(raw->contains("Password=fixture-secret-only\n"));
        QVERIFY(!editor.values().contains(u"Certificate"_s)); QVERIFY(editor.reconnectRequired());
    }
    void defaultInheritanceAndLegacyPreservation() {
        BrokerPreferences editor; QVERIFY(editor.reload()); editor.defaults(); QVERIFY2(editor.save(),qPrintable(editor.error())); QVERIFY(editor.values().isEmpty());
        const auto parsed=BrokerUserSettings::readUser(getuid()); QVERIFY(parsed.error.isEmpty()); QVERIFY(!parsed.preferences.quality); QVERIFY(!parsed.preferences.chroma);
        QVERIFY(!parsed.preferences.codec); QVERIFY(!parsed.preferences.monitorMode); QVERIFY(!parsed.preferences.standardClientMedia);
        const auto raw=UserConfiguration::readUser(getuid()); QVERIFY(raw); QCOMPARE(*raw,QByteArray("[General]\nCertificate=/root/fixture-only.pem\nPassword=fixture-secret-only\n"));
        const auto uidBefore=::setfsuid(uid_t(-1)); QCOMPARE(uid_t(uidBefore),getuid());
    }
};
QTEST_GUILESS_MAIN(BrokerPreferencesNativeTest)
#include "BrokerPreferencesNativeTest.moc"
