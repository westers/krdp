// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-067: the clipboard planner, state by state, crossed with the administrator's ClipboardDirection.

#include <QTest>

#include "ConsoleClipboardPolicy.h"

using namespace KRdp;
using namespace KRdp::ConsoleClipboardPolicy;

namespace
{
constexpr quint32 Owner = 1000;
constexpr quint32 Other = 1001;
}

class ConsoleClipboardPolicyTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void stateTable_data()
    {
        QTest::addColumn<int>("route");
        QTest::addColumn<int>("adapter");
        QTest::addColumn<bool>("locked");
        QTest::addColumn<quint32>("clientUid");
        QTest::addColumn<bool>("controls");
        QTest::addColumn<bool>("hostToClient");
        QTest::addColumn<bool>("clientToHost");
        QTest::addColumn<int>("reason");
        using A = ConsoleSeat::Adapter;
        const int console = int(Route::Console), virt = int(Route::Virtual);
        QTest::newRow("greeter: write only") << console << int(A::Greeter) << false << Owner << true << false << true << int(Reason::Greeter);
        QTest::newRow("greeter, another account") << console << int(A::Greeter) << false << Other << true << false << true << int(Reason::Greeter);
        QTest::newRow("locked, same account") << console << int(A::PhysicalUser) << true << Owner << true << true << true << int(Reason::Allowed);
        QTest::newRow("locked, other account") << console << int(A::PhysicalUser) << true << Other << true << false << false << int(Reason::ForeignLocked);
        QTest::newRow("unlocked, same account") << console << int(A::PhysicalUser) << false << Owner << true << true << true << int(Reason::Allowed);
        QTest::newRow("unlocked, other account") << console << int(A::PhysicalUser) << false << Other << true << false << false << int(Reason::ForeignUnlocked);
        QTest::newRow("viewer on greeter") << console << int(A::Greeter) << false << Owner << false << false << false << int(Reason::Viewer);
        QTest::newRow("viewer on own desktop") << console << int(A::PhysicalUser) << false << Owner << false << false << false << int(Reason::Viewer);
        QTest::newRow("no seat adapter") << console << int(A::None) << false << Owner << true << false << false << int(Reason::NoSeat);
        QTest::newRow("virtual adapter on console") << console << int(A::VirtualUser) << false << Owner << true << false << false << int(Reason::NoSeat);
        QTest::newRow("no uid") << console << int(A::PhysicalUser) << false << 0u << true << false << false << int(Reason::UnknownUser);
        QTest::newRow("virtual owner") << virt << int(A::VirtualUser) << false << Owner << true << true << true << int(Reason::Allowed);
        QTest::newRow("virtual, not the owner") << virt << int(A::VirtualUser) << false << Other << true << false << false << int(Reason::NoSeat);
        QTest::newRow("virtual, not attached") << virt << int(A::None) << false << Owner << true << false << false << int(Reason::NoSeat);
        QTest::newRow("virtual, not controlling") << virt << int(A::VirtualUser) << false << Owner << false << false << false << int(Reason::Viewer);
    }
    void stateTable()
    {
        QFETCH(int, route);
        QFETCH(int, adapter);
        QFETCH(bool, locked);
        QFETCH(quint32, clientUid);
        QFETCH(bool, controls);
        QFETCH(bool, hostToClient);
        QFETCH(bool, clientToHost);
        QFETCH(int, reason);
        Input input;
        input.route = Route(route);
        input.adapter = ConsoleSeat::Adapter(adapter);
        input.locked = locked;
        input.clientUid = clientUid ? std::optional<quint32>(clientUid) : std::nullopt;
        input.sessionUid = Owner;
        input.ownsControl = controls;
        const auto plan = ConsoleClipboardPolicy::plan(input);
        QCOMPARE(plan.hostToClient, hostToClient);
        QCOMPARE(plan.clientToHost, clientToHost);
        QCOMPARE(int(plan.reason), reason);
        QCOMPARE(plan.greeter, input.route == Route::Console && input.adapter == ConsoleSeat::Adapter::Greeter);
        QCOMPARE(plan.locked, locked);
    }

    // The setting only narrows: for every state and every ClipboardDirection, effective == state AND setting.
    void settingNeverWidens_data()
    {
        QTest::addColumn<int>("adapter");
        QTest::addColumn<bool>("locked");
        QTest::addColumn<quint32>("clientUid");
        QTest::addColumn<int>("route");
        using A = ConsoleSeat::Adapter;
        QTest::newRow("greeter") << int(A::Greeter) << false << Owner << int(Route::Console);
        QTest::newRow("locked own") << int(A::PhysicalUser) << true << Owner << int(Route::Console);
        QTest::newRow("locked other") << int(A::PhysicalUser) << true << Other << int(Route::Console);
        QTest::newRow("unlocked own") << int(A::PhysicalUser) << false << Owner << int(Route::Console);
        QTest::newRow("none") << int(A::None) << false << Owner << int(Route::Console);
        QTest::newRow("virtual") << int(A::VirtualUser) << false << Owner << int(Route::Virtual);
    }
    void settingNeverWidens()
    {
        QFETCH(int, adapter);
        QFETCH(bool, locked);
        QFETCH(quint32, clientUid);
        QFETCH(int, route);
        Input input;
        input.route = Route(route);
        input.adapter = ConsoleSeat::Adapter(adapter);
        input.locked = locked;
        input.clientUid = clientUid;
        input.sessionUid = Owner;
        input.ownsControl = true;
        input.setting = Direction::Both;
        const auto state = ConsoleClipboardPolicy::plan(input);
        for (const auto direction : {Direction::Both, Direction::HostToClient, Direction::ClientToHost, Direction::Off}) {
            input.setting = direction;
            const auto plan = ConsoleClipboardPolicy::plan(input);
            const bool wantHost = direction == Direction::Both || direction == Direction::HostToClient;
            const bool wantClient = direction == Direction::Both || direction == Direction::ClientToHost;
            QCOMPARE(plan.hostToClient, state.hostToClient && wantHost);
            QCOMPARE(plan.clientToHost, state.clientToHost && wantClient);
            QVERIFY(!(plan.hostToClient && !state.hostToClient));
            QVERIFY(!(plan.clientToHost && !state.clientToHost));
            if (direction == Direction::Off) {
                QVERIFY(!plan.hostToClient && !plan.clientToHost);
            }
            // "setting" is the reason only when the state would have allowed more.
            const bool narrowed = (state.hostToClient && !plan.hostToClient) || (state.clientToHost && !plan.clientToHost);
            if (narrowed && state.reason == Reason::Allowed) {
                QCOMPARE(plan.reason, Reason::Setting);
            } else {
                QCOMPARE(plan.reason, state.reason);
            }
        }
    }

    void policyFollowsEachInputChange()
    {
        Input input;
        input.adapter = ConsoleSeat::Adapter::PhysicalUser;
        input.clientUid = Owner;
        input.sessionUid = Owner;
        input.ownsControl = true;
        QVERIFY(ConsoleClipboardPolicy::plan(input).hostToClient);
        input.ownsControl = false; // control moved away
        QVERIFY(!ConsoleClipboardPolicy::plan(input).hostToClient && !ConsoleClipboardPolicy::plan(input).clientToHost);
        input.ownsControl = true;
        input.adapter = ConsoleSeat::Adapter::Greeter; // the session ended, the greeter is back
        const auto greeter = ConsoleClipboardPolicy::plan(input);
        QVERIFY(!greeter.hostToClient && greeter.clientToHost);
        input.adapter = ConsoleSeat::Adapter::PhysicalUser;
        input.clientUid = Other; // another account's session now owns the seat
        QVERIFY(!ConsoleClipboardPolicy::plan(input).hostToClient && !ConsoleClipboardPolicy::plan(input).clientToHost);
    }

    void directionNamesRoundTrip()
    {
        for (const auto direction : {Direction::Both, Direction::HostToClient, Direction::ClientToHost, Direction::Off}) {
            QCOMPARE(parseDirection(directionName(direction)), std::optional(direction));
        }
        for (const char *bad : {"", "Both", "BOTH", "host_to_client", "on", "true", " both", "both "}) {
            QVERIFY(!parseDirection(QString::fromLatin1(bad)));
        }
        // The reasons are fixed strings, never data.
        for (int reason = int(Reason::Allowed); reason <= int(Reason::Setting); ++reason) {
            QVERIFY(qstrlen(reasonName(Reason(reason))) > 0);
            QVERIFY(qstrcmp(reasonName(Reason(reason)), "unknown") != 0);
        }
    }
};

QTEST_GUILESS_MAIN(ConsoleClipboardPolicyTest)

#include "ConsoleClipboardPolicyTest.moc"
