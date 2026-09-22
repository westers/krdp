// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionMaintenanceWriterPolicy.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusVirtualObject>
#include <QDBusVariant>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QProcess>
#include <QTest>
#include <QUuid>
#include "VirtualSessionRuntimeProfile.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QThread>
#include <sys/stat.h>
#include <unistd.h>
#include <limits>
#include <utility>

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
// Test-only generic wire emitter. Tuple layouts are independently listed here;
// production parsing never calls this emitter or executes fixture commands.
const QByteArray tupleSignatures[] = {"(ss)", "(sb)", "(ssbt)", "(bs)", "(bas)", "(tus)", "(aiai)",
    "(sasasttttuii)", "(sbbsi)", "(say)", "(sba(ss))", "(ssba(ss))", "(sst)", "(iayu)", "(iiqq)", "(iiss)"};
QMap<QByteArray, QMetaType> wireTypes;
QByteArray takeType(const QByteArray &s, qsizetype &pos) {
    const auto begin = pos; const auto c = s[pos++];
    if (c == 'a') takeType(s, pos);
    else if (c == '(') { while (s[pos] != ')') takeType(s, pos); ++pos; }
    return s.mid(begin, pos-begin);
}
void emitWire(QDBusArgument &a, const QByteArray &sig, const QJsonValue &v) {
    if (sig == "s") a << v.toString();
    else if (sig == "b") a << v.toBool();
    else if (sig == "t") a << v.toString().toULongLong();
    else if (sig == "u") a << quint32(v.toDouble());
    else if (sig == "i") a << qint32(v.toDouble());
    else if (sig == "q") a << quint16(v.toDouble());
    else if (sig == "y") a << quint8(v.toDouble());
    else if (sig.startsWith('a')) {
        const auto element = sig.mid(1);
        a.beginArray(wireTypes.value(element));
        for (const auto &item : v.toArray()) emitWire(a, element, item);
        a.endArray();
    } else {
        a.beginStructure(); qsizetype pos = 1, index = 0;
        while (sig[pos] != ')') {
            const auto element = takeType(sig, pos); const auto values = v.toArray();
            emitWire(a, element, index < values.size() ? values[index] : QJsonValue()); ++index;
        }
        a.endStructure();
    }
}
template<size_t N> struct Tuple {};
template<size_t N> QDBusArgument &operator<<(QDBusArgument &a, const Tuple<N> &) { emitWire(a, tupleSignatures[N], QJsonArray{}); return a; }
template<size_t N> const QDBusArgument &operator>>(const QDBusArgument &a, Tuple<N> &) { return a; }
template<size_t N> void registerTuple() {
    const auto type = qDBusRegisterMetaType<Tuple<N>>(); wireTypes.insert(tupleSignatures[N], type);
    const auto list = qDBusRegisterMetaType<QList<Tuple<N>>>(); wireTypes.insert('a' + tupleSignatures[N], list);
}
template<size_t... N> void registerTuples(std::index_sequence<N...>) { (registerTuple<N>(), ...); }
void registerWireTypes() {
    wireTypes = {{"s", QMetaType::fromType<QString>()}, {"b", QMetaType::fromType<bool>()}, {"t", QMetaType::fromType<quint64>()},
        {"u", QMetaType::fromType<quint32>()}, {"i", QMetaType::fromType<qint32>()}, {"q", QMetaType::fromType<quint16>()},
        {"y", QMetaType::fromType<quint8>()}, {"as", QMetaType::fromType<QStringList>()}, {"ay", QMetaType::fromType<QByteArray>()}};
    wireTypes.insert("ai", qDBusRegisterMetaType<QList<qint32>>());
    registerTuples(std::make_index_sequence<std::size(tupleSignatures)>{});
}
QVariant wire(const QByteArray &sig, const QJsonValue &value) {
    QDBusArgument a; emitWire(a, sig, value); return QVariant::fromValue(a);
}
QJsonValue tree(const QByteArray &sig, const QJsonValue &v) {
    if (sig == "(bs)") return QJsonArray{v[QStringLiteral("ignoreFailure")], v[QStringLiteral("value")]};
    if (sig == "(bas)") return QJsonArray{v[QStringLiteral("allowList")], v[QStringLiteral("items")]};
    if (sig == "(tus)") return QJsonArray{v[QStringLiteral("bytes")], v[QStringLiteral("scale")], v[QStringLiteral("enforce")]};
    if (sig == "(aiai)") return QJsonArray{v[QStringLiteral("statuses")], v[QStringLiteral("signals")]};
    if (sig == "a(sasasttttuii)" || sig == "a(sbbsi)") {
        QJsonArray a;
        for (const auto &item : v.toArray()) {
            if (sig == "a(sasasttttuii)") a.append(QJsonArray{item[QStringLiteral("path")], item[QStringLiteral("argv")], item[QStringLiteral("flags")],
                QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"), 0, 0, 0});
            else a.append(QJsonArray{item[QStringLiteral("type")], item[QStringLiteral("trigger")], item[QStringLiteral("negate")], item[QStringLiteral("parameter")], 0});
        }
        return a;
    }
    return v;
}
class Fixture : public QDBusVirtualObject {
public:
    QList<QVariant> args;
    QVariantMap manager;
    QMap<QString, QVariantMap> units, services;
    int inventoryReads = 0;
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &m, const QDBusConnection &bus) override {
        const QString root = QStringLiteral("/org/freedesktop/systemd1");
        if (m.path().startsWith(root)) {
            if (m.member() == QStringLiteral("GetUnitByPID")) {
                auto label = QStringLiteral("krdp_2dmaintenance_2dvalidate_2eservice");
                return bus.send(m.createReply({QVariant::fromValue(QDBusObjectPath(root + QStringLiteral("/unit/") + label))}));
            }
            if (m.member() == QStringLiteral("GetUnit")) {
                ++inventoryReads; auto label = m.arguments().first().toString();
                label.replace(QStringLiteral("-"), QStringLiteral("_2d")); label.replace(QStringLiteral("."), QStringLiteral("_2e"));
                return bus.send(m.createReply({QVariant::fromValue(QDBusObjectPath(root + QStringLiteral("/unit/") + label))}));
            }
            if (m.member() == QStringLiteral("Get")) return bus.send(m.createReply({QVariant::fromValue(QDBusVariant(false))}));
            if (m.member() == QStringLiteral("GetAll")) {
                const auto iface = m.arguments().first().toString();
                const auto values = m.path() == root ? manager : iface.endsWith(QStringLiteral(".Unit")) ? units.value(m.path()) : services.value(m.path());
                return bus.send(m.createReply(QList<QVariant>{values}));
            }
            return false;
        }
        return bus.send(m.createReply(args));
    }
};
// Only a disposable private daemon; no system/session host bus access.
struct Harness {
    QProcess daemon;
    QString sn = QUuid::createUuid().toString(), cn = QUuid::createUuid().toString();
    QDBusConnection server{sn}, client{cn};
    Fixture fixture;
    QThread thread;
    bool ready = false;
    Harness() {
        daemon.start(QStringLiteral("dbus-daemon"), {QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1")});
        if (!daemon.waitForStarted(3000) || !daemon.waitForReadyRead(3000)) return;
        const auto address = QString::fromUtf8(daemon.readLine().trimmed());
        if (!address.startsWith(QStringLiteral("unix:"))) return;
        server = QDBusConnection::connectToBus(address, sn); client = QDBusConnection::connectToBus(address, cn);
        if (!server.isConnected() || !client.isConnected() || !server.registerService(QStringLiteral("org.freedesktop.systemd1"))) return;
        fixture.moveToThread(&thread); thread.start();
        QMetaObject::invokeMethod(&fixture, [&] {
            ready = server.registerVirtualObject(QStringLiteral("/fixture"), &fixture)
                && server.registerVirtualObject(QStringLiteral("/org/freedesktop/systemd1"), &fixture, QDBusConnection::SubPath);
        }, Qt::BlockingQueuedConnection);
    }
    ~Harness() {
        server.unregisterObject(QStringLiteral("/fixture"));
        server.unregisterObject(QStringLiteral("/org/freedesktop/systemd1"), QDBusConnection::UnregisterTree);
        if (thread.isRunning()) {
            auto *main = QThread::currentThread();
            QMetaObject::invokeMethod(&fixture, [&] { fixture.moveToThread(main); }, Qt::BlockingQueuedConnection);
            thread.quit(); thread.wait();
        }
        QDBusConnection::disconnectFromBus(cn); QDBusConnection::disconnectFromBus(sn);
        daemon.terminate(); if (!daemon.waitForFinished(3000)) { daemon.kill(); daemon.waitForFinished(3000); }
    }
    QDBusMessage reply(const QList<QVariant> &args) {
        QMetaObject::invokeMethod(&fixture, [&] { fixture.args = args; }, Qt::BlockingQueuedConnection);
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
    static QJsonValue sample(const P::Descriptor &d, const QString &unitName) {
        using R = P::Rule;
        const QByteArray sig(d.signature), name(d.name); QJsonValue v;
        if (sig == "s") v = QStringLiteral("");
        else if (sig == "b") v = false;
        else if (sig == "t") v = QStringLiteral("0");
        else if (sig == "q" || sig == "u" || sig == "i") v = 0;
        else if (sig == "(bs)") v = QJsonObject{{QStringLiteral("ignoreFailure"), false}, {QStringLiteral("value"), QStringLiteral("")}};
        else if (sig == "(bas)") v = QJsonObject{{QStringLiteral("allowList"), false}, {QStringLiteral("items"), QJsonArray{}}};
        else if (sig == "(ss)") v = QJsonArray{QStringLiteral("no"), QStringLiteral("")};
        else if (sig == "(tus)") v = QJsonObject{{QStringLiteral("bytes"), QStringLiteral("0")}, {QStringLiteral("scale"), 0}, {QStringLiteral("enforce"), QStringLiteral("no")}};
        else if (sig == "(aiai)") v = QJsonObject{{QStringLiteral("statuses"), QJsonArray{}}, {QStringLiteral("signals"), QJsonArray{}}};
        else v = QJsonArray{};
        if (d.rule == R::Loaded) v = QStringLiteral("loaded");
        if (d.rule == R::Id) v = unitName;
        if (d.rule == R::Fragment) v = QStringLiteral("/approved/unit.service");
        if (d.rule == R::No) v = QStringLiteral("no");
        if (d.rule == R::Init) v = QStringLiteral("init");
        if (d.rule == R::Max64) v = QStringLiteral("18446744073709551615");
        if (d.rule == R::Input) v = QStringLiteral("null");
        if (d.rule == R::Output || d.rule == R::Error) v = QStringLiteral("journal");
        if (name == "Names") v = QJsonArray{unitName};
        if (name == "Type") v = QStringLiteral("oneshot");
        if (name == "Restart") v = QStringLiteral("no");
        if (name == "AppArmorProfile") {
            QString label;
            if (unitName == QStringLiteral("apt-news.service")) label = QStringLiteral("ubuntu_pro_apt_news");
            if (unitName == QStringLiteral("esm-cache.service")) label = QStringLiteral("ubuntu_pro_esm_cache");
            v = QJsonObject{{QStringLiteral("ignoreFailure"), !label.isEmpty()}, {QStringLiteral("value"), QJsonValue(label)}};
        }
        return v;
    }
    static QJsonObject baseline(const QString &name) {
        QJsonObject result;
        for (const auto &d : P::descriptors()) {
            const auto group = QString::fromLatin1(d.group); if (group == QStringLiteral("manager")) continue;
            auto g = result[group].toObject(); g[QString::fromLatin1(d.name)] = sample(d, name); result[group] = g;
        }
        return result;
    }
    static QJsonObject document() {
        QJsonObject manager, writers;
        for (const auto &d : P::descriptors()) if (QByteArray(d.group) == "manager") manager[QString::fromLatin1(d.name)] = sample(d, QString());
        for (size_t i = 2; i < size_t(P::Inventory::Count); ++i) writers[P::inventoryName(i)] = baseline(P::inventoryName(i));
        QJsonObject waiter{{QStringLiteral("baseline"), baseline(P::inventoryName(1))}, {QStringLiteral("interpreter"), QStringLiteral("/approved/python")},
            {QStringLiteral("script"), QStringLiteral("/approved/waiter")}, {QStringLiteral("wrapper"), QStringLiteral("/approved/wrapper")},
            {QStringLiteral("backend"), QStringLiteral("/approved/backend")}, {QStringLiteral("logPaths"), QJsonArray{}}, {QStringLiteral("lockPaths"), QJsonArray{}}};
        return {{QStringLiteral("v"), 1}, {QStringLiteral("manager"), manager}, {QStringLiteral("coordinator"), baseline(P::inventoryName(0))},
            {QStringLiteral("writers"), writers}, {QStringLiteral("shutdownWaiter"), waiter}};
    }
    static QJsonValue baselineAt(const QJsonObject &doc, size_t i) {
        return i == 0 ? doc[QStringLiteral("coordinator")] : i == 1 ? doc[QStringLiteral("shutdownWaiter")].toObject()[QStringLiteral("baseline")]
            : doc[QStringLiteral("writers")].toObject()[P::inventoryName(i)];
    }
    static QVariantMap rawMap(const QJsonValue &base, bool unit) {
        QVariantMap result;
        for (const auto &d : P::descriptors()) {
            const QByteArray group(d.group);
            if (group == "manager" || (group == "unit") != unit || QByteArray(d.name) == "PermissionsStartOnly") continue;
            const auto value = base[QString::fromLatin1(d.group)][QString::fromLatin1(d.name)];
            result[QString::fromLatin1(d.name)] = wire(d.signature, tree(d.signature, value));
        }
        return result;
    }
    static QVariantMap rawManager(const QJsonObject &doc) {
        QVariantMap result;
        for (const auto &d : P::descriptors()) if (QByteArray(d.group) == "manager") {
            const auto value = doc[QStringLiteral("manager")].toObject()[QString::fromLatin1(d.name)];
            result[QString::fromLatin1(d.name)] = wire(d.signature, tree(d.signature, value));
        }
        return result;
    }
    static P::Inputs inputs(Harness &h, const QJsonObject &doc) {
        P::Inputs in; in.manager = h.reply({rawManager(doc)});
        for (size_t i = 0; i < in.units.size(); ++i) {
            in.units[i].unit = h.reply({rawMap(baselineAt(doc, i), true)});
            in.units[i].service = h.reply({rawMap(baselineAt(doc, i), false)});
        }
        return in;
    }
    static void change(QJsonObject &doc, const P::Descriptor &d, const QJsonValue &v, bool remove = false) {
        const auto group = QString::fromLatin1(d.group), name = QString::fromLatin1(d.name);
        if (group == QStringLiteral("manager")) {
            auto m = doc[group].toObject(); if (remove) m.remove(name); else m[name] = v; doc[group] = m;
        } else {
            auto b = doc[QStringLiteral("coordinator")].toObject(); auto g = b[group].toObject();
            if (remove) g.remove(name); else g[name] = v; b[group] = g; doc[QStringLiteral("coordinator")] = b;
        }
    }
    static QJsonValue mutateTree(const QByteArray &sig, const QJsonValue &value) {
        if (sig.startsWith('a')) { auto a = value.toArray(); if (a.isEmpty()) a.append(QJsonArray{}); else a.removeLast(); return a; }
        if (sig.startsWith('(')) {
            auto a = value.toArray(); qsizetype p = 1; const auto first = takeType(sig, p);
            a[0] = mutateTree(first, a[0]); return a;
        }
        if (sig == "s") return QJsonValue(QString(value.toString() + QStringLiteral("changed")));
        if (sig == "b") return !value.toBool();
        if (sig == "t") return value == QStringLiteral("1") ? QStringLiteral("2") : QStringLiteral("1");
        return value.toInt() == 1 ? 2 : 1;
    }
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
        registerWireTypes();
    }
    void independentDescriptorOracle() {
        QFile f(QFINDTESTDATA("data/maintenance-property-signatures.txt")); QVERIFY(f.open(QIODevice::ReadOnly));
        const auto bytes = f.readAll();
        QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(), QByteArray("d78353972af6c9d1316e6bbf9161a12b112a42bba3b2c74c08e12b4cf73bc6bb"));
        QList<QByteArray> expected, actual;
        for (const auto &line : bytes.split('\n')) if (!line.isEmpty() && !line.startsWith('#')) expected.append(line);
        for (const auto &d : P::descriptors()) actual.append(QByteArray(d.group) + '\t' + d.name + '\t' + d.signature);
        std::sort(actual.begin(), actual.end()); QCOMPARE(actual.size(), 321); QCOMPARE(actual, expected);
    }
    void completeFixtureAndAllDescriptorMutations() {
        Harness h; QVERIFY(h.ready); const auto doc = document(); QString error;
        const auto parsed = P::parseApproved(P::canonical(doc) + '\n', &error); QVERIFY2(parsed, qPrintable(error));
        const auto all = inputs(h, doc); QVERIFY2(P::compareEffective(*parsed, all, &error), qPrintable(error));
        QVERIFY(!P::finishComparison(*parsed, all, &error)); QVERIFY(error.startsWith(QStringLiteral("runtimeChecksIncomplete:")));
        int checked = 0;
        for (const auto &d : P::descriptors()) {
            const QByteArray group(d.group); const auto key = QString::fromLatin1(d.name);
            const auto value = group == "manager" ? doc[QStringLiteral("manager")].toObject()[key]
                : baselineAt(doc, 0)[QString::fromLatin1(d.group)][key];
            auto missing = doc; change(missing, d, {}, true);
            QVERIFY2(!P::parseApproved(P::canonical(missing) + '\n'), d.name);
            auto wrong = doc; change(wrong, d, QJsonValue());
            // canonical intentionally has no null encoder; use Qt to test explicit null.
            QVERIFY2(!P::parseApproved(QJsonDocument(wrong).toJson(QJsonDocument::Compact) + '\n'), d.name);
            for (int mutation = 0; mutation < 3; ++mutation) {
                auto in = all;
                auto map = group == "manager" ? rawManager(doc) : rawMap(baselineAt(doc, 0), group == "unit");
                if (mutation == 0) map.remove(key);
                if (mutation == 1) map[key] = QByteArray(d.signature) == "ay" ? QVariant(false) : QVariant(QByteArray("wrong-wire-type"));
                if (mutation == 2) map[key] = wire(d.signature, mutateTree(d.signature, tree(d.signature, value)));
                if (key == QStringLiteral("PermissionsStartOnly") && mutation == 0) {
                    // Explicit-Get transport guarantees presence/type. There is
                    // no missing-bool encoding in completed Inputs; exercise its
                    // mandatory false value here, schema absence above, and the
                    // Identity suite covers missing/wrong Get replies.
                    in.units[0].permissionsStartOnly = true;
                }
                const auto reply = h.reply({map});
                QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
                QCOMPARE(reply.signature(), QStringLiteral("a{sv}")); QCOMPARE(reply.arguments().size(), 1);
                QVERIFY2(P::properties(reply), d.name); // No marshaling-error false positives.
                if (group == "manager") in.manager = reply;
                else if (group == "unit") in.units[0].unit = reply;
                else in.units[0].service = reply;
                QVERIFY2(!P::compareEffective(*parsed, in, &error), qPrintable(key + QString::number(mutation)));
            }
            ++checked;
        }
        QCOMPARE(checked, 321);
    }
    void canonicalAndNullNegatives() {
        const auto doc = document(); const QByteArray good = P::canonical(doc) + '\n'; QVERIFY(P::parseApproved(good));
        for (const auto &bytes : QList<QByteArray>{good.left(good.size()-1), good + '\n', QByteArray(" ") + good, good + ' ', QByteArray(1024*1024+1, ' ')})
            QVERIFY(!P::parseApproved(bytes));
        auto duplicate = good; duplicate.replace("\"v\":1", "\"v\":1,\"v\":1"); QVERIFY(!P::parseApproved(duplicate));
        auto escaped = good; escaped.replace("\"loaded\"", "\"lo\\u0061ded\""); QVERIFY(!P::parseApproved(escaped));
        auto nul = good; nul.replace("\"loaded\"", "\"load\\u0000ed\""); QVERIFY(!P::parseApproved(nul));
        auto nonascii = good; nonascii.replace("\"loaded\"", "\"load\\u00e9d\""); QVERIFY(!P::parseApproved(nonascii));
        auto unknown = doc; unknown[QStringLiteral("extra")] = false; QVERIFY(!P::parseApproved(P::canonical(unknown)+'\n'));
        auto empty = doc; empty[QStringLiteral("coordinator")] = QJsonObject{}; QVERIFY(!P::parseApproved(P::canonical(empty)+'\n'));
        auto depth = QByteArray(13, '[') + QByteArray(13, ']') + '\n'; QVERIFY(!P::parseApproved(depth));
        QVERIFY(P::canonical(QJsonValue(1e100)).isEmpty()); QVERIFY(P::canonical(QJsonValue(std::numeric_limits<double>::infinity())).isEmpty());
        Harness h; QVERIFY(h.ready);
        for (const auto &d : P::descriptors()) if (QByteArray(d.signature) == "s" && sample(d, P::inventoryName(0)) == QStringLiteral("")) {
            const auto mapReply = h.reply({QVariantMap{{QStringLiteral("value"), QVariant(QString())}}});
            const auto map = P::properties(mapReply); QVERIFY(map);
            const auto decoded = P::decode(d, map->value(QStringLiteral("value"))); QVERIFY(decoded); QVERIFY(decoded->isString()); QCOMPARE(decoded->toString(), QStringLiteral(""));
            for (const auto &invalid : {QJsonValue(), QJsonValue(QJsonValue::Undefined), QJsonValue(QJsonArray{}), QJsonValue(QJsonObject{}), QJsonValue(false), QJsonValue(0)})
                QVERIFY2(!P::normalize(d, invalid, P::inventoryName(0), false), d.name);
            break;
        }
        for (const auto &d : P::descriptors()) if (QByteArray(d.signature) == "(bs)") {
            for (const auto &invalid : {QJsonValue(), QJsonValue(QJsonArray{}), QJsonValue(QJsonObject{}), QJsonValue(false), QJsonValue(0)}) {
                auto tuple = sample(d, P::inventoryName(0)).toObject(); tuple[QStringLiteral("value")] = invalid;
                QVERIFY(!P::normalize(d, tuple, P::inventoryName(0), false));
            }
        }
    }
    void setOrderSecurityAndVolatileFields() {
        Harness h; QVERIFY(h.ready); const auto name = P::inventoryName(0);
        for (const auto &d : P::descriptors()) {
            if (QByteArray(d.name) == "Names") {
                QVERIFY(!P::normalize(d, QJsonArray{}, name, false));
                QVERIFY(!P::normalize(d, QJsonArray{QStringLiteral("unrelated.service")}, name, false));
            }
            if (d.rule == P::Rule::Set) {
                QJsonArray a{QStringLiteral("z"), name, QStringLiteral("a")};
                const auto n = P::normalize(d, a, name, true); QVERIFY(n);
                QVERIFY(!P::normalize(d, a, name, false));
                a.append(name); QVERIFY(!P::normalize(d, a, name, true));
            }
            if (QByteArray(d.name) == "Environment") {
                QJsonArray a{QStringLiteral("X=second"), QStringLiteral("X=first"), QStringLiteral("X=first")};
                const auto n = P::normalize(d, a, name, true); QVERIFY(n); QCOMPARE(*n, QJsonValue(a));
            }
            if (QByteArray(d.name) == "SystemCallFilter") {
                QJsonObject filter{{QStringLiteral("allowList"), false}, {QStringLiteral("items"), QJsonArray{QStringLiteral("write:EIO"), QStringLiteral("read:EPERM")}}};
                auto reply = h.reply({QVariantMap{{QStringLiteral("value"), wire(d.signature, tree(d.signature, filter))}}});
                const auto p = P::properties(reply); QVERIFY(p); const auto decoded = P::decode(d, p->value(QStringLiteral("value"))); QVERIFY(decoded);
                QCOMPARE(*decoded, QJsonValue(filter));
            }
            if (QByteArray(d.signature) == "a(sbbsi)" || QByteArray(d.signature) == "a(sasasttttuii)") {
                const bool command = QByteArray(d.signature) == "a(sasasttttuii)";
                QJsonArray expected;
                if (command) expected.append(QJsonObject{{QStringLiteral("path"), QStringLiteral("/approved/guard")}, {QStringLiteral("argv"), QJsonArray{QStringLiteral("/approved/guard")}},
                    {QStringLiteral("flags"), QJsonArray{QStringLiteral("ignore-failure"), QStringLiteral("no-env-expand")}}});
                else expected.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("ConditionPathExists")}, {QStringLiteral("parameter"), QStringLiteral("/approved")},
                    {QStringLiteral("trigger"), false}, {QStringLiteral("negate"), false}});
                for (int result : {-1, 0, 1, 2}) {
                    auto rows = tree(d.signature, expected).toArray(); auto row = rows[0].toArray();
                    if (command) {
                        row[2] = QJsonArray{QStringLiteral("no-env-expand"), QStringLiteral("ignore-failure")};
                        for (int k = 3; k <= 6; ++k) row[k] = QString::number(result + 10);
                        row[7] = 123; row[8] = result; row[9] = 42;
                    } else row[4] = result;
                    rows[0] = row;
                    const auto props = P::properties(h.reply({QVariantMap{{QStringLiteral("value"), wire(d.signature, rows)}}})); QVERIFY(props);
                    const auto decoded = P::decode(d, props->value(QStringLiteral("value")));
                    if (!command && result == 2) { QVERIFY(!decoded); continue; }
                    QVERIFY(decoded); const auto norm = P::normalize(d, *decoded, name, true); QVERIFY(norm); QCOMPARE(*norm, QJsonValue(expected));
                }
            }
            if (QByteArray(d.name) == "AppArmorProfile") {
                auto value = sample(d, P::inventoryName(size_t(P::Inventory::AptNews)));
                QVERIFY(P::normalize(d, value, P::inventoryName(size_t(P::Inventory::AptNews)), false));
                QVERIFY(!P::normalize(d, value, name, false)); QVERIFY(!P::normalize(d, value, P::inventoryName(size_t(P::Inventory::EsmCache)), false));
                auto object = value.toObject(); object[QStringLiteral("ignoreFailure")] = false;
                QVERIFY(!P::normalize(d, object, P::inventoryName(size_t(P::Inventory::AptNews)), false));
            }
        }
    }
    void numericAndContainerBoundaries() {
        const auto name = P::inventoryName(0);
        for (const auto &d : P::descriptors()) {
            const QByteArray sig(d.signature);
            if (d.rule != P::Rule::Exact) continue;
            if (sig == "q" || sig == "u" || sig == "i") {
                const double lo = sig == "i" ? -2147483648.0 : 0.0;
                const double hi = sig == "i" ? 2147483647.0 : sig == "u" ? 4294967295.0 : 65535.0;
                QVERIFY2(P::normalize(d, lo, name, false), d.name); QVERIFY(P::normalize(d, hi, name, false));
                for (double bad : {lo-1, hi+1, 0.5, std::numeric_limits<double>::infinity()}) QVERIFY(!P::normalize(d, bad, name, false));
                QVERIFY(!P::normalize(d, QStringLiteral("0"), name, false));
            }
            if (sig == "t") {
                for (const auto &value : {QStringLiteral("0"), QStringLiteral("18446744073709551615")}) QVERIFY(P::normalize(d, value, name, false));
                for (const auto &value : {QStringLiteral(""), QStringLiteral("00"), QStringLiteral("01"), QStringLiteral("+1"), QStringLiteral("-1"),
                         QStringLiteral("1.0"), QStringLiteral("1e2"), QStringLiteral("18446744073709551616")}) QVERIFY(!P::normalize(d, value, name, false));
                QVERIFY(!P::normalize(d, 1, name, false));
            }
        }
        const auto doc = document();
        for (const auto &d : P::descriptors()) {
            if (QByteArray(d.name) == "SyslogIdentifier") {
                auto m = doc; change(m, d, QString(4096, QLatin1Char('x'))); QVERIFY(P::parseApproved(P::canonical(m)+'\n'));
                change(m, d, QString(4097, QLatin1Char('x'))); QVERIFY(!P::parseApproved(P::canonical(m)+'\n'));
                change(m, d, QStringLiteral("quote\" slash\\ line\n tab\t")); const QByteArray b = P::canonical(m)+'\n'; QVERIFY(P::parseApproved(b));
            }
            if (QByteArray(d.name) == "Environment") {
                QJsonArray a; for (int i = 0; i < 256; ++i) a.append(QStringLiteral("X=a"));
                QVERIFY(P::normalize(d, a, name, false)); a.append(QStringLiteral("X=a")); QVERIFY(!P::normalize(d, a, name, false));
            }
            if (QByteArray(d.signature) == "a(sasasttttuii)") {
                const QJsonObject c{{QStringLiteral("path"), QStringLiteral("/approved/app")}, {QStringLiteral("argv"), QJsonArray{QStringLiteral("/approved/app")}}, {QStringLiteral("flags"), QJsonArray{}}};
                QJsonArray a; for (int i = 0; i < 64; ++i) a.append(c);
                QVERIFY(P::normalize(d, a, name, false)); a.append(c); QVERIFY(!P::normalize(d, a, name, false));
                auto bad = c; bad[QStringLiteral("flags")] = QJsonArray{QStringLiteral("via-shell"), QStringLiteral("via-shell")};
                QVERIFY(!P::normalize(d, QJsonArray{bad}, name, true));
                bad = c; bad[QStringLiteral("path")] = QStringLiteral("/approved/with\\backslash"); QVERIFY(!P::normalize(d, QJsonArray{bad}, name, false));
            }
        }
    }
    void newWireDecoderWidthsCommandsAndUnknownProperties() {
        Harness h; QVERIFY(h.ready); const auto name = P::inventoryName(0);
        const auto received = [&](const QVariant &v) {
            const auto reply = h.reply({QVariantMap{{QStringLiteral("value"), v}}});
            if (reply.type() != QDBusMessage::ReplyMessage || reply.signature() != QStringLiteral("a{sv}") || reply.arguments().size() != 1) return QVariant();
            const auto p = P::properties(reply); return p ? p->value(QStringLiteral("value")) : QVariant();
        };
        for (const auto &d : P::descriptors()) {
            if (QByteArray(d.signature) == "u") {
                const auto v = received(QVariant(qint32(0))); QVERIFY(v.isValid()); QCOMPARE(v.metaType(), QMetaType::fromType<qint32>()); QVERIFY(!P::decode(d, v));
            }
            if (QByteArray(d.signature) == "i") { auto v = received(QVariant(quint32(0))); QVERIFY(v.isValid()); QVERIFY(!P::decode(d, v)); }
            if (QByteArray(d.signature) == "q") { auto v = received(QVariant(quint32(0))); QVERIFY(v.isValid()); QVERIFY(!P::decode(d, v)); }
            if (QByteArray(d.signature) == "(bs)") {
                QDBusArgument a; a.beginStructure(); a << quint32(0) << QStringLiteral(""); a.endStructure();
                const auto v = received(QVariant::fromValue(a)); QVERIFY(v.isValid());
                QCOMPARE(qvariant_cast<QDBusArgument>(v).currentSignature(), QStringLiteral("(us)")); QVERIFY(!P::decode(d, v));
            }
            if (QByteArray(d.signature) == "a(sasasttttuii)") {
                const QJsonArray row{QStringLiteral("/approved/app"), QJsonArray{QStringLiteral("app")}, QJsonArray{},
                    QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"), 0, 0, 0};
                for (int mutation = 0; mutation < 4; ++mutation) {
                    auto r = row; QJsonArray rows;
                    if (mutation == 0) r[2] = QJsonArray{QStringLiteral("future-flag")};
                    if (mutation == 1) r[2] = QJsonArray{QStringLiteral("via-shell"), QStringLiteral("via-shell")};
                    if (mutation == 2) r[1] = QJsonArray{};
                    for (int i = 0; i < (mutation == 3 ? 65 : 1); ++i) rows.append(r);
                    const auto v = received(wire(d.signature, rows)); QVERIFY(v.isValid());
                    QCOMPARE(qvariant_cast<QDBusArgument>(v).currentSignature(), QString::fromLatin1(d.signature));
                    const auto decoded = P::decode(d, v);
                    QVERIFY(!decoded || !P::normalize(d, *decoded, name, true));
                }
            }
        }
        const auto doc = document(); const auto policy = P::parseApproved(P::canonical(doc)+'\n'); QVERIFY(policy);
        auto in = inputs(h, doc); auto manager = rawManager(doc); manager[QStringLiteral("UnrelatedExportedProperty")] = QStringLiteral("ignored");
        in.manager = h.reply({manager}); QVERIFY(P::compareEffective(*policy, in));
        in.manager = h.reply({QVariant::fromValue(DuplicateProperties{})}); QVERIFY(!P::compareEffective(*policy, in));
        in = inputs(h, doc); auto service = rawMap(baselineAt(doc, 0), false);
        for (const QVariant &bad : {QVariant(true), QVariant(quint32(0))}) {
            service[QStringLiteral("PermissionsStartOnly")] = bad; in.units[0].service = h.reply({service}); QString error;
            QVERIFY(!P::compareEffective(*policy, in, &error)); QVERIFY(error.contains(QStringLiteral("inconsistent hidden property")));
        }
    }
    void publicMatchingFixtureStillRefuses() {
        Harness h; QVERIFY(h.ready); const auto doc = document(); const QByteArray bytes = P::canonical(doc) + '\n';
        QString error; const auto parsed = P::parseApproved(bytes, &error); QVERIFY2(parsed, qPrintable(error));
        const auto in = inputs(h, doc); QVERIFY2(P::compareEffective(*parsed, in, &error), qPrintable(error));
        QMetaObject::invokeMethod(&h.fixture, [&] {
            h.fixture.manager = rawManager(doc);
            for (size_t i = 0; i < size_t(P::Inventory::Count); ++i) {
                const auto path = VirtualSessionCoordinatorIdentity::policyUnitPath(static_cast<P::Inventory>(i));
                h.fixture.units[path] = rawMap(baselineAt(doc, i), true);
                h.fixture.services[path] = rawMap(baselineAt(doc, i), false);
                if (i == 0) {
                    auto &u = h.fixture.units[path]; auto &s = h.fixture.services[path];
                    u[QStringLiteral("ActiveState")] = QStringLiteral("activating"); u[QStringLiteral("SubState")] = QStringLiteral("start");
                    u[QStringLiteral("InvocationID")] = QUuid::createUuid().toRfc4122(); s[QStringLiteral("MainPID")] = quint32(getpid());
                }
            }
        }, Qt::BlockingQueuedConnection);
        auto identity = VirtualSessionCoordinatorIdentity::pinOnBus(h.client, QFile::symLinkTarget(QStringLiteral("/proc/self/exe")), getuid(), getpid(), true, 3000);
        QVERIFY(identity);
        QTemporaryDir dir; QVERIFY(dir.isValid()); QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("etc/krdp"))));
        QVERIFY(!chmod(QFile::encodeName(dir.filePath(QStringLiteral("etc"))).constData(), 0700));
        QVERIFY(!chmod(QFile::encodeName(dir.filePath(QStringLiteral("etc/krdp"))).constData(), 0700));
        const auto put = [](const QString &p, const QByteArray &data) {
            QFile f(p); return f.open(QIODevice::WriteOnly) && !fchmod(f.handle(), 0600) && f.write(data) == data.size() && f.flush();
        };
        const auto logical = QStringLiteral("/etc/krdp/virtual-writer-policy.json"); QVERIFY(put(dir.path()+logical, bytes));
        QFile policyFile(dir.path()+logical); QVERIFY(policyFile.open(QIODevice::ReadOnly));
        struct stat policyStat{}; QVERIFY(!fstat(policyFile.handle(), &policyStat)); policyFile.close();
        const QJsonObject file{{QStringLiteral("path"), logical}, {QStringLiteral("mode"), 0600}, {QStringLiteral("uid"), qint64(getuid())},
            {QStringLiteral("gid"), qint64(policyStat.st_gid)}, {QStringLiteral("size"), bytes.size()},
            {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
        const QJsonObject manifest{{QStringLiteral("v"), 1}, {QStringLiteral("files"), QJsonArray{file}}, {QStringLiteral("directories"), QJsonArray{}},
            {QStringLiteral("symlinks"), QJsonArray{}}, {QStringLiteral("absent"), QJsonArray{}}, {QStringLiteral("processes"), QJsonArray{}}};
        QVERIFY(put(dir.filePath(QStringLiteral("profile.json")), P::canonical(manifest)+'\n'));
        auto profile = VirtualSessionRuntimeProfile::loadAt(dir.path(), QStringLiteral("/profile.json"), getuid(), &error); QVERIFY2(profile, qPrintable(error));
        QVERIFY(!P::validate(*identity, *profile, &error));
        QVERIFY2(error.startsWith(QStringLiteral("runtimeChecksIncomplete:")), qPrintable(error));
        QMetaObject::invokeMethod(&h.fixture, [&] { QCOMPARE(h.fixture.inventoryReads, 12); }, Qt::BlockingQueuedConnection);
        // No runtime-role/profile-path binding or quiescence approval is claimed.
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
