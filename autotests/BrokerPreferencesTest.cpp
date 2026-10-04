// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerpreferences.h"
#include "settingfielddefinition.h"
#include "UserConfiguration.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace Qt::StringLiterals;
using namespace KRdp;
class BrokerPreferencesTest : public QObject {
    Q_OBJECT
    static void write(const QString &path,const QByteArray &bytes) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate)); QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)); QCOMPARE(file.write(bytes),bytes.size()); }
    static QByteArray read(const QString &path) { QFile file(path); if(!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
    static QVariantMap allValues() {
        return {{u"Quality"_s,u"91"_s},{u"AdaptiveQuality"_s,u"true"_s},{u"PreferAudioQuality"_s,u"true"_s},{u"Codec"_s,u"avc444"_s},
            {u"SoftwareEncoding"_s,u"prefer"_s},{u"Av1Tiles"_s,u"8"_s},{u"Avc444MotionGapMs"_s,u"50"_s},{u"Avc444RestMs"_s,u"75"_s},{u"Avc444MaxGapMs"_s,u"1000"_s},
            {u"MonitorMode"_s,u"specific"_s},{u"MonitorIndex"_s,u"2"_s},{u"VirtualMonitorPolicy"_s,u"extend"_s},{u"VirtualMonitorLayout"_s,u"physical"_s},
            {u"VirtualMonitorFallbackSize"_s,u"2560x1440"_s},{u"WakeDisplayOnConnect"_s,u"false"_s},{u"StandardClientMedia"_s,u"false"_s},{u"VirtualStockClientPolicy"_s,u"refuse"_s}};
    }
private Q_SLOTS:
    void definitionsDriveTheForm() {
        QTemporaryDir dir; BrokerPreferences model(dir.path()); QVERIFY(model.reload());
        QSet<QString> keys; int numeric=0; QVariantList bounds;
        for(const auto &row:model.definitions()) {
            const auto definition=row.toMap(); const auto key=definition[u"key"_s].toString();
            QVERIFY2(KRdp::SettingFields::definitionProblem(definition).isEmpty(),qPrintable(key+u": "_s+KRdp::SettingFields::definitionProblem(definition)));
            QVERIFY(!keys.contains(key)); keys.insert(key);
            const auto control=definition[u"control"_s].toString();
            if(control==u"spin"||control==u"slider") { ++numeric; bounds.append(definition); }
            const auto when=definition[u"showWhenKey"_s].toString(); if(!when.isEmpty()) QVERIFY2(keys.contains(when)||when==u"MonitorMode",qPrintable(key));
        }
        // The advertised bounds are values the model accepts and can save, all numeric fields at once (they constrain each other).
        for(const auto &edge:{u"min"_s,u"max"_s}) {
            for(const auto &row:bounds) QVERIFY(model.setValue(row.toMap()[u"key"_s].toString(),QString::number(row.toMap()[edge].toInt())));
            QVERIFY2(model.canSave(),qPrintable(edge)); model.discard();
        }
        QCOMPARE(keys.size(),17); QVERIFY(numeric>=5);
    }
    void completeWhitelistedRoundtripAndInheritance() {
        QTemporaryDir dir; const auto path=dir.filePath(u"farsideserverrc"_s);
        const QByteArray preserved("# preserved comment\n[General]\nListenPort=4321\nCertificate=/root/fixture.pem\nPassword=fixture-secret-only\nUsers=old-owner\nUnknown=unchanged\n[Other]\nQuality=5\n");
        write(path,preserved); BrokerPreferences model(dir.path()); QVERIFY(!model.loaded()); QVERIFY(model.reload()); QVERIFY(model.values().isEmpty());
        QStringList defined; for(const auto &v:model.definitions()) defined.append(v.toMap()[u"key"_s].toString());
        auto expected=BrokerUserSettings::preferenceKeys(); defined.sort(); expected.sort(); QCOMPARE(defined,expected); QCOMPARE(defined.size(),17);
        const auto desired=allValues(); for(auto it=desired.cbegin();it!=desired.cend();++it) QVERIFY(model.setValue(it.key(),it.value().toString()));
        QVERIFY(model.canSave()); QVERIFY2(model.save(),qPrintable(model.error())); QVERIFY(!model.modified()); QVERIFY(model.reconnectRequired()); QCOMPARE(model.values(),desired);
        struct stat state{}; QCOMPARE(::stat(QFile::encodeName(path).constData(),&state),0); QCOMPARE(state.st_uid,getuid()); QCOMPARE(state.st_mode&0777,mode_t(0600));
        const auto bytes=read(path); QVERIFY(bytes.contains("Password=fixture-secret-only\n")); QVERIFY(bytes.contains("Certificate=/root/fixture.pem\n")); QVERIFY(bytes.contains("[Other]\nQuality=5\n"));
        const auto production=BrokerUserSettings::parse(bytes); QVERIFY(production.error.isEmpty()); QCOMPARE(production.preferences.quality,std::optional<quint8>(91));
        QCOMPARE(production.preferences.chroma,std::optional(ChromaPolicy{50,75,1000})); QCOMPARE(production.preferences.virtualMonitorFallbackSize,std::optional(QSize(2560,1440)));
        QVERIFY(!QJsonDocument::fromVariant(model.values()).toJson().contains("fixture-secret")); QVERIFY(!model.values().contains(u"Certificate"_s));
        model.defaults(); QVERIFY(model.canSave()); QVERIFY(model.save()); QCOMPARE(read(path),preserved); QVERIFY(model.values().isEmpty());
        QVERIFY(!BrokerUserSettings::parse(read(path)).preferences.quality);
    }
    void preserveRepeatedSectionsMarkersAndCanonicalizeValues() {
        const QByteArray original("# untouched\r\n[General]\r\n Quality = 31\r\nQuality[fr]=90\r\nPreferAudioQuality[$e]=$FIXTURE_ENV\r\nQuality=42\r\nUnknown=1\r\n[Other]\r\nQuality=100\r\n[General]\r\nWakeDisplayOnConnect=0\r\nHost=2");
        const auto desired=QVariantMap{{u"Quality"_s,u"0077"_s},{u"WakeDisplayOnConnect"_s,u"TRUE"_s}};
        const auto edited=BrokerUserSettings::edit(original,desired); QVERIFY2(edited.error.isEmpty(),qPrintable(edited.error));
        const QByteArray expected("# untouched\r\n[General]\r\nQuality[fr]=90\r\nPreferAudioQuality[$e]=$FIXTURE_ENV\r\nUnknown=1\r\n[Other]\r\nQuality=100\r\n[General]\r\nHost=2\r\nQuality=77\r\nWakeDisplayOnConnect=true\r\n");
        QCOMPARE(edited.document,expected); QCOMPARE(BrokerUserSettings::parse(edited.document).preferences.quality,std::optional<quint8>(77));
        const auto again=BrokerUserSettings::edit(edited.document,{{u"Quality"_s,u"77"_s},{u"WakeDisplayOnConnect"_s,u"true"_s}}); QCOMPARE(again.document,edited.document);
    }
    void immutableValuesAndGroupArePreserved() {
        QTemporaryDir dir; const auto path=dir.filePath(u"farsideserverrc"_s); const QByteArray original("[General]\nQuality=31\nQuality[$i]=42\nAv1Tiles=8\nUsers=old-owner\n");
        write(path,original); BrokerPreferences model(dir.path()); QVERIFY(model.reload()); QCOMPARE(model.values()[u"Quality"_s].toString(),u"42"_s);
        QVERIFY(model.lockedKeys().contains(u"Quality"_s)); QVERIFY(!model.setValue(u"Quality"_s,u"50"_s)); QVERIFY(!model.inherit(u"Quality"_s));
        model.defaults(); QVERIFY(model.save()); QCOMPARE(read(path),QByteArray("[General]\nQuality=31\nQuality[$i]=42\nUsers=old-owner\n"));
        const QByteArray group("[General][$i]\nQuality=42\nUnknown=1\n"); write(path,group); QVERIFY(model.reload()); QCOMPARE(model.lockedKeys().size(),17);
        model.defaults(); QVERIFY(!model.modified()); QVERIFY(!model.setValue(u"AdaptiveQuality"_s,u"true"_s)); QCOMPARE(read(path),group);
        QVERIFY(!BrokerUserSettings::edit(group,{}).error.isEmpty());
    }
    void invalidStagedTransactionsAndInjectionNeverWrite() {
        QTemporaryDir dir; const auto path=dir.filePath(u"farsideserverrc"_s); const QByteArray original("[General]\nQuality=42\nHostSecret=fixture-secret-only\n");
        write(path,original); BrokerPreferences model(dir.path()); QVERIFY(model.reload());
        for(const auto &value:{u"101"_s,u"-1"_s,u"not-a-number"_s}) { QVERIFY(model.setValue(u"Quality"_s,value)); QVERIFY(!model.canSave()); QVERIFY(!model.save()); QCOMPARE(read(path),original); QVERIFY(model.error().contains(u"Quality"_s)); }
        QVERIFY(!model.setValue(u"Quality"_s,u"42\nListenPort=1"_s)); QVERIFY(!model.setValue(u"Certificate"_s,u"/root/other"_s));
        QVERIFY(!BrokerUserSettings::edit(original,{{u"Quality"_s,42}}).error.isEmpty());
        QVERIFY(model.reload()); QVERIFY(model.setValue(u"Avc444MotionGapMs"_s,u"200"_s)); QVERIFY(!model.canSave()); QVERIFY(!model.save()); QCOMPARE(read(path),original);
        QVERIFY(model.setValue(u"Avc444RestMs"_s,u"300"_s)); QVERIFY(model.setValue(u"Avc444MaxGapMs"_s,u"1000"_s)); QVERIFY(model.canSave()); QVERIFY(model.save());
    }
    void staleEditorAndNonPreferenceConcurrentChangeKeepEdits() {
        QTemporaryDir dir; const auto path=dir.filePath(u"farsideserverrc"_s); write(path,"[General]\nQuality=42\nHost=original\n");
        BrokerPreferences first(dir.path()),second(dir.path()); QVERIFY(first.reload()); QVERIFY(second.reload());
        QVERIFY(first.setValue(u"Quality"_s,u"75"_s)); QVERIFY(first.save()); const auto accepted=read(path);
        QVERIFY(second.setValue(u"Quality"_s,u"88"_s)); QVERIFY(!second.save()); QCOMPARE(read(path),accepted); QCOMPARE(second.values()[u"Quality"_s].toString(),u"88"_s);
        QVERIFY(second.error().contains(u"changed"_s)); QVERIFY(second.reload());
        QVERIFY(second.setValue(u"Quality"_s,u"90"_s)); write(path,accepted+"# external host change\n"); const auto changed=read(path);
        QVERIFY(!second.save()); QCOMPARE(read(path),changed); QVERIFY(second.modified());
    }
    void missingConfigAndSafeParentSymlink() {
        QTemporaryDir dir; const auto directory=dir.filePath(u"new-config"_s); BrokerPreferences model(directory); QVERIFY(model.reload()); QVERIFY(model.values().isEmpty());
        QVERIFY(!QFileInfo::exists(directory)); QVERIFY(model.setValue(u"Quality"_s,u"80"_s)); QVERIFY2(model.save(),qPrintable(model.error()));
        const auto link=dir.filePath(u"config-link"_s); QVERIFY(QFile::link(directory,link)); BrokerPreferences linked(link); QVERIFY(linked.reload());
        QVERIFY(linked.setValue(u"Quality"_s,u"85"_s)); QVERIFY2(linked.save(),qPrintable(linked.error())); QCOMPARE(BrokerUserSettings::parse(read(directory+u"/farsideserverrc"_s)).preferences.quality,std::optional<quint8>(85));
        QVERIFY(!UserConfiguration::userPath(0)); QVERIFY(!UserConfiguration::userPath(quint32(-1)));
    }
    void unsafeFilesAndLockNeverOverwrite() {
        QTemporaryDir dir; const auto path=dir.filePath(u"farsideserverrc"_s); const auto backup=dir.filePath(u"backup"_s);
        const QByteArray original("[General]\nQuality=42\n"); write(path,original); BrokerPreferences model(dir.path()); QVERIFY(model.reload()); QVERIFY(model.setValue(u"Quality"_s,u"75"_s));
        QVERIFY(QFile::rename(path,backup)); QVERIFY(QFile::link(backup,path)); QVERIFY(!model.save()); QCOMPARE(read(backup),original); QVERIFY(QFile::remove(path)); QVERIFY(QFile::rename(backup,path));
        const auto lockPath=dir.filePath(u".farsideserverrc.lock"_s); QFile::remove(lockPath); QVERIFY(QFile::link(path,lockPath)); QVERIFY(!model.save()); QCOMPARE(read(path),original); QVERIFY(QFile::remove(lockPath));
        const int lock=::open(QFile::encodeName(lockPath).constData(),O_CREAT|O_RDWR,0600); QVERIFY(lock>=0); QCOMPARE(::flock(lock,LOCK_EX|LOCK_NB),0);
        QVERIFY(!model.save()); QCOMPARE(read(path),original); ::close(lock);
        QCOMPARE(::chmod(QFile::encodeName(path).constData(),0666),0); QVERIFY(!model.save()); QCOMPARE(read(path),original); QCOMPARE(::chmod(QFile::encodeName(path).constData(),0600),0);
        QVERIFY(QFile::rename(path,backup)); QCOMPARE(::mkfifo(QFile::encodeName(path).constData(),0600),0); QVERIFY(!model.reload()); QVERIFY(!model.save()); QVERIFY(QFile::remove(path)); QVERIFY(QFile::rename(backup,path));
        QVERIFY(model.reload()); QVERIFY(model.setValue(u"Quality"_s,u"75"_s)); QCOMPARE(::chmod(QFile::encodeName(dir.path()).constData(),0777),0); QVERIFY(!model.save()); QCOMPARE(read(path),original); QCOMPARE(::chmod(QFile::encodeName(dir.path()).constData(),0700),0);
    }
};
QTEST_GUILESS_MAIN(BrokerPreferencesTest)
#include "BrokerPreferencesTest.moc"
