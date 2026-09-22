// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <QJsonArray>
#include "VirtualSessionControl.h"

using namespace KRdp;
using Phase = VirtualSessionState::Phase;

static QJsonObject command(const QString &id, const QString &action, const QString &session = {})
{
    QJsonObject value{{QStringLiteral("type"), QStringLiteral("virtual-session")}, {QStringLiteral("v"), 1},
                      {QStringLiteral("id"), id}, {QStringLiteral("action"), action}};
    if (!session.isEmpty()) value.insert(QStringLiteral("session"), session);
    return value;
}
static std::optional<VirtualSessionSupervisor::Launch> sleeper(quint32, const VirtualSessionRegistry::Handle &)
{
    return VirtualSessionSupervisor::Launch{QStringLiteral("/usr/bin/sleep"), {QStringLiteral("60")}, {}, {}};
}

class VirtualSessionControlTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void dismissSchemaCapabilityAndReplay()
    {
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> { return {}; });
        VirtualSessionControl control(supervisor, {});
        const auto created = control.request(1000, 1, command(QStringLiteral("create"), QStringLiteral("create")));
        const auto id = created.value(QStringLiteral("session")).toString();
        const auto before = control.request(1000, 1, command(QStringLiteral("list-old"), QStringLiteral("list")));
        QVERIFY(!before.value(QStringLiteral("sessions")).toArray().first().toObject().contains(QStringLiteral("dismissible")));
        QVERIFY(!control.request(1000, 1, command(QStringLiteral("unsupported"), QStringLiteral("dismiss"), id)).value(QStringLiteral("ok")).toBool());
        int mutations = 0;
        bool uncertain = true;
        control.setDismissHandlers([&](quint32 uid, const QString &session) { return uid == 1000 && session == id; },
            [&](quint32 uid, const QString &session) {
                ++mutations;
                if (uid != 1000 || session != id) return VirtualSessionControl::DismissResult::Unavailable;
                return uncertain ? VirtualSessionControl::DismissResult::Uncertain : VirtualSessionControl::DismissResult::Accepted;
            });
        const auto listed = control.request(1000, 1, command(QStringLiteral("list-new"), QStringLiteral("list")));
        const auto capability = listed.value(QStringLiteral("sessions")).toArray().first().toObject().value(QStringLiteral("dismissible"));
        QVERIFY(capability.isBool()); QVERIFY(capability.toBool());
        for (auto malformed : {command(QStringLiteral("bad"), QStringLiteral("dismiss")),
                               command(QStringLiteral("bad"), QStringLiteral("dismiss"), QStringLiteral("../path"))})
            QVERIFY(!control.request(1000, 1, malformed).value(QStringLiteral("ok")).toBool());
        auto extra = command(QStringLiteral("bad"), QStringLiteral("dismiss"), id); extra.insert(QStringLiteral("uid"), 1000);
        QVERIFY(!control.request(1000, 1, extra).value(QStringLiteral("ok")).toBool()); QCOMPARE(mutations, 0);
        const auto request = command(QStringLiteral("dismiss-once"), QStringLiteral("dismiss"), id);
        const auto failed = control.request(1000, 1, request); QVERIFY(!failed.value(QStringLiteral("ok")).toBool());
        uncertain = false;
        QCOMPARE(control.request(1000, 1, request), failed); QCOMPARE(mutations, 1);
        const auto accepted = control.request(1000, 1, command(QStringLiteral("fresh-retry"), QStringLiteral("dismiss"), id));
        QVERIFY(accepted.value(QStringLiteral("ok")).toBool()); QCOMPARE(mutations, 2);
        QCOMPARE(accepted.value(QStringLiteral("session")).toString(), id);
        QCOMPARE(accepted.value(QStringLiteral("state")).toString(), QStringLiteral("dismissed"));
        // The host acknowledgement callback, not a hidden Stop, owns retirement.
        QCOMPARE(supervisor.list(1000).first().phase, Phase::Failed);
    }
    void dismissalHandlersMayDestroyControl_data()
    {
        QTest::addColumn<bool>("listing"); QTest::newRow("list") << true; QTest::newRow("dismiss") << false;
    }
    void dismissalHandlersMayDestroyControl()
    {
        QFETCH(bool, listing);
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> { return {}; });
        auto control = std::make_unique<VirtualSessionControl>(supervisor, VirtualSessionControl::Release{});
        const auto created = control->request(1000, 1, command(QStringLiteral("create"), QStringLiteral("create")));
        const auto id = created.value(QStringLiteral("session")).toString();
        control->setDismissHandlers([&](quint32, const QString &) { control.reset(); return true; },
            [&](quint32, const QString &) { control.reset(); return VirtualSessionControl::DismissResult::Accepted; });
        const auto response = control->request(1000, 1, command(QStringLiteral("callback"),
            listing ? QStringLiteral("list") : QStringLiteral("dismiss"), listing ? QString{} : id));
        QVERIFY(!response.value(QStringLiteral("ok")).toBool()); QVERIFY(!control);
    }
    void revokeGuardAlsoCoversMissingRecordAndControlDestruction()
    {
        VirtualSessionSupervisor supervisor(sleeper);
        auto control = std::make_unique<VirtualSessionControl>(supervisor, VirtualSessionControl::Release{});
        bool called = false, guarded = false, refused = false;
        control->disconnected(42, [&] {
            called = true; guarded = control->dispatchActive();
            refused = !control->request(1000, 42, command(QStringLiteral("reenter"), QStringLiteral("create")))
                .value(QStringLiteral("ok")).toBool();
            control.reset();
        });
        QVERIFY(called); QVERIFY(guarded); QVERIFY(refused); QVERIFY(!control);
        QVERIFY(supervisor.list(1000).isEmpty());
    }
    void reentrantRequestsCannotRepeatOrReplacePendingMutation()
    {
        VirtualSessionSupervisor supervisor(sleeper);
        VirtualSessionControl control(supervisor, {});
        const auto create = command(QStringLiteral("c"), QStringLiteral("create"));
        int called = 0;
        bool guarded = false;
        QList<QJsonObject> nested;
        control.setCreateHandler([&](quint32 uid) {
            ++called;
            guarded = control.dispatchActive();
            // Bound the fixture even against the old recursively executing code.
            if (called == 1) {
                nested.append(control.request(uid, 1, create));
                nested.append(control.request(uid, 1, command(QStringLiteral("different"), QStringLiteral("create"))));
                nested.append(control.request(1001, 2, command(QStringLiteral("other"), QStringLiteral("create"))));
            }
            return supervisor.create(uid);
        });
        const auto response = control.request(1000, 1, create);
        QVERIFY(response.value(QStringLiteral("ok")).toBool());
        QVERIFY(guarded); QVERIFY(!control.dispatchActive()); QCOMPARE(called, 1);
        QCOMPARE(nested.size(), 3);
        for (const auto &value : nested) QVERIFY(!value.value(QStringLiteral("ok")).toBool());
        QCOMPARE(control.request(1000, 1, create), response); QCOMPARE(called, 1);
        QCOMPARE(supervisor.list(1000).size(), 1); QVERIFY(supervisor.list(1001).isEmpty());
    }
    void disconnectDuringCreateDoesNotCacheIntoReplacement()
    {
        VirtualSessionSupervisor supervisor(sleeper);
        VirtualSessionControl control(supervisor, {});
        bool nestedRefused = false;
        control.setCreateHandler([&](quint32 uid) {
            control.disconnected(1);
            nestedRefused = !control.request(1001, 1, command(QStringLiteral("fresh"), QStringLiteral("list")))
                .value(QStringLiteral("ok")).toBool();
            return supervisor.create(uid);
        });
        const auto result = control.request(1000, 1, command(QStringLiteral("c"), QStringLiteral("create")));
        QVERIFY(!result.value(QStringLiteral("ok")).toBool()); QVERIFY(nestedRefused);
        QCOMPARE(supervisor.list(1000).size(), 1); // Accepted mutation is not rolled back or repeated.
        const auto replacement = control.request(1001, 1, command(QStringLiteral("c"), QStringLiteral("list")));
        QVERIFY(replacement.value(QStringLiteral("ok")).toBool());
        QVERIFY(replacement.value(QStringLiteral("sessions")).toArray().isEmpty());
    }
    void createCallbacksMayDestroyControlOrSupervisor_data()
    {
        QTest::addColumn<QString>("target");
        for (const auto *target : {"control", "supervisor", "factory-supervisor"})
            QTest::newRow(target) << QString::fromLatin1(target);
    }
    void createCallbacksMayDestroyControlOrSupervisor()
    {
        QFETCH(QString, target);
        std::unique_ptr<VirtualSessionSupervisor> supervisor;
        supervisor = std::make_unique<VirtualSessionSupervisor>([&](quint32 uid, const auto &handle) {
            if (target == QStringLiteral("factory-supervisor")) supervisor.reset();
            return sleeper(uid, handle);
        });
        auto control = std::make_unique<VirtualSessionControl>(*supervisor, VirtualSessionControl::Release{});
        if (target != QStringLiteral("factory-supervisor")) {
            control->setCreateHandler([&](quint32) -> std::optional<VirtualSessionControl::Handle> {
                if (target == QStringLiteral("control")) control.reset();
                else supervisor.reset();
                return {};
            });
        }
        const auto response = control->request(1000, 1, command(QStringLiteral("c"), QStringLiteral("create")));
        QVERIFY(!response.value(QStringLiteral("ok")).toBool());
        if (control) QVERIFY(!control->dispatchActive());
    }
    void releaseMayReenterDisconnectOrDestroyControl_data()
    {
        QTest::addColumn<QString>("action"); QTest::addColumn<bool>("destroy");
        for (const auto *action : {"detach", "stop", "disconnect"}) {
            QTest::newRow(qPrintable(QString::fromLatin1(action) + QStringLiteral("-reenter"))) << QString::fromLatin1(action) << false;
            QTest::newRow(qPrintable(QString::fromLatin1(action) + QStringLiteral("-destroy"))) << QString::fromLatin1(action) << true;
        }
    }
    void releaseMayReenterDisconnectOrDestroyControl()
    {
        QFETCH(QString, action); QFETCH(bool, destroy);
        std::optional<VirtualSessionControl::Handle> handle;
        VirtualSessionSupervisor supervisor([&](quint32 uid, const auto &created) {
            handle = created; return sleeper(uid, created);
        });
        int releases = 0; bool nestedRefused = false;
        std::unique_ptr<VirtualSessionControl> control;
        control = std::make_unique<VirtualSessionControl>(supervisor, [&](quint64 client, const auto &) {
            ++releases;
            if (destroy) { control.reset(); return; }
            control->disconnected(client);
            control->disconnected(client);
            nestedRefused = !control->request(1001, client, command(QStringLiteral("replacement"), QStringLiteral("create")))
                .value(QStringLiteral("ok")).toBool();
        });
        const auto made = control->request(1000, 1, command(QStringLiteral("c"), QStringLiteral("create")));
        const QString id = made.value(QStringLiteral("session")).toString();
        QVERIFY(handle); bool ready = false;
        QTRY_VERIFY(ready || (ready = supervisor.captureReady(*handle)));
        QVERIFY(control->request(1000, 1, command(QStringLiteral("a"), QStringLiteral("attach"), id)).value(QStringLiteral("ok")).toBool());
        const auto other = control->request(1001, 2, command(QStringLiteral("other"), QStringLiteral("create")));
        QVERIFY(other.value(QStringLiteral("ok")).toBool());
        if (action == QStringLiteral("disconnect")) control->disconnected(1);
        else control->request(1000, action == QStringLiteral("stop") ? 3 : 1,
            command(QStringLiteral("release"), action, action == QStringLiteral("stop") ? id : QString{}));
        QCOMPARE(releases, 1);
        if (control) {
            QVERIFY(nestedRefused); QVERIFY(!control->attachment(1)); QVERIFY(!control->dispatchActive());
            QVERIFY(control->request(1001, 1, command(QStringLiteral("replacement"), QStringLiteral("list"))).value(QStringLiteral("ok")).toBool());
        }
        QCOMPARE(supervisor.list(1001).size(), 1);
        QCOMPARE(supervisor.list(1001).first().id, other.value(QStringLiteral("session")).toString());
        QCOMPARE(supervisor.list(1001).first().phase, Phase::Starting);
        QVERIFY(supervisor.list(1000).first().phase != Phase::Attached);
    }
    void rejectClaimedIdentityAndMalformedRequests()
    {
        VirtualSessionSupervisor supervisor(sleeper);
        VirtualSessionControl control(supervisor, {});
        auto create = command(QStringLiteral("1"), QStringLiteral("create"));
        QVERIFY(!control.request({}, 1, create).value(QStringLiteral("ok")).toBool());
        QVERIFY(!control.request(0, 1, create).value(QStringLiteral("ok")).toBool());
        QVERIFY(!control.request(1000, 0, create).value(QStringLiteral("ok")).toBool());
        create.insert(QStringLiteral("uid"), 1000);
        QVERIFY(!control.request(1000, 1, create).value(QStringLiteral("ok")).toBool());
        create.remove(QStringLiteral("uid"));
        create.insert(QStringLiteral("v"), 1.5);
        QVERIFY(!control.request(1000, 1, create).value(QStringLiteral("ok")).toBool());
        QVERIFY(!control.request(1000, 1, command(QStringLiteral("1"), QStringLiteral("attach"), QStringLiteral("../seat0"))).value(QStringLiteral("ok")).toBool());
        QVERIFY(!control.request(1000, 1, command(QString(65, QLatin1Char('x')), QStringLiteral("create"))).value(QStringLiteral("ok")).toBool());
        QVERIFY(supervisor.list(1000).isEmpty());
    }
    void duplicateCreateIsNotAnotherDesktop()
    {
        int launched = 0;
        VirtualSessionSupervisor supervisor([&](quint32 uid, const auto &handle) {
            ++launched;
            return sleeper(uid, handle);
        });
        VirtualSessionControl control(supervisor, {});
        const auto create = command(QStringLiteral("create1"), QStringLiteral("create"));
        const auto first = control.request(1000, 1, create);
        QVERIFY(first.value(QStringLiteral("ok")).toBool());
        QCOMPARE(control.request(1000, 1, create), first);
        QCOMPARE(launched, 1);
        QCOMPARE(supervisor.list(1000).size(), 1);
        QVERIFY(!control.request(1000, 1, command(QStringLiteral("create1"), QStringLiteral("list"))).value(QStringLiteral("ok")).toBool());
        QVERIFY(!control.request(1001, 1, command(QStringLiteral("new"), QStringLiteral("list"))).value(QStringLiteral("ok")).toBool());
        const auto other = control.request(1001, 2, command(QStringLiteral("list1"), QStringLiteral("list")));
        QVERIFY(other.value(QStringLiteral("sessions")).toArray().isEmpty());
        QVERIFY(!control.request(1001, 2, command(QStringLiteral("stop1"), QStringLiteral("stop"), first.value(QStringLiteral("session")).toString())).value(QStringLiteral("ok")).toBool());
    }
    void attachDisconnectResumeAndOwnerStop()
    {
        std::optional<VirtualSessionRegistry::Handle> handle;
        VirtualSessionSupervisor supervisor([&](quint32 uid, const auto &created) {
            handle = created;
            return sleeper(uid, created);
        });
        QList<quint64> released;
        VirtualSessionControl control(supervisor, [&](quint64 client, const auto &releasedHandle) {
            QCOMPARE(releasedHandle.generation, handle->generation);
            QCOMPARE(supervisor.list(1000).first().phase, Phase::Attached);
            released.append(client);
        });
        const auto created = control.request(1000, 1, command(QStringLiteral("c"), QStringLiteral("create")));
        const auto id = created.value(QStringLiteral("session")).toString();
        QVERIFY(handle);
        QVERIFY(!control.request(1000, 1, command(QStringLiteral("early"), QStringLiteral("attach"), id)).value(QStringLiteral("ok")).toBool());
        bool ready = false;
        QTRY_VERIFY(ready || (ready = supervisor.captureReady(*handle)));
        QVERIFY(control.request(1000, 1, command(QStringLiteral("a"), QStringLiteral("attach"), id)).value(QStringLiteral("ok")).toBool());
        QVERIFY(control.attachment(1));
        QVERIFY(!control.request(1000, 2, command(QStringLiteral("a"), QStringLiteral("attach"), id)).value(QStringLiteral("ok")).toBool());
        control.disconnected(1);
        QCOMPARE(released, QList<quint64>{1});
        QVERIFY(!control.attachment(1));
        QCOMPARE(supervisor.list(1000).first().phase, Phase::Retained);
        QVERIFY(control.request(1000, 2, command(QStringLiteral("a2"), QStringLiteral("attach"), id)).value(QStringLiteral("ok")).toBool());
        QCOMPARE(control.attachment(2)->generation, handle->generation);
        // Another transport of the authenticated owner may explicitly stop it.
        QVERIFY(control.request(1000, 3, command(QStringLiteral("s"), QStringLiteral("stop"), id)).value(QStringLiteral("ok")).toBool());
        QCOMPARE(released, (QList<quint64>{1, 2}));
        QVERIFY(!control.attachment(2));
        QTRY_COMPARE(supervisor.list(1000).first().phase, Phase::Absent);
    }
    void boundedReplayHistoryNeverReexecutesOldMutation()
    {
        VirtualSessionSupervisor supervisor(sleeper);
        VirtualSessionControl control(supervisor, {});
        const auto create = command(QStringLiteral("c"), QStringLiteral("create"));
        const auto created = control.request(1000, 1, create);
        for (int i = 0; i < 255; ++i) {
            QVERIFY(control.request(1000, 1, command(QString::number(i), QStringLiteral("list"))).value(QStringLiteral("ok")).toBool());
        }
        QVERIFY(!control.request(1000, 1, command(QStringLiteral("overflow"), QStringLiteral("create"))).value(QStringLiteral("ok")).toBool());
        QCOMPARE(control.request(1000, 1, create), created);
        QCOMPARE(supervisor.list(1000).size(), 1);
        control.disconnected(1);
        QVERIFY(control.request(1000, 2, command(QStringLiteral("fresh"), QStringLiteral("list"))).value(QStringLiteral("ok")).toBool());
    }
    void failedLaunchIsReportedAsFailedNotReady()
    {
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> { return {}; });
        VirtualSessionControl control(supervisor, {});
        const auto created = control.request(1000, 1, command(QStringLiteral("c"), QStringLiteral("create")));
        QVERIFY(created.value(QStringLiteral("ok")).toBool());
        QCOMPARE(created.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        QVERIFY(!control.request(1000, 1, command(QStringLiteral("a"), QStringLiteral("attach"), created.value(QStringLiteral("session")).toString())).value(QStringLiteral("ok")).toBool());
    }
};
QTEST_GUILESS_MAIN(VirtualSessionControlTest)
#include "VirtualSessionControlTest.moc"
