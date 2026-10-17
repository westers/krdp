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

    // OPT-078: file copy, state by state (spec 4.5 + amendment 12.1). "started" = a transfer in that direction began before this evaluation.
    void filesStateTable_data()
    {
        QTest::addColumn<int>("route");
        QTest::addColumn<int>("adapter");
        QTest::addColumn<bool>("locked");
        QTest::addColumn<quint32>("clientUid");
        QTest::addColumn<bool>("controls");
        QTest::addColumn<bool>("hostToClientNew");
        QTest::addColumn<bool>("clientToHostNew");
        QTest::addColumn<bool>("hostToClientKeeps");
        QTest::addColumn<bool>("clientToHostKeeps");
        QTest::addColumn<int>("reason");
        using A = ConsoleSeat::Adapter;
        const int console = int(Route::Console), virt = int(Route::Virtual);
        QTest::newRow("greeter: nothing") << console << int(A::Greeter) << false << Owner << true << false << false << false << false << int(Reason::Greeter);
        QTest::newRow("greeter, another account") << console << int(A::Greeter) << false << Other << true << false << false << false << false << int(Reason::Greeter);
        QTest::newRow("locked, same account: new client->host denied, host->client allowed") << console << int(A::PhysicalUser) << true << Owner << true << true << false << true << true << int(Reason::LockedNewTransfer);
        QTest::newRow("locked, other account") << console << int(A::PhysicalUser) << true << Other << true << false << false << false << false << int(Reason::ForeignLocked);
        QTest::newRow("unlocked, same account") << console << int(A::PhysicalUser) << false << Owner << true << true << true << true << true << int(Reason::Allowed);
        QTest::newRow("unlocked, other account") << console << int(A::PhysicalUser) << false << Other << true << false << false << false << false << int(Reason::ForeignUnlocked);
        QTest::newRow("viewer on own desktop") << console << int(A::PhysicalUser) << false << Owner << false << false << false << false << false << int(Reason::Viewer);
        QTest::newRow("viewer on a locked desktop") << console << int(A::PhysicalUser) << true << Owner << false << false << false << false << false << int(Reason::Viewer);
        QTest::newRow("no seat adapter") << console << int(A::None) << false << Owner << true << false << false << false << false << int(Reason::NoSeat);
        QTest::newRow("no uid") << console << int(A::PhysicalUser) << false << 0u << true << false << false << false << false << int(Reason::UnknownUser);
        QTest::newRow("virtual owner") << virt << int(A::VirtualUser) << false << Owner << true << true << true << true << true << int(Reason::Allowed);
        QTest::newRow("virtual, not the owner") << virt << int(A::VirtualUser) << false << Other << true << false << false << false << false << int(Reason::NoSeat);
        QTest::newRow("virtual, not controlling") << virt << int(A::VirtualUser) << false << Owner << false << false << false << false << false << int(Reason::Viewer);
    }
    void filesStateTable()
    {
        QFETCH(int, route);
        QFETCH(int, adapter);
        QFETCH(bool, locked);
        QFETCH(quint32, clientUid);
        QFETCH(bool, controls);
        QFETCH(bool, hostToClientNew);
        QFETCH(bool, clientToHostNew);
        QFETCH(bool, hostToClientKeeps);
        QFETCH(bool, clientToHostKeeps);
        QFETCH(int, reason);
        Input input;
        input.route = Route(route);
        input.adapter = ConsoleSeat::Adapter(adapter);
        input.locked = locked;
        input.clientUid = clientUid ? std::optional<quint32>(clientUid) : std::nullopt;
        input.sessionUid = Owner;
        input.ownsControl = controls;
        // nothing started: the "new transfer" rule
        auto plan = ConsoleClipboardPolicy::plan(input);
        QCOMPARE(plan.files.hostToClient, hostToClientNew);
        QCOMPARE(plan.files.clientToHost, clientToHostNew);
        QCOMPARE(int(plan.files.reason), reason);
        // started in that direction: the "continues" rule, each direction on its own
        input.hostToClientFileStarted = true;
        plan = ConsoleClipboardPolicy::plan(input);
        QCOMPARE(plan.files.hostToClient, hostToClientKeeps);
        QCOMPARE(plan.files.clientToHost, clientToHostNew); // the other direction is still a new request
        input.hostToClientFileStarted = false;
        input.clientToHostFileStarted = true;
        plan = ConsoleClipboardPolicy::plan(input);
        QCOMPARE(plan.files.clientToHost, clientToHostKeeps);
        QCOMPARE(plan.files.hostToClient, hostToClientNew);
    }
    // Steve's decision 2026-10-17: the lock only denies NEW transfers; one already started keeps going.
    void inProgressTransferContinuesAfterTheScreenLocks()
    {
        Input input;
        input.adapter = ConsoleSeat::Adapter::PhysicalUser;
        input.clientUid = Owner;
        input.sessionUid = Owner;
        input.ownsControl = true;
        input.locked = false;
        QVERIFY(ConsoleClipboardPolicy::plan(input).files.clientToHost); // allowed to start while unlocked
        input.clientToHostFileStarted = true;
        input.locked = true; // the screen locks mid-transfer
        auto plan = ConsoleClipboardPolicy::plan(input);
        QVERIFY(plan.files.clientToHost);                         // it continues
        QCOMPARE(plan.files.reason, Reason::Allowed);              // no "locked" refusal is reported for it
        input.clientToHostFileStarted = false;                     // it finished; a new one is now denied
        plan = ConsoleClipboardPolicy::plan(input);
        QVERIFY(!plan.files.clientToHost);
        QCOMPARE(plan.files.reason, Reason::LockedNewTransfer);
        QVERIFY(plan.files.hostToClient);                          // host->client stays allowed while locked
        QVERIFY(plan.hostToClient && plan.clientToHost);           // and text/images are untouched
        // Everything else still ends a started transfer, locked or not.
        input.clientToHostFileStarted = input.hostToClientFileStarted = true;
        input.ownsControl = false;
        plan = ConsoleClipboardPolicy::plan(input);
        QVERIFY(!plan.files.clientToHost && !plan.files.hostToClient);
        input.ownsControl = true;
        input.clientUid = Other;
        plan = ConsoleClipboardPolicy::plan(input);
        QVERIFY(!plan.files.clientToHost && !plan.files.hostToClient);
        input.clientUid = Owner;
        input.adapter = ConsoleSeat::Adapter::Greeter; // logged out underneath it
        plan = ConsoleClipboardPolicy::plan(input);
        QVERIFY(!plan.files.clientToHost && !plan.files.hostToClient);
        input.adapter = ConsoleSeat::Adapter::PhysicalUser;
        input.filesSetting = Direction::Off; // the administrator turned file copy off
        plan = ConsoleClipboardPolicy::plan(input);
        QVERIFY(!plan.files.clientToHost && !plan.files.hostToClient);
        QCOMPARE(plan.files.reason, Reason::Setting); // the setting narrowed an allowed state
    }
    void virtualHasNoLockedState()
    {
        Input input;
        input.route = Route::Virtual;
        input.adapter = ConsoleSeat::Adapter::VirtualUser;
        input.clientUid = Owner;
        input.sessionUid = Owner;
        input.ownsControl = true;
        input.locked = true; // meaningless on this route; never narrows
        const auto plan = ConsoleClipboardPolicy::plan(input);
        QVERIFY(plan.files.hostToClient && plan.files.clientToHost);
    }
    // files = state AND ClipboardFiles AND ClipboardDirection, for every state and every pair of settings.
    void filesSettingsNeverWiden_data()
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
    void filesSettingsNeverWiden()
    {
        QFETCH(int, adapter);
        QFETCH(bool, locked);
        QFETCH(quint32, clientUid);
        QFETCH(int, route);
        const auto directions = {Direction::Both, Direction::HostToClient, Direction::ClientToHost, Direction::Off};
        for (const bool startedH : {false, true}) {
            for (const bool startedC : {false, true}) {
                Input input;
                input.route = Route(route);
                input.adapter = ConsoleSeat::Adapter(adapter);
                input.locked = locked;
                input.clientUid = clientUid;
                input.sessionUid = Owner;
                input.ownsControl = true;
                input.hostToClientFileStarted = startedH;
                input.clientToHostFileStarted = startedC;
                const auto state = ConsoleClipboardPolicy::plan(input).files; // both settings wide open
                for (const auto files : directions) {
                    for (const auto direction : directions) {
                        input.filesSetting = files;
                        input.setting = direction;
                        const auto plan = ConsoleClipboardPolicy::plan(input);
                        const auto allows = [](Direction d, bool hostToClient) {
                            return d == Direction::Both || d == (hostToClient ? Direction::HostToClient : Direction::ClientToHost);
                        };
                        QCOMPARE(plan.files.hostToClient, state.hostToClient && allows(files, true) && allows(direction, true));
                        QCOMPARE(plan.files.clientToHost, state.clientToHost && allows(files, false) && allows(direction, false));
                        // text and images are unaffected by ClipboardFiles
                        Input textOnly = input;
                        textOnly.filesSetting = Direction::Both;
                        QCOMPARE(plan.hostToClient, ConsoleClipboardPolicy::plan(textOnly).hostToClient);
                        QCOMPARE(plan.clientToHost, ConsoleClipboardPolicy::plan(textOnly).clientToHost);
                    }
                }
            }
        }
    }
    void filesSettingReportsAsSettingOnlyWhenItNarrowedAnAllowedState()
    {
        Input input;
        input.adapter = ConsoleSeat::Adapter::PhysicalUser;
        input.clientUid = Owner;
        input.sessionUid = Owner;
        input.ownsControl = true;
        input.filesSetting = Direction::HostToClient;
        auto plan = ConsoleClipboardPolicy::plan(input);
        QVERIFY(plan.files.hostToClient && !plan.files.clientToHost);
        QCOMPARE(plan.files.reason, Reason::Setting);
        input.filesSetting = Direction::Both;
        QCOMPARE(ConsoleClipboardPolicy::plan(input).files.reason, Reason::Allowed);
        input.adapter = ConsoleSeat::Adapter::Greeter;
        input.filesSetting = Direction::Off;
        QCOMPARE(ConsoleClipboardPolicy::plan(input).files.reason, Reason::Greeter); // the state's reason wins
    }
    void defaultsAreBoth()
    {
        Input input;
        QCOMPARE(input.filesSetting, Direction::Both);
        QVERIFY(!input.hostToClientFileStarted && !input.clientToHostFileStarted);
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
        for (int reason = int(Reason::Allowed); reason <= int(Reason::LockedNewTransfer); ++reason) {
            QVERIFY(qstrlen(reasonName(Reason(reason))) > 0);
            QVERIFY(qstrcmp(reasonName(Reason(reason)), "unknown") != 0);
        }
    }
};

QTEST_GUILESS_MAIN(ConsoleClipboardPolicyTest)

#include "ConsoleClipboardPolicyTest.moc"
