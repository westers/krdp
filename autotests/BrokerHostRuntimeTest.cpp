// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerHostRuntime.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QTest>
using namespace Qt::StringLiterals;
namespace Runtime = KRdp::BrokerHostRuntime;
namespace Host = KRdp::BrokerHostSettings;
class BrokerHostRuntimeTest : public QObject {
    Q_OBJECT
    static QString option(const QString &key) {
        static const QMap<QString, QString> names{{u"Address"_s,u"address"_s},{u"Port"_s,u"port"_s},
            {u"Certificate"_s,u"certificate"_s},{u"CertificateKey"_s,u"certificate-key"_s},{u"Quality"_s,u"quality"_s},
            {u"AdaptiveQuality"_s,u"adaptive-quality"_s},{u"PreferAudioQuality"_s,u"prefer-audio-quality"_s},
            {u"StandardClientMedia"_s,u"standard-client-media"_s},{u"CameraLoopbackDevice"_s,u"camera-loopback-device"_s},
            {u"SoftwareEncoding"_s,u"software-encoding"_s},{u"Av1Tiles"_s,u"av1-tiles"_s},{u"VaapiDriver"_s,u"vaapi-driver"_s}};
        return names.value(key);
    }
    static Runtime::Unit unit(Host::Scope scope) {
        const auto contract=Runtime::installedContract(scope);
        Runtime::Unit result;result.id=Runtime::unitName(scope);result.loadState=u"loaded"_s;result.activeState=u"active"_s;result.subState=u"running"_s;
        result.user=u"root"_s;result.type=u"exec"_s;result.fragment=u"/usr/lib/systemd/system/"_s+result.id;
        result.controlGroup=u"/system.slice/"_s+result.id;result.pid=42;result.started=123;
        result.files.append({u"/etc/farside/"_s+Host::fileName(scope),true});
        QStringList argv{contract.broker};
        if(scope==Host::Scope::Console) { argv+={u"--worker"_s,u"${FARSIDE_CONSOLE_WORKER}"_s};result.environment.append(u"FARSIDE_CONSOLE_WORKER="_s+contract.worker); }
        for(const auto &key:Host::keys(scope)) {
            const auto env=Host::environmentName(scope,key);result.environment.append(env+u"="_s+Host::defaults(scope)[key].toString());
            argv+={u"--"_s+option(key),u"${"_s+env+u"}"_s};
        }
        if(scope==Host::Scope::Virtual) argv=QStringList{u"/usr/bin/env"_s,u"-i"_s,u"PATH=/usr/bin:/bin"_s,u"LANG=C.UTF-8"_s,u"HOME=/var/lib/farside/virtual-host"_s}+argv;
        result.commands.append({argv[0],argv,false});return result;
    }
    static Runtime::Process process(Host::Scope scope,const QVariantMap &values) {
        const auto contract=Runtime::installedContract(scope); Runtime::Process result;
        result.pid=42;result.startTicks=123;result.pidIdentity=444;result.root=result.executableMatches=result.alive=true;
        result.controlGroup=u"/system.slice/"_s+Runtime::unitName(scope); result.argv={contract.broker};
        if(scope==Host::Scope::Console) result.argv+={u"--worker"_s,contract.worker};
        for(const auto &key:Host::keys(scope)) result.argv.append(u"--"_s+option(key)+u"="_s+values[key].toString());
        return result;
    }
private Q_SLOTS:
    void currentFilesLoadedUnitAndActualArguments_data() { QTest::addColumn<bool>("console");QTest::newRow("Console")<<true;QTest::newRow("Virtual")<<false; }
    void currentFilesLoadedUnitAndActualArguments() {
        QFETCH(bool,console); const auto scope=console?Host::Scope::Console:Host::Scope::Virtual;
        const auto u=unit(scope); const auto contract=Runtime::installedContract(scope);
        const QByteArray file=console?"FARSIDE_CONSOLE_QUALITY=91\nPRIVATE_KEY=fixture-secret-only\n":"FARSIDE_VIRTUAL_QUALITY=91\nPRIVATE_KEY=fixture-secret-only\n";
        auto expected=Host::defaults(scope);expected[u"Quality"_s]=u"91"_s;
        const auto projected=Runtime::project(scope,u,{{true,file,{}}},contract);
        QVERIFY2(projected.verified(),qPrintable(projected.reasons.join(u',')));QCOMPARE(projected.fields.values,expected);QVERIFY(!projected.custom);
        const auto actual=process(scope,expected);const auto value=Runtime::summarize(scope,u,projected,actual,expected,QString(64,u'a'),contract);
        QVERIFY(Runtime::validPublic(scope,value));QCOMPARE(value[u"state"_s].toString(),u"verified"_s);
        QVERIFY(value[u"runningVerified"_s].toBool());QCOMPARE(value[u"running"_s].toObject().toVariantMap(),expected);
        QVERIFY(!QJsonDocument(value).toJson().contains("fixture-secret"));QVERIFY(!QJsonDocument(value).toJson().contains("argv"));
        auto changed=expected;changed[u"Quality"_s]=u"55"_s;
        const auto pending=Runtime::summarize(scope,u,projected,actual,changed,QString(64,u'b'),contract);
        QCOMPARE(pending[u"state"_s].toString(),u"different"_s);QCOMPARE(pending[u"runningDifferences"_s].toArray().size(),1);
    }
    void orderedFilesUnknownBytesAndUnsetEnvironment() {
        auto u=unit(Host::Scope::Console);const auto contract=Runtime::installedContract(Host::Scope::Console);
        u.files.append({u"/etc/farside/admin.conf"_s,false});u.dropIns.append(u"/etc/systemd/system/farside-console-host.service.d/custom.conf"_s);
        const QList<Runtime::File> files{{true,"FARSIDE_CONSOLE_QUALITY=bad\nPRIVATE=fixture-secret-only\n",{}},
            {true,"FARSIDE_CONSOLE_QUALITY=92\nFARSIDE_CONSOLE_CERTIFICATE='/etc/farside/a$literal.crt'\n",{}}};
        auto projection=Runtime::project(Host::Scope::Console,u,files,contract);QVERIFY(projection.verified());QVERIFY(projection.custom);
        QCOMPARE(projection.fields.values[u"Quality"_s].toString(),u"92"_s);QCOMPARE(projection.fields.values[u"Certificate"_s].toString(),u"/etc/farside/a$literal.crt"_s);
        u.unsetEnvironment={u"FARSIDE_CONSOLE_QUALITY=90"_s};QVERIFY(Runtime::project(Host::Scope::Console,u,files,contract).verified());
        u.unsetEnvironment={u"FARSIDE_CONSOLE_QUALITY=92"_s};projection=Runtime::project(Host::Scope::Console,u,files,contract);
        QVERIFY(!projection.verified());QVERIFY(projection.reasons.contains(u"unknown-expansion"_s));
        u.unsetEnvironment={u"FARSIDE_CONSOLE_CERTIFICATE"_s};QVERIFY(!Runtime::project(Host::Scope::Console,u,files,contract).verified());
    }
    void customLiteralCommandsAndMissingFileSemantics() {
        auto u=unit(Host::Scope::Console);const auto contract=Runtime::installedContract(Host::Scope::Console);
        auto projection=Runtime::project(Host::Scope::Console,u,{{false,{},{}}},contract);QVERIFY(projection.verified());
        const auto index=u.commands[0].arguments.indexOf(u"--port"_s);u.commands[0].arguments[index+1]=u"3391"_s;
        projection=Runtime::project(Host::Scope::Console,u,{{false,{},{}}},contract);QVERIFY(projection.verified());QVERIFY(projection.custom);
        const auto value=Runtime::summarize(Host::Scope::Console,u,projection,process(Host::Scope::Console,Host::defaults(Host::Scope::Console)),
            Host::defaults(Host::Scope::Console),QString(64,u'a'),contract);QCOMPARE(value[u"state"_s].toString(),u"custom"_s);
        u.files[0].optional=false;QVERIFY(!Runtime::project(Host::Scope::Console,u,{{false,{},{}}},contract).verified());
        QVERIFY(!Runtime::project(Host::Scope::Console,u,{{true,{},u"unsafe"_s}},contract).verified());
        QVERIFY(!Runtime::project(Host::Scope::Console,u,{},contract).verified());
        u.commands[0].path=u"/bin/sh"_s;QVERIFY(!Runtime::project(Host::Scope::Console,u,{{true,{},{}}},contract).verified());
    }
    void activeIdentityAndMissingArgumentsNeverGuessDefaults_data() {
        QTest::addColumn<QString>("fault");
        for(const auto &name:{"pid","uid","exe","dead","ticks","pidfd","cgroup","missing","duplicate","unknown","wrong-bool","worker"})QTest::newRow(name)<<QString::fromLatin1(name);
    }
    void activeIdentityAndMissingArgumentsNeverGuessDefaults() {
        QFETCH(QString,fault);const auto scope=Host::Scope::Console;const auto u=unit(scope);const auto contract=Runtime::installedContract(scope);
        const auto projection=Runtime::project(scope,u,{{false,{},{}}},contract);auto actual=process(scope,Host::defaults(scope));
        if(fault==u"pid")actual.pid=43;else if(fault==u"uid")actual.root=false;else if(fault==u"exe")actual.executableMatches=false;
        else if(fault==u"dead")actual.alive=false;else if(fault==u"ticks")actual.startTicks=0;else if(fault==u"pidfd")actual.pidIdentity=0;
        else if(fault==u"cgroup")actual.controlGroup=u"/wrong"_s;else if(fault==u"missing")actual.argv.removeLast();
        else if(fault==u"duplicate")actual.argv.append(u"--quality=92"_s);else if(fault==u"unknown")actual.argv+={u"--password"_s,u"fixture-secret-only"_s};
        else if(fault==u"wrong-bool") { const int i=actual.argv.indexOf(u"--adaptive-quality=false"_s);QVERIFY(i>=0);actual.argv[i]=u"--adaptive-quality=FALSE"_s; }
        else if(fault==u"worker")actual.argv[2]=u"/tmp/foreign-worker"_s;
        const auto value=Runtime::summarize(scope,u,projection,actual,Host::defaults(scope),QString(64,u'a'),contract);
        QVERIFY(Runtime::validPublic(scope,value));QVERIFY(!value[u"runningVerified"_s].toBool());QVERIFY(value[u"state"_s].toString()!=u"verified");
        QVERIFY(!QJsonDocument(value).toJson().contains("fixture-secret"));
        if(fault==u"missing")QVERIFY(value[u"missing"_s].toArray().size()>0);
    }
    void inheritedReloadAndInactiveStateStaySeparate() {
        const auto scope=Host::Scope::Console;auto u=unit(scope);const auto contract=Runtime::installedContract(scope);
        u.needsReload=true;auto projection=Runtime::project(scope,u,{{false,{},{}}},contract);QVERIFY(!projection.verified());
        auto value=Runtime::summarize(scope,u,projection,process(scope,Host::defaults(scope)),Host::defaults(scope),QString(64,u'a'),contract);
        QVERIFY(value[u"runningVerified"_s].toBool());QVERIFY(!value[u"configuredVerified"_s].toBool());QCOMPARE(value[u"state"_s].toString(),u"partial"_s);
        u.needsReload=false;u.passEnvironment={u"FARSIDE_CONSOLE_PORT"_s};QVERIFY(!Runtime::project(scope,u,{{false,{},{}}},contract).verified());
        u.passEnvironment.clear();u.pamName=u"login"_s;QVERIFY(!Runtime::project(scope,u,{{false,{},{}}},contract).verified());
        u.pamName.clear();u.activeState=u"inactive"_s;u.subState=u"dead"_s;u.pid=0;projection=Runtime::project(scope,u,{{false,{},{}}},contract);
        value=Runtime::summarize(scope,u,projection,{},Host::defaults(scope),QString(64,u'a'),contract);QVERIFY(Runtime::validPublic(scope,value));
        QCOMPARE(value[u"state"_s].toString(),u"inactive"_s);QVERIFY(value[u"configuredVerified"_s].toBool());QVERIFY(!value[u"runningVerified"_s].toBool());
        value=Runtime::summarize(scope,u,projection,{},Host::defaults(scope),QString(64,u'a'),contract,u"stale"_s);
        QCOMPARE(value[u"state"_s].toString(),u"stale"_s);QVERIFY(value[u"configured"_s].toObject().isEmpty());
    }
    void publicSchemaRefusesPrivateAndMistypedFields() {
        const auto scope=Host::Scope::Console;const auto u=unit(scope);const auto contract=Runtime::installedContract(scope);
        const auto projection=Runtime::project(scope,u,{{false,{},{}}},contract);
        const auto value=Runtime::summarize(scope,u,projection,process(scope,Host::defaults(scope)),Host::defaults(scope),QString(64,u'a'),contract);
        QVERIFY(Runtime::validPublic(scope,value));auto bad=value;bad[u"argv"_s]=u"fixture-secret-only"_s;QVERIFY(!Runtime::validPublic(scope,bad));
        bad=value;bad[u"scope"_s]=u"virtual"_s;QVERIFY(!Runtime::validPublic(scope,bad));bad=value;bad[u"unit"_s]=u"other.service"_s;QVERIFY(!Runtime::validPublic(scope,bad));
        bad=value;bad[u"pid"_s]=u"42"_s;QVERIFY(!Runtime::validPublic(scope,bad));bad=value;bad[u"configuredVerified"_s]=1;QVERIFY(!Runtime::validPublic(scope,bad));
        bad=value;auto fields=bad[u"running"_s].toObject();fields[u"Password"_s]=u"fixture-secret-only"_s;bad[u"running"_s]=fields;QVERIFY(!Runtime::validPublic(scope,bad));
        QVERIFY(!Runtime::validPublic(Host::Scope::VirtualSession,value));
        bad=value;bad[u"runningVerified"_s]=false;QVERIFY(!Runtime::validPublic(scope,bad));
        bad=value;bad[u"state"_s]=u"stale"_s;QVERIFY(!Runtime::validPublic(scope,bad));
        bad=value;bad[u"custom"_s]=true;QVERIFY(!Runtime::validPublic(scope,bad));
        bad=value;bad[u"missing"_s]=QJsonArray{u"Port"_s};QVERIFY(!Runtime::validPublic(scope,bad));
        bad=value;bad[u"needsReload"_s]=true;QVERIFY(!Runtime::validPublic(scope,bad));
        bad=value;bad[u"pid"_s]=0;QVERIFY(!Runtime::validPublic(scope,bad));
        bad=value;bad[u"state"_s]=u"different"_s;QVERIFY(!Runtime::validPublic(scope,bad));
    }
};
QTEST_GUILESS_MAIN(BrokerHostRuntimeTest)
#include "BrokerHostRuntimeTest.moc"
