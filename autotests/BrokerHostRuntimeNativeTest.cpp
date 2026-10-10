// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Sol only: root/private dbus-run-session. No installed service mutations.
#include "BrokerHostRuntimeReader.h"
#include <QDBusVirtualObject>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QSysInfo>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
using namespace Qt::StringLiterals;
namespace Runtime = KRdp::BrokerHostRuntime;
namespace Host = KRdp::BrokerHostSettings;
struct RuntimeFile { QString path; bool optional = false; };
struct RuntimeExec {
    QString path; QStringList args; bool ignore=false; quint64 a=0,b=0,c=0,d=0; quint32 pid=0; int code=0,status=0;
};
Q_DECLARE_METATYPE(RuntimeFile)
Q_DECLARE_METATYPE(QList<RuntimeFile>)
Q_DECLARE_METATYPE(RuntimeExec)
Q_DECLARE_METATYPE(QList<RuntimeExec>)
QDBusArgument &operator<<(QDBusArgument &a,const RuntimeFile &v) { a.beginStructure();a<<v.path<<v.optional;a.endStructure();return a; }
const QDBusArgument &operator>>(const QDBusArgument &a,RuntimeFile &v) { a.beginStructure();a>>v.path>>v.optional;a.endStructure();return a; }
QDBusArgument &operator<<(QDBusArgument &a,const RuntimeExec &v) { a.beginStructure();a<<v.path<<v.args<<v.ignore<<v.a<<v.b<<v.c<<v.d<<v.pid<<v.code<<v.status;a.endStructure();return a; }
const QDBusArgument &operator>>(const QDBusArgument &a,RuntimeExec &v) { a.beginStructure();a>>v.path>>v.args>>v.ignore>>v.a>>v.b>>v.c>>v.d>>v.pid>>v.code>>v.status;a.endStructure();return a; }
QString option(const QString &key) {
    static const QMap<QString,QString> names{{u"Address"_s,u"address"_s},{u"Port"_s,u"port"_s},{u"Certificate"_s,u"certificate"_s},
        {u"CertificateKey"_s,u"certificate-key"_s},{u"Quality"_s,u"quality"_s},{u"AdaptiveQuality"_s,u"adaptive-quality"_s},
        {u"PreferAudioQuality"_s,u"prefer-audio-quality"_s},{u"StandardClientMedia"_s,u"standard-client-media"_s},
        {u"CameraLoopbackDevice"_s,u"camera-loopback-device"_s},{u"SoftwareEncoding"_s,u"software-encoding"_s},{u"SoftwareAvc"_s,u"software-avc"_s},{u"SoftwareHevc"_s,u"software-hevc"_s},{u"SoftwareAv1"_s,u"software-av1"_s},
        {u"Av1Tiles"_s,u"av1-tiles"_s},{u"VaapiDriver"_s,u"vaapi-driver"_s}};return names.value(key);
}
class RuntimeManager : public QDBusVirtualObject {
public:
    QDBusConnection bus;
    QString directory;
    int reads=0;
    std::unique_ptr<QDBusConnection> replacement;
    RuntimeManager(const QDBusConnection &connection,const QString &path) : bus(connection),directory(path) {}
    QString introspect(const QString &) const override { return {}; }
    QJsonObject configuration() const { QFile file(directory+u"/mode.json"_s);if(!file.open(QIODevice::ReadOnly))return {};return QJsonDocument::fromJson(file.readAll()).object(); }
    bool handleMessage(const QDBusMessage &message,const QDBusConnection &connection) override {
        const auto config=configuration();const auto mode=config[u"mode"_s].toString();
        const auto scope=message.path().endsWith(u"/console")?Host::Scope::Console:Host::Scope::Virtual;
        const auto name=Runtime::unitName(scope);const auto contract=Runtime::installedContract(scope);
        if(message.interface()==u"org.freedesktop.systemd1.Manager") {
            if(message.member()!=u"LoadUnit")return connection.send(message.createErrorReply(u"org.freedesktop.DBus.Error.AccessDenied"_s,u"mutation-forbidden"_s));
            const auto requested=message.arguments().value(0).toString();
            if(mode==u"missing" || (requested!=Runtime::unitName(Host::Scope::Console) && requested!=Runtime::unitName(Host::Scope::Virtual)))
                return connection.send(message.createErrorReply(u"org.freedesktop.systemd1.NoSuchUnit"_s,u"fixture-secret-only"_s));
            return connection.send(message.createReply(QVariant::fromValue(QDBusObjectPath(requested==Runtime::unitName(Host::Scope::Console)
                ?u"/org/freedesktop/systemd1/unit/console"_s:u"/org/freedesktop/systemd1/unit/virtual"_s))));
        }
        if(message.interface()!=u"org.freedesktop.DBus.Properties" || message.member()!=u"GetAll")return false;
        if(mode==u"denied")return connection.send(message.createErrorReply(u"org.freedesktop.DBus.Error.AccessDenied"_s,u"fixture-secret-only"_s));
        QVariantMap values;
        if(message.arguments().value(0).toString()==u"org.freedesktop.systemd1.Unit") {
            values={{u"Id"_s,name},{u"LoadState"_s,u"loaded"_s},{u"ActiveState"_s,mode==u"inactive"?u"inactive"_s:u"active"_s},
                {u"SubState"_s,mode==u"inactive"?u"dead"_s:u"running"_s},{u"FragmentPath"_s,QString(u"/usr/lib/systemd/system/"_s+name)},
                {u"NeedDaemonReload"_s,mode==u"reload"},{u"DropInPaths"_s,mode==u"custom"?QStringList{u"/etc/systemd/system/fixture.conf"_s}:QStringList{}}};
        } else {
            ++reads; QStringList environment{u"PRIVATE_ROOT_VALUE=fixture-secret-only"_s},args{contract.broker};
            if(scope==Host::Scope::Console) { environment.append(u"FARSIDE_CONSOLE_WORKER="_s+contract.worker);args+={u"--worker"_s,u"${FARSIDE_CONSOLE_WORKER}"_s}; }
            for(const auto &key:Host::keys(scope)) {
                const auto env=Host::environmentName(scope,key);environment.append(env+u"="_s+Host::defaults(scope)[key].toString());
                args+={u"--"_s+option(key),u"${"_s+env+u"}"_s};
            }
            if(scope==Host::Scope::Virtual)args=QStringList{u"/usr/bin/env"_s,u"-i"_s,u"PATH=/usr/bin:/bin"_s,u"LANG=C.UTF-8"_s,u"HOME=/var/lib/farside/virtual-host"_s}+args;
            const auto path=config[u"file"_s].toString(u"/etc/farside/"_s+Host::fileName(scope));
            QList<RuntimeFile> files{{path,true}};
            if(mode==u"custom")files.append({u"/etc/farside/runtime-extra.conf"_s,true});
            if(mode==u"literal") { const auto index=args.indexOf(u"--quality"_s);args[index+1]=u"91"_s; }
            values={{u"MainPID"_s,mode==u"malformed"?QVariant(u"42"_s):QVariant(quint32(mode==u"inactive"?0:mode==u"pid-change"&&reads>1?43:42))},
                {u"ExecMainStartTimestampMonotonic"_s,QVariant::fromValue(quint64(123))},{u"User"_s,u"root"_s},{u"Type"_s,u"exec"_s},
                {u"ControlGroup"_s,QString(u"/system.slice/"_s+name)},{u"PAMName"_s,QString()},{u"Environment"_s,environment},
                {u"PassEnvironment"_s,QStringList{}},{u"UnsetEnvironment"_s,QStringList{}},
                {u"EnvironmentFiles"_s,QVariant::fromValue(files)},
                {u"ExecStart"_s,QVariant::fromValue(QList<RuntimeExec>{{args[0],args,false,0,0,0,0,0,0,0}})}};
            if(mode==u"owner-change"&&reads>1) {
                bus.unregisterService(u"org.freedesktop.systemd1"_s);
                replacement=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(QDBusConnection::SessionBus,u"runtime-replacement"_s));
                if(!replacement->registerService(u"org.freedesktop.systemd1"_s))return false;
            }
        }
        return connection.send(message.createReply(QVariantList{values}));
    }
};
class BrokerHostRuntimeNativeTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory{u"/run/farside-runtime-native-XXXXXX"_s};
    QProcess server;
    std::unique_ptr<QDBusConnection> client;
    void mode(const QString &value,const QString &path={}) {
        QFile file(directory.filePath(u"mode.json"_s));QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
        auto config=QJsonObject{{u"mode"_s,value}};if(!path.isEmpty())config[u"file"_s]=path;
        const auto data=QJsonDocument(config).toJson();QCOMPARE(file.write(data),data.size());
    }
    Runtime::ReaderDependencies dependencies(Host::Scope scope) {
        Runtime::ReaderDependencies result;result.requirePidOne=false;
        result.readFile=[scope](const QString &) { return Runtime::File{true,(Host::environmentName(scope,u"Quality"_s)+u"=91\nPRIVATE=fixture-secret-only\n"_s).toUtf8(),{}}; };
        result.readProcess=[](const Runtime::Unit &unit,const Runtime::Contract &contract) {
            Runtime::Process p;p.pid=unit.pid;p.startTicks=123;p.pidIdentity=444;p.root=p.alive=p.executableMatches=true;p.controlGroup=unit.controlGroup;
            const auto scope=unit.id==Runtime::unitName(Host::Scope::Console)?Host::Scope::Console:Host::Scope::Virtual;
            p.argv={contract.broker};if(scope==Host::Scope::Console)p.argv+={u"--worker"_s,contract.worker};
            for(const auto &key:Host::keys(scope))p.argv.append(u"--"_s+option(key)+u"="_s+(key==u"Quality"?u"91"_s:Host::defaults(scope)[key].toString()));
            return p;
        };return result;
    }
    QJsonObject sample(Host::Scope scope,const Runtime::ReaderDependencies &deps) {
        auto stored=Host::defaults(scope);stored[u"Quality"_s]=u"91"_s;
        return Runtime::inspect(scope,stored,QString(64,u'a'),*client,deps);
    }
