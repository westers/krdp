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
