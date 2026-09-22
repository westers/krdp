// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionMaintenanceWriterPolicy.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusVirtualObject>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QProcess>
#include <QTest>
#include <QUuid>

struct WireCommand {
    QString path = QStringLiteral("/approved/guard");
    QStringList argv{QStringLiteral("/approved/guard"), QStringLiteral("check")}, flags;
};
Q_DECLARE_METATYPE(WireCommand)
using WireCommands = QList<WireCommand>;
Q_DECLARE_METATYPE(WireCommands)
QDBusArgument &operator<<(QDBusArgument &a, const WireCommand &c) {
    a.beginStructure(); a << c.path << c.argv << c.flags << quint64(0) << quint64(0) << quint64(0) << quint64(0)
        << quint32(0) << qint32(0) << qint32(0); a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, WireCommand &c) {
    quint64 x; quint32 p; qint32 n;
    a.beginStructure(); a >> c.path >> c.argv >> c.flags >> x >> x >> x >> x >> p >> n >> n; a.endStructure(); return a;
}
struct WireSuccess { QList<qint32> statuses, signalNumbers; };
Q_DECLARE_METATYPE(WireSuccess)
QDBusArgument &operator<<(QDBusArgument &a, const WireSuccess &v) {
    a.beginStructure(); a << v.statuses << v.signalNumbers; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, WireSuccess &v) {
    a.beginStructure(); a >> v.statuses >> v.signalNumbers; a.endStructure(); return a;
}
struct DuplicateProperties {};
Q_DECLARE_METATYPE(DuplicateProperties)
QDBusArgument &operator<<(QDBusArgument &a, const DuplicateProperties &) {
    a.beginMap(QMetaType::fromType<QString>(), QMetaType::fromType<QDBusVariant>());
    for (int i = 0; i < 2; ++i) {
        a.beginMapEntry(); a << QStringLiteral("Transient") << QDBusVariant(false); a.endMapEntry();
    }
    a.endMap(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, DuplicateProperties &) {
    a.beginMap();
    while (!a.atEnd()) { QString k; QDBusVariant v; a.beginMapEntry(); a >> k >> v; a.endMapEntry(); }
    a.endMap(); return a;
}
namespace {
class Fixture : public QDBusVirtualObject {
public:
    QList<QVariant> args;
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &m, const QDBusConnection &bus) override {
        return bus.send(m.createReply(args));
    }
};
// Only a disposable private daemon; no system/session host bus access.
struct Harness {
    QProcess daemon;
    QString sn = QUuid::createUuid().toString(), cn = QUuid::createUuid().toString();
    QDBusConnection server{sn}, client{cn};
    Fixture fixture;
    bool ready = false;
    Harness() {
        daemon.start(QStringLiteral("dbus-daemon"), {QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1")});
        if (!daemon.waitForStarted(3000) || !daemon.waitForReadyRead(3000)) return;
        const auto address = QString::fromUtf8(daemon.readLine().trimmed());
        if (!address.startsWith(QStringLiteral("unix:"))) return;
        server = QDBusConnection::connectToBus(address, sn); client = QDBusConnection::connectToBus(address, cn);
        ready = server.isConnected() && client.isConnected() && server.registerVirtualObject(QStringLiteral("/fixture"), &fixture);
    }
    ~Harness() {
        server.unregisterObject(QStringLiteral("/fixture"));
        QDBusConnection::disconnectFromBus(cn); QDBusConnection::disconnectFromBus(sn);
        daemon.terminate(); if (!daemon.waitForFinished(3000)) { daemon.kill(); daemon.waitForFinished(3000); }
    }
    QDBusMessage reply(const QList<QVariant> &args) {
        fixture.args = args;
        auto m = QDBusMessage::createMethodCall(server.baseService(), QStringLiteral("/fixture"), QStringLiteral("org.example.Fixture"), QStringLiteral("Read"));
        m.setAutoStartService(false);
        auto call = client.asyncCall(m, 1000);
        QElapsedTimer elapsed; elapsed.start();
        while (!call.isFinished() && elapsed.elapsed() < 1500) QTest::qWait(1);
        return call.isFinished() ? call.reply() : QDBusMessage();
    }
};
}
namespace KRdp {
class VirtualSessionMaintenanceWriterPolicyTest : public QObject {
    Q_OBJECT
    using P = VirtualSessionMaintenanceWriterPolicy;
    static QVariantMap service() {
        QVariantMap m;
        for (const char *s : {"ExecConditionEx", "ExecStartPreEx", "ExecStartEx", "ExecStartPostEx", "ExecReloadEx", "ExecReloadPostEx", "ExecStopEx", "ExecStopPostEx"})
            m.insert(QString::fromLatin1(s), QVariant::fromValue(WireCommands{}));
        m.insert(QStringLiteral("ExecStartPreEx"), QVariant::fromValue(WireCommands{WireCommand{}}));
        m.insert(QStringLiteral("SuccessExitStatus"), QVariant::fromValue(WireSuccess{}));
        return m;
    }
    static QVariantMap unit() {
        return {{QStringLiteral("Id"), QStringLiteral("fixture.service")}, {QStringLiteral("LoadState"), QStringLiteral("loaded")},
            {QStringLiteral("ActiveState"), QStringLiteral("inactive")}, {QStringLiteral("SubState"), QStringLiteral("dead")},
            {QStringLiteral("FragmentPath"), QStringLiteral("/usr/lib/systemd/system/fixture.service")}, {QStringLiteral("SourcePath"), QString()},
            {QStringLiteral("DropInPaths"), QStringList{}}, {QStringLiteral("Transient"), false}, {QStringLiteral("NeedDaemonReload"), false}};
    }
private Q_SLOTS:
    void initTestCase() {
        qDBusRegisterMetaType<WireCommand>(); qDBusRegisterMetaType<WireCommands>(); qDBusRegisterMetaType<WireSuccess>();
        qDBusRegisterMetaType<DuplicateProperties>();
    }
    void actualWireSchemas() {
        Harness h; QVERIFY(h.ready);
        auto s = P::service(h.reply({service()})); QVERIFY(s);
        QCOMPARE(s->commands.size(), 8);
        const auto c = s->commands.value(QStringLiteral("ExecStartPreEx")); QCOMPARE(c.size(), 1);
        QCOMPARE(c[0].path, QStringLiteral("/approved/guard")); QCOMPARE(c[0].argv, WireCommand{}.argv); QVERIFY(c[0].flags.isEmpty());
        QVERIFY(s->successStatuses.isEmpty()); QVERIFY(P::unit(h.reply({unit()})));
    }
    void flagsArePreservedNotApproved() {
        Harness h; QVERIFY(h.ready); auto m = service(); WireCommand c;
        c.flags = {QStringLiteral("ignore-failure"), QStringLiteral("via-shell")};
        m[QStringLiteral("ExecStartEx")] = QVariant::fromValue(WireCommands{c});
        auto s = P::service(h.reply({m})); QVERIFY(s);
        QCOMPARE(s->commands.value(QStringLiteral("ExecStartEx"))[0].flags, c.flags);
    }
    void duplicateDictionaryKeysRefused() {
        Harness h; QVERIFY(h.ready);
        const auto reply = h.reply({QVariant::fromValue(DuplicateProperties{})});
        QCOMPARE(reply.signature(), QStringLiteral("a{sv}"));
        QVERIFY(!P::properties(reply));
    }
    void malformed_data() {
        QTest::addColumn<QString>("kind");
        for (const char *s : {"wrong-reply", "extra-reply", "missing", "wrong-command-type", "unknown-flag", "duplicate-flag", "empty-argv", "too-many", "wrong-success-type", "negative-status", "duplicate-status"})
            QTest::newRow(s) << QString::fromLatin1(s);
    }
    void malformed() {
        QFETCH(QString, kind); Harness h; QVERIFY(h.ready); auto m = service(); WireCommand c;
        if (kind == QStringLiteral("wrong-reply")) { QVERIFY(!P::service(h.reply({true}))); return; }
        if (kind == QStringLiteral("extra-reply")) { QVERIFY(!P::service(h.reply({m, true}))); return; }
        if (kind == QStringLiteral("missing")) m.remove(QStringLiteral("ExecStopPostEx"));
        if (kind == QStringLiteral("wrong-command-type")) m[QStringLiteral("ExecStartEx")] = QStringList{};
        if (kind == QStringLiteral("unknown-flag")) c.flags = {QStringLiteral("new-flag")};
        if (kind == QStringLiteral("duplicate-flag")) c.flags = {QStringLiteral("ignore-failure"), QStringLiteral("ignore-failure")};
        if (kind == QStringLiteral("empty-argv")) c.argv.clear();
        if (kind == QStringLiteral("unknown-flag") || kind == QStringLiteral("duplicate-flag") || kind == QStringLiteral("empty-argv"))
            m[QStringLiteral("ExecStartEx")] = QVariant::fromValue(WireCommands{c});
        if (kind == QStringLiteral("too-many")) m[QStringLiteral("ExecStartEx")] = QVariant::fromValue(WireCommands(65));
        if (kind == QStringLiteral("wrong-success-type")) m[QStringLiteral("SuccessExitStatus")] = QStringList{};
        if (kind == QStringLiteral("negative-status")) m[QStringLiteral("SuccessExitStatus")] = QVariant::fromValue(WireSuccess{{-1}, {}});
        if (kind == QStringLiteral("duplicate-status")) m[QStringLiteral("SuccessExitStatus")] = QVariant::fromValue(WireSuccess{{1, 1}, {}});
        QVERIFY(!P::service(h.reply({m})));
    }
    void metadataStrictTypes() {
        Harness h; QVERIFY(h.ready);
        for (const auto &key : unit().keys()) {
            auto m = unit(); m.remove(key); QVERIFY(!P::unit(h.reply({m})));
            m = unit(); m[key] = quint32(0); QVERIFY(!P::unit(h.reply({m})));
        }
        auto m = unit(); m[QStringLiteral("Transient")] = true; m[QStringLiteral("NeedDaemonReload")] = true;
        auto u = P::unit(h.reply({m})); QVERIFY(u); QVERIFY(u->transient); QVERIFY(u->needReload);
        // Parsing this observation is deliberately not policy authorization.
    }
};
}
QTEST_GUILESS_MAIN(KRdp::VirtualSessionMaintenanceWriterPolicyTest)
#include "VirtualSessionMaintenanceWriterPolicyTest.moc"