private Q_SLOTS:
    void initTestCase() {
        QVERIFY2(QSysInfo::machineHostName().split(u'.')[0]==u"sol" && !getuid() && !geteuid()
            && qEnvironmentVariable("FARSIDE_RUNTIME_PRIVATE_BUS")==u"1", "Root/Sol/private dbus-run-session only");
        QVERIFY(directory.isValid());client=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(QDBusConnection::SessionBus,u"runtime-test-client"_s));
        QVERIFY(client->isConnected());
    }
    void init() {
        mode(u"success"_s);server.setProgram(QCoreApplication::applicationFilePath());server.setArguments({u"--manager"_s,directory.path()});server.start();
        QVERIFY(server.waitForStarted(3000));QVERIFY(server.waitForReadyRead(3000));QCOMPARE(server.readLine().trimmed(),QByteArray("ready"));
    }
    void typedPrivateScopeQueries_data() { QTest::addColumn<bool>("console");QTest::newRow("Console")<<true;QTest::newRow("Virtual")<<false; }
    void typedPrivateScopeQueries() {
        QFETCH(bool,console);const auto scope=console?Host::Scope::Console:Host::Scope::Virtual;
        const auto value=sample(scope,dependencies(scope));QVERIFY(Runtime::validPublic(scope,value));
        QCOMPARE(value[u"state"_s].toString(),u"verified"_s);QVERIFY(value[u"runningVerified"_s].toBool());
        QCOMPARE(value[u"running"_s].toObject()[u"Quality"_s].toString(),u"91"_s);
        QVERIFY(!QJsonDocument(value).toJson().contains("fixture-secret"));
    }
    void customMissingInactiveMalformedDeniedAndReplacement_data() {
        QTest::addColumn<QString>("name");QTest::addColumn<QString>("state");
        QTest::newRow("drop-in")<<u"custom"_s<<u"custom"_s;QTest::newRow("literal")<<u"literal"_s<<u"custom"_s;
        QTest::newRow("missing")<<u"missing"_s<<u"missing"_s;QTest::newRow("inactive")<<u"inactive"_s<<u"inactive"_s;
        QTest::newRow("reload")<<u"reload"_s<<u"partial"_s;QTest::newRow("malformed")<<u"malformed"_s<<u"malformed"_s;
        QTest::newRow("denied")<<u"denied"_s<<u"denied"_s;QTest::newRow("pid replacement")<<u"pid-change"_s<<u"stale"_s;
        QTest::newRow("manager replacement")<<u"owner-change"_s<<u"stale"_s;
    }
    void customMissingInactiveMalformedDeniedAndReplacement() {
        QFETCH(QString,name);QFETCH(QString,state);mode(name);
        const auto value=sample(Host::Scope::Console,dependencies(Host::Scope::Console));QVERIFY(Runtime::validPublic(Host::Scope::Console,value));
        QCOMPARE(value[u"state"_s].toString(),state);QVERIFY(!QJsonDocument(value).toJson().contains("fixture-secret"));
        if(state==u"stale" || state==u"denied" || state==u"malformed") { QVERIFY(!value[u"runningVerified"_s].toBool());QVERIFY(value[u"running"_s].toObject().isEmpty()); }
    }
    void fileAndProcessChangeInvalidateSample() {
        auto deps=dependencies(Host::Scope::Console);int reads=0;
        deps.readFile=[&](const QString &) { return Runtime::File{true,++reads==1?QByteArray("FARSIDE_CONSOLE_QUALITY=91\n"):QByteArray("FARSIDE_CONSOLE_QUALITY=92\n"),{}}; };
        QCOMPARE(sample(Host::Scope::Console,deps)[u"state"_s].toString(),u"stale"_s);
        deps=dependencies(Host::Scope::Console);reads=0;
        deps.readFile=[&](const QString &) { return Runtime::File{true,"FARSIDE_CONSOLE_QUALITY=91\n",{},QString::number(++reads)}; };
        QCOMPARE(sample(Host::Scope::Console,deps)[u"state"_s].toString(),u"stale"_s); // replacement with identical bytes
        deps=dependencies(Host::Scope::Console);const auto original=deps.readProcess;int probes=0;
        deps.readProcess=[&](const auto &unit,const auto &contract) { auto p=original(unit,contract);p.startTicks=++probes;return p; };
        QCOMPARE(sample(Host::Scope::Console,deps)[u"state"_s].toString(),u"stale"_s);
        deps=dependencies(Host::Scope::Console);probes=0;
        deps.readProcess=[&](const auto &unit,const auto &contract) { auto p=original(unit,contract);p.executableStamp=QString::number(++probes);return p; };
        QCOMPARE(sample(Host::Scope::Console,deps)[u"state"_s].toString(),u"stale"_s);
    }
    void actualBoundedRootFileChecks() {
        const auto path=directory.filePath(u"environment"_s);QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner));file.write("FARSIDE_CONSOLE_QUALITY=91\nPRIVATE=fixture-secret-only\n");file.close();
        mode(u"success"_s,path);auto deps=dependencies(Host::Scope::Console);deps.readFile={};
        auto value=sample(Host::Scope::Console,deps);QCOMPARE(value[u"state"_s].toString(),u"custom"_s);QVERIFY(value[u"configuredVerified"_s].toBool());
        QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::WriteOther));value=sample(Host::Scope::Console,deps);QCOMPARE(value[u"state"_s].toString(),u"partial"_s);
        QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner));const auto hard=directory.filePath(u"hard"_s);QVERIFY(!::link(QFile::encodeName(path).constData(),QFile::encodeName(hard).constData()));
        QVERIFY(!sample(Host::Scope::Console,deps)[u"configuredVerified"_s].toBool());QVERIFY(QFile::remove(hard));
        QVERIFY(!::chown(QFile::encodeName(path).constData(),1000,1000));QVERIFY(!sample(Host::Scope::Console,deps)[u"configuredVerified"_s].toBool());
        QVERIFY(!::chown(QFile::encodeName(path).constData(),0,0));
        const auto symlink=directory.filePath(u"symlink"_s);QVERIFY(!::symlink(QFile::encodeName(path).constData(),QFile::encodeName(symlink).constData()));
        mode(u"success"_s,symlink);QVERIFY(sample(Host::Scope::Console,deps)[u"configuredVerified"_s].toBool());
        QVERIFY(!::lchown(QFile::encodeName(symlink).constData(),1000,1000));QVERIFY(!sample(Host::Scope::Console,deps)[u"configuredVerified"_s].toBool());
        const auto fifo=directory.filePath(u"fifo"_s);QVERIFY(!::mkfifo(QFile::encodeName(fifo).constData(),0600));mode(u"success"_s,fifo);
        QVERIFY(!sample(Host::Scope::Console,deps)[u"configuredVerified"_s].toBool());
        const auto large=directory.filePath(u"large"_s);QFile oversized(large);QVERIFY(oversized.open(QIODevice::WriteOnly));oversized.setPermissions(QFile::ReadOwner|QFile::WriteOwner);oversized.write(QByteArray(65537,'x'));oversized.close();mode(u"success"_s,large);
        QVERIFY(!sample(Host::Scope::Console,deps)[u"configuredVerified"_s].toBool());
        QVERIFY(!QJsonDocument(value).toJson().contains("fixture-secret"));
    }
    void productionManagerOwnerAndInstalledReadOnlyProcesses() {
        auto deps=dependencies(Host::Scope::Console);deps.requirePidOne=true;
        QCOMPARE(sample(Host::Scope::Console,deps)[u"state"_s].toString(),u"denied"_s); // fake manager is not PID1
        for(const auto scope:{Host::Scope::Console,Host::Scope::Virtual}) {
            const auto actual=Runtime::inspect(scope,Host::defaults(scope),QString(64,u'a'),QDBusConnection::systemBus());
            QVERIFY2(Runtime::validPublic(scope,actual),"invalid actual installed public metadata");
            QVERIFY2(actual[u"runningVerified"_s].toBool(),qPrintable(actual[u"state"_s].toString()+u":"_s+QString::fromUtf8(QJsonDocument(actual[u"reasons"_s].toArray()).toJson(QJsonDocument::Compact))));
            QVERIFY(actual[u"pid"_s].toInt()>1);QCOMPARE(actual[u"running"_s].toObject()[u"Port"_s].toString(),scope==Host::Scope::Console?u"3391"_s:u"3395"_s);
            QVERIFY(!QJsonDocument(actual).toJson().contains("PRIVATE KEY"));
        }
    }
    void cleanup() {
        server.terminate();if(!server.waitForFinished(3000)) { server.kill();QVERIFY(server.waitForFinished(3000)); }
    }
};
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if(argc==3&&QString::fromLocal8Bit(argv[1])==u"--manager") {
        qDBusRegisterMetaType<RuntimeFile>();qDBusRegisterMetaType<QList<RuntimeFile>>();qDBusRegisterMetaType<RuntimeExec>();qDBusRegisterMetaType<QList<RuntimeExec>>();
        auto bus=QDBusConnection::connectToBus(QDBusConnection::SessionBus,u"runtime-manager"_s);
        RuntimeManager manager(bus,QString::fromLocal8Bit(argv[2]));
        if(!bus.isConnected()||!bus.registerService(u"org.freedesktop.systemd1"_s)||!bus.registerVirtualObject(u"/org/freedesktop/systemd1"_s,&manager,QDBusConnection::SubPath))return 1;
        QFile output;if(!output.open(stdout,QIODevice::WriteOnly))return 1;output.write("ready\n");output.flush();return app.exec();
    }
    BrokerHostRuntimeNativeTest test;return QTest::qExec(&test,argc,argv);
}
#include "BrokerHostRuntimeNativeTest.moc"
