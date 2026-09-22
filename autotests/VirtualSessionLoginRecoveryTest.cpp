// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionLoginRecovery.h"
#include "VirtualSessionLoginTag.h"
#include <QTest>
#include <QDBusVirtualObject>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusMetaType>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QElapsedTimer>
#include <functional>
using namespace Qt::StringLiterals;
using namespace KRdp;
struct RecoveryRow { QString id; quint32 uid; QString name, seat; QDBusObjectPath path; };
Q_DECLARE_METATYPE(RecoveryRow)
QDBusArgument &operator<<(QDBusArgument &a, const RecoveryRow &r) {
    a.beginStructure(); a << r.id << r.uid << r.name << r.seat << r.path; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, RecoveryRow &r) {
    a.beginStructure(); a >> r.id >> r.uid >> r.name >> r.seat >> r.path; a.endStructure(); return a;
}
namespace {
const QString root = u"/org/freedesktop/login1"_s;
const QString prefix = root + u"/session/"_s;
const QString service = u"org.freedesktop.login1"_s;
QVariant userTuple(quint32 uid, const QString &path = root + u"/user/_1000"_s) {
    QDBusArgument a; a.beginStructure(); a << uid << QDBusObjectPath(path); a.endStructure();
    return QVariant::fromValue(a);
}
QVariant seatTuple(const QString &seat = {}) {
    QDBusArgument a; a.beginStructure(); a << seat << QDBusObjectPath(seat.isEmpty() ? u"/"_s : root + u"/seat/seat0"_s); a.endStructure();
    return QVariant::fromValue(a);
}
QVariantMap props(const QString &id, const QString &tag) {
    const QString scope = u"session-"_s + id + u".scope"_s;
    return {{u"Id"_s, id}, {u"Desktop"_s, tag}, {u"Service"_s, u"krdp-virtual-session"_s},
        {u"Type"_s, u"wayland"_s}, {u"Class"_s, u"background"_s}, {u"State"_s, u"closing"_s},
        {u"TTY"_s, QString()}, {u"Display"_s, QString()}, {u"Scope"_s, scope},
        {u"Leader"_s, quint32(2345)}, {u"VTNr"_s, quint32(0)}, {u"User"_s, userTuple(1000)}, {u"Seat"_s, seatTuple()}};
}
class RecoveryServer : public QDBusVirtualObject {
public:
    QList<RecoveryRow> rows;
    QMap<QString, QVariantMap> fields;
    QStringList terminated;
    QString failMethod;
    bool malformedList = false, retain = false, mutateBeforeTerminate = false, eraseTagAfterTerminate = false;
    int lists = 0, propertyReads = 0, delayList = 0, delayPropertiesMs = 0;
    std::function<void()> replaceOwner;
    bool replaceAtList = false, replaceAtTerminate = false;
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &m, const QDBusConnection &bus) override {
        const auto error = [&] { bus.send(m.createErrorReply(u"org.freedesktop.DBus.Error.Failed"_s, u"fixture rejection"_s)); };
        if (m.member() == failMethod) { error(); return true; }
        if (m.path() == root && m.interface() == u"org.freedesktop.login1.Manager"_s && m.member() == u"ListSessions"_s && m.arguments().isEmpty()) {
            ++lists;
            if (replaceAtList && replaceOwner) { replaceOwner(); replaceAtList = false; }
            if (malformedList) bus.send(m.createReply(QList<QVariant>{QStringLiteral("not an array")}));
            else bus.send(m.createReply(QList<QVariant>{QVariant::fromValue(lists <= delayList ? QList<RecoveryRow>{} : rows)}));
        } else if (m.interface() == u"org.freedesktop.DBus.Properties"_s && m.member() == u"GetAll"_s
            && m.arguments() == QList<QVariant>{u"org.freedesktop.login1.Session"_s} && fields.contains(m.path())) {
            if (delayPropertiesMs) QThread::msleep(delayPropertiesMs);
            ++propertyReads;
            // Candidate is read after physical row: its revalidation is read3.
            if (mutateBeforeTerminate && propertyReads == 3) fields[m.path()][u"Leader"_s] = quint32(9999);
            bus.send(m.createReply(QList<QVariant>{fields[m.path()]}));
        } else if (m.interface() == u"org.freedesktop.login1.Session"_s && m.member() == u"Terminate"_s
            && m.arguments().isEmpty() && fields.contains(m.path())) {
            terminated.append(m.path());
            if (eraseTagAfterTerminate) fields[m.path()][u"Desktop"_s] = u"changed"_s;
            else if (!retain) {
                fields.remove(m.path());
                rows.removeIf([&](const auto &r) { return r.path.path() == m.path(); });
            }
            if (replaceAtTerminate && replaceOwner) { replaceOwner(); replaceAtTerminate = false; }
            bus.send(m.createReply());
        } else error();
        return true;
    }
};
class Fixture {
public:
    QProcess daemon;
    QString serverName = QUuid::createUuid().toString(), clientName = QUuid::createUuid().toString(), nextName = QUuid::createUuid().toString();
    std::optional<QDBusConnection> bus, client, next;
    QThread thread;
    RecoveryServer *server = nullptr;
    VirtualSessionJournal::Record record{1000, uuid(), uuid(), uuid(), uuid(), QByteArray(32, 's')};
    VirtualSessionJournal::Keeper keeper{2345, 42, 4242};
    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    bool start() {
        const auto executable = QStandardPaths::findExecutable(u"dbus-daemon"_s);
        if (executable.isEmpty()) return false;
        daemon.start(executable, {u"--session"_s, u"--nofork"_s, u"--print-address=1"_s});
        if (!daemon.waitForStarted(3000) || !daemon.waitForReadyRead(3000)) return false;
        const auto address = daemon.readLine().trimmed(); if (!address.startsWith("unix:")) return false;
        bus.emplace(QDBusConnection::connectToBus(QString::fromUtf8(address), serverName));
        client.emplace(QDBusConnection::connectToBus(QString::fromUtf8(address), clientName));
        next.emplace(QDBusConnection::connectToBus(QString::fromUtf8(address), nextName));
        if (!bus->isConnected() || !client->isConnected() || !next->isConnected() || !bus->registerService(service)) return false;
        server = new RecoveryServer;
        server->rows = {{u"c42"_s, 1000, u"fixture"_s, {}, QDBusObjectPath(prefix + u"c42"_s)},
            {u"3"_s, 1000, u"fixture"_s, u"seat0"_s, QDBusObjectPath(prefix + u"_33"_s)}};
        server->fields[prefix + u"c42"_s] = props(u"c42"_s, virtualLoginTag(record.launch));
        server->fields[prefix + u"_33"_s] = props(u"3"_s, u"KDE"_s);
        server->fields[prefix + u"_33"_s][u"Service"_s] = u"sddm"_s;
        server->replaceOwner = [&] { bus->unregisterService(service); next->registerService(service); };
        server->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, server, &QObject::deleteLater);
        thread.start();
        bool ok = false;
        QMetaObject::invokeMethod(server, [&] { ok = bus->registerVirtualObject(root, server, QDBusConnection::SubPath); }, Qt::BlockingQueuedConnection);
        return ok;
    }
    void edit(const std::function<void(RecoveryServer &)> &fn) {
        QMetaObject::invokeMethod(server, [&] { fn(*server); }, Qt::BlockingQueuedConnection);
    }
    QStringList terminations() { QStringList result; edit([&](auto &s) { result = s.terminated; }); return result; }
    ~Fixture() {
        if (bus) bus->unregisterObject(root, QDBusConnection::UnregisterTree);
        thread.quit(); thread.wait();
        QDBusConnection::disconnectFromBus(clientName); QDBusConnection::disconnectFromBus(serverName); QDBusConnection::disconnectFromBus(nextName);
        if (daemon.state() != QProcess::NotRunning) { daemon.terminate(); if (!daemon.waitForFinished(1000)) { daemon.kill(); daemon.waitForFinished(1000); } }
    }
};
}
class VirtualSessionLoginRecoveryTest : public QObject {
    Q_OBJECT
    using Result = VirtualSessionLoginRecovery::Result;
private Q_SLOTS:
    void initTestCase() { qDBusRegisterMetaType<RecoveryRow>(); qDBusRegisterMetaType<QList<RecoveryRow>>(); }
    void reconcilesOnlyExactVirtualLogin_data() {
        QTest::addColumn<QString>("state");
        for (const auto *state : {"opening", "online", "active", "closing"}) QTest::newRow(state) << QString::fromLatin1(state);
    }
    void reconcilesOnlyExactVirtualLogin() {
        QFETCH(QString, state); Fixture f; QVERIFY(f.start());
        f.edit([&](auto &s) { s.fields[prefix + u"c42"_s][u"State"_s] = state; });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 1000), Result::Removed);
        QCOMPARE(f.terminations(), QStringList{prefix + u"c42"_s});
        QList<RecoveryRow> remaining; f.edit([&](auto &s) { remaining = s.rows; });
        QCOMPARE(remaining.size(), 1); QCOMPARE(remaining[0].id, u"3"_s);
    }
    void rejectsContradictoryMarker_data() {
        QTest::addColumn<QString>("field");
        for (const auto *field : {"Id", "Service", "Type", "Class", "State", "TTY", "Display", "Scope", "Leader", "VTNr", "User", "Seat"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void rejectsContradictoryMarker() {
        QFETCH(QString, field); Fixture f; QVERIFY(f.start());
        f.edit([&](auto &s) {
            auto &properties = s.fields[prefix + u"c42"_s];
            if (field == u"Leader"_s || field == u"VTNr"_s) properties[field] = quint32(99);
            else if (field == u"User"_s) properties[field] = userTuple(1001);
            else if (field == u"Seat"_s) properties[field] = seatTuple(u"seat0"_s);
            else properties[field] = u"wrong"_s;
        });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 300), Result::Refused);
        QVERIFY(f.terminations().isEmpty());
    }
    void duplicatesRefusedBeforeAnyTermination() {
        Fixture f; QVERIFY(f.start());
        f.edit([&](auto &s) {
            s.rows.append({u"c43"_s, 1000, {}, {}, QDBusObjectPath(prefix + u"c43"_s)});
            s.fields[prefix + u"c43"_s] = props(u"c43"_s, virtualLoginTag(f.record.launch));
        });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 300), Result::Refused);
        QVERIFY(f.terminations().isEmpty());
    }
    void rejectsContradictoryUserObject_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("different uid") << root + u"/user/_1001"_s;
        QTest::newRow("noncanonical uid") << root + u"/user/_01000"_s;
        QTest::newRow("suffix") << root + u"/user/_1000extra"_s;
        QTest::newRow("child") << root + u"/user/_1000/child"_s;
    }
    void rejectsContradictoryUserObject() {
        QFETCH(QString, path); Fixture f; QVERIFY(f.start());
        f.edit([&](auto &s) { s.fields[prefix + u"c42"_s][u"User"_s] = userTuple(1000, path); });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 300), Result::Refused);
        QVERIFY(f.terminations().isEmpty());
    }
    void absenceIsNotProofAndLateRegistrationIsFound() {
        Fixture f; QVERIFY(f.start());
        f.edit([](auto &s) { s.delayList = 100; });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 120), Result::Unresolved);
        QVERIFY(f.terminations().isEmpty());
        f.edit([](auto &s) { s.lists = 0; s.delayList = 2; });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 1000), Result::Removed);
        QCOMPARE(f.terminations(), QStringList{prefix + u"c42"_s});
    }
    void terminateAcknowledgementIsNotDisappearance() {
        Fixture f; QVERIFY(f.start()); f.edit([](auto &s) { s.retain = true; });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 150), Result::Unresolved);
        QCOMPARE(f.terminations(), QStringList{prefix + u"c42"_s});
    }
    void successfulCloseRecordAllowsVerifiedAbsence() {
        Fixture f; QVERIFY(f.start()); f.edit([](auto &s) { s.rows.clear(); });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 300, true), Result::AlreadyClosed);
        QVERIFY(f.terminations().isEmpty());
    }
    void lostTagIsNotDisappearance() {
        Fixture f; QVERIFY(f.start()); f.edit([](auto &s) { s.eraseTagAfterTerminate = true; });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 500), Result::Refused);
        QCOMPARE(f.terminations(), QStringList{prefix + u"c42"_s});
    }
    void rejectsUnsafeListOrRevalidation_data() {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"list type", "duplicate row", "audit id", "path", "listed uid", "listed seat", "missing tag", "wrong type", "revalidation"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void rejectsUnsafeListOrRevalidation() {
        QFETCH(QString, mode); Fixture f; QVERIFY(f.start());
        f.edit([&](auto &s) {
            if (mode == u"list type"_s) s.malformedList = true;
            if (mode == u"duplicate row"_s) s.rows.append(s.rows[0]);
            if (mode == u"audit id"_s) s.rows[0].id = u"42"_s;
            if (mode == u"path"_s) s.rows[0].path = QDBusObjectPath(u"/outside"_s);
            if (mode == u"listed uid"_s) s.rows[0].uid = 1001;
            if (mode == u"listed seat"_s) s.rows[0].seat = u"seat0"_s;
            if (mode == u"missing tag"_s) s.fields[prefix + u"c42"_s].remove(u"Desktop"_s);
            if (mode == u"wrong type"_s) s.fields[prefix + u"c42"_s][u"VTNr"_s] = u"0"_s;
            if (mode == u"revalidation"_s) s.mutateBeforeTerminate = true;
        });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 300), Result::Refused);
        QVERIFY(f.terminations().isEmpty());
    }
    void methodFailuresAreNotAbsence_data() {
        QTest::addColumn<QString>("method");
        for (const auto *method : {"ListSessions", "GetAll", "Terminate"}) QTest::newRow(method) << QString::fromLatin1(method);
    }
    void methodFailuresAreNotAbsence() {
        QFETCH(QString, method); Fixture f; QVERIFY(f.start()); f.edit([&](auto &s) { s.failMethod = method; });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 300), Result::Refused);
        QVERIFY(f.terminations().isEmpty());
    }
    void ownerChangeNeverCountsAsCleanup_data() {
        QTest::addColumn<bool>("afterTerminate");
        QTest::newRow("before termination") << false;
        QTest::newRow("after termination") << true;
    }
    void ownerChangeNeverCountsAsCleanup() {
        QFETCH(bool, afterTerminate); Fixture f; QVERIFY(f.start());
        f.edit([&](auto &s) { s.replaceAtList = !afterTerminate; s.replaceAtTerminate = afterTerminate; });
        QCOMPARE(VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 500), Result::Refused);
        QCOMPARE(f.terminations().size(), afterTerminate ? 1 : 0);
    }
    void blockingReplyUsesOverallDeadline() {
        Fixture f; QVERIFY(f.start()); f.edit([](auto &s) { s.delayPropertiesMs = 100; });
        QElapsedTimer elapsed; elapsed.start();
        const auto result = VirtualSessionLoginRecovery::reconcile(*f.client, f.record, f.keeper, 50);
        QVERIFY(result != Result::Removed); QVERIFY(elapsed.elapsed() < 500);
        QVERIFY(f.terminations().isEmpty());
    }
};
QTEST_GUILESS_MAIN(VirtualSessionLoginRecoveryTest)
#include "VirtualSessionLoginRecoveryTest.moc"
