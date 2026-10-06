// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "ConsoleVirtualOutputPlan.h"
#include <limits>
#include <QTest>

using namespace KRdp;
namespace Plan = ConsoleVirtualOutputPlan;

class ConsoleVirtualOutputPlanTest : public QObject
{
    Q_OBJECT
    static ConsoleVirtualOutputPolicy request()
    {
        const auto policy = ConsoleVirtualOutputPolicy::parse(true, QStringLiteral("replace"), QStringLiteral("client"),
            QSize(1920, 1080), {QSize(3200, 1080), {{QRect(-1280, 160, 1280, 720), false}, {QRect(0, 0, 1920, 1080), true}}});
        Q_ASSERT(policy);
        return *policy;
    }
    static QVector<OutputSnapshot::Output> physical()
    {
        return {{QStringLiteral("DP-1"), true, QPoint(0, 0), 1, QSize(1920, 1080), 1.25},
                {QStringLiteral("DP-2"), true, QPoint(1536, 0), 2, QSize(2560, 1440), 2.0}};
    }
    static QVector<OutputSnapshot::Output> names() { return physical(); }

private Q_SLOTS:
    // OPT-060 S2: the pure per-connection gate. Every refusal means normal Console capture.
    void replaceGateCases_data()
    {
        using P = ConsoleVirtualOutputPolicy;
        QTest::addColumn<int>("permission");
        QTest::addColumn<bool>("block");
        QTest::addColumn<bool>("physicalUser");
        QTest::addColumn<bool>("attempted");
        QTest::addColumn<int>("expected");
        QTest::newRow("no monitor block: capture") << int(P::Permission::Ask) << false << true << false << int(P::Gate::NoMonitorBlock);
        QTest::newRow("block and permission off: capture") << int(P::Permission::Off) << true << true << false << int(P::Gate::PermissionOff);
        QTest::newRow("block and asks: Replace") << int(P::Permission::Ask) << true << true << false << int(P::Gate::Replace);
        QTest::newRow("one attempt only: capture") << int(P::Permission::Ask) << true << true << true << int(P::Gate::AlreadyAttempted);
        QTest::newRow("greeter or lock-less non-user: capture") << int(P::Permission::Ask) << true << false << false << int(P::Gate::NotPhysicalUser);
        QTest::newRow("greeter beats everything") << int(P::Permission::Off) << false << false << true << int(P::Gate::NotPhysicalUser);
    }
    void replaceGateCases()
    {
        QFETCH(int, permission); QFETCH(bool, block); QFETCH(bool, physicalUser); QFETCH(bool, attempted); QFETCH(int, expected);
        QCOMPARE(int(ConsoleVirtualOutputPolicy::gate(ConsoleVirtualOutputPolicy::Permission(permission), block, physicalUser, attempted)), expected);
    }
    void permissionAndMonitorBlockParsing()
    {
        QCOMPARE(ConsoleVirtualOutputPolicy::permissionOf(QStringLiteral("off")), ConsoleVirtualOutputPolicy::Permission::Off);
        QCOMPARE(ConsoleVirtualOutputPolicy::permissionOf(QStringLiteral("replace")), ConsoleVirtualOutputPolicy::Permission::Ask);
        QCOMPARE(ConsoleVirtualOutputPolicy::permissionOf(QStringLiteral("extend")), ConsoleVirtualOutputPolicy::Permission::Ask);
        QCOMPARE(ConsoleVirtualOutputPolicy::planPolicy(QStringLiteral("off")), QStringLiteral("replace"));
        QCOMPARE(ConsoleVirtualOutputPolicy::planPolicy(QStringLiteral("extend")), QStringLiteral("extend"));
        QVERIFY(!ConsoleVirtualOutputPolicy::monitorBlockSent({QSize(1920, 1080), {}}));
        QVERIFY(ConsoleVirtualOutputPolicy::monitorBlockSent({QSize(1920, 1080), {{QRect(0, 0, 1920, 1080), true}}}));
    }

    // OPT-060 D0: a one-monitor client sends no standard block; its explicit request is the other source.
    void effectiveRequestCombinesBlockAndExplicitRequest()
    {
        using P = ConsoleVirtualOutputPolicy;
        const ClientDisplay::Info plain{QSize(1920, 1080), {}};
        const QVector<VideoMonitor> lone{{QRect(0, 0, 1366, 768), true}};
        const QVector<VideoMonitor> pair{{QRect(0, 0, 1920, 1080), true}, {QRect(1920, 0, 1280, 1024), false}};
        // No block and no request: nothing is asked.
        QVERIFY(!P::monitorBlockSent(P::effectiveRequest(plain, std::nullopt)));
        QVERIFY(!P::monitorBlockSent(P::effectiveRequest(plain, QVector<VideoMonitor>{})));
        QCOMPARE(P::effectiveRequest(plain, std::nullopt), plain);
        // A one-monitor request opens the gate, sized by that monitor, and plans one output of that size.
        const auto one = P::effectiveRequest(plain, lone);
        QVERIFY(P::monitorBlockSent(one));
        QCOMPARE(one.desktopSize, QSize(1366, 768));
        QCOMPARE(int(P::gate(P::Permission::Ask, P::monitorBlockSent(one), true, false)), int(P::Gate::Replace));
        const auto policy = P::parse(true, QStringLiteral("replace"), QStringLiteral("client"), QSize(1920, 1080), one);
        QVERIFY(policy);
        QCOMPARE(ClientDisplay::singleSize(policy->client, QSize(1920, 1080)), QSize(1366, 768));
        // Two monitors keep both.
        const auto two = P::effectiveRequest(plain, pair);
        QCOMPARE(two.desktopSize, QSize(3200, 1080));
        QCOMPARE(P::parse(true, QStringLiteral("replace"), QStringLiteral("client"), QSize(1920, 1080), two)->client.monitors.size(), 2);
        // The standard block wins over an explicit request.
        const ClientDisplay::Info block{QSize(3200, 1080), pair};
        QCOMPARE(P::effectiveRequest(block, lone), block);
        // The gate itself is unchanged: permission off, greeter and a spent attempt still capture normally.
        QCOMPARE(int(P::gate(P::Permission::Off, true, true, false)), int(P::Gate::PermissionOff));
        QCOMPARE(int(P::gate(P::Permission::Ask, true, false, false)), int(P::Gate::NotPhysicalUser));
        QCOMPARE(int(P::gate(P::Permission::Ask, true, true, true)), int(P::Gate::AlreadyAttempted));
    }

    void replaceExtendAndParkPreserveClientLayout()
    {
        auto policy = request();
        const auto replace = Plan::build(policy, physical(), names());
        QVERIFY(replace); QVERIFY(replace->replace);
        QCOMPARE(replace->outputs.size(), 2);
        QCOMPARE(replace->outputs[0].position, QPoint(0, 160));
        QCOMPARE(replace->outputs[1].position, QPoint(1280, 0));
        QCOMPARE(replace->outputs[0].parkPosition, QPoint(2816, 160));
        QCOMPARE(replace->outputs[1].parkPosition, QPoint(4096, 0));
        QVERIFY(!replace->outputs[0].primary); QVERIFY(replace->outputs[1].primary);
        policy.policy = ConsoleVirtualOutputPolicy::Policy::Extend;
        const auto extend = Plan::build(policy, physical(), names());
        QVERIFY(extend); QVERIFY(!extend->replace);
        QCOMPARE(extend->outputs[0].position, replace->outputs[0].parkPosition);
        QCOMPARE(extend->outputs[1].position, replace->outputs[1].parkPosition);
        QCOMPARE(extend->outputs[0].name, replace->outputs[0].name);
    }

    void fractionalPhysicalMirrorPreservesNativePixelsAndLogicalPositions()
    {
        auto policy = request(); policy.layout = ConsoleVirtualOutputPolicy::Layout::Physical;
        const auto mirror = Plan::build(policy, physical(), names());
        QVERIFY(mirror); QVERIFY(mirror->mirroredPhysical);
        QCOMPARE(mirror->outputs.size(), 2);
        QCOMPARE(mirror->outputs[0].pixels, QSize(1920, 1080));
        QCOMPARE(mirror->outputs[0].scale, 1.25);
        QCOMPARE(mirror->outputs[1].pixels, QSize(2560, 1440));
        QCOMPARE(mirror->outputs[1].scale, 2.0);
        QCOMPARE(mirror->outputs[1].position, QPoint(1536, 0));
        const auto first = RemoteMonitorGeometry::logicalRect(mirror->outputs[0].position, mirror->outputs[0].pixels, mirror->outputs[0].scale);
        const auto second = RemoteMonitorGeometry::logicalRect(mirror->outputs[1].position, mirror->outputs[1].pixels, mirror->outputs[1].scale);
        QVERIFY(!first.intersects(second)); QCOMPARE(second.x(), first.right() + 1);
    }

    void singleAndFailedMultiUseBoundedFallback()
    {
        auto policy = request(); policy.layout = ConsoleVirtualOutputPolicy::Layout::Single;
        auto single = Plan::build(policy, physical(), names());
        QVERIFY(single); QCOMPARE(single->outputs.size(), 1); QCOMPARE(single->outputs.first().pixels, QSize(3200, 1080));
        policy = request();
        auto fallback = Plan::build(policy, physical(), names(), true);
        QVERIFY(fallback); QVERIFY(fallback->singleFallback); QCOMPARE(fallback->outputs.size(), 1);
        policy.client = policy.normalize({QSize(6000, 2000), {{QRect(0, 0, 3000, 2000), true}, {QRect(3000, 0, 3000, 2000), false}}});
        auto multi = Plan::build(policy, physical(), names());
        QVERIFY(multi); QCOMPARE(multi->outputs.size(), 2); QCOMPARE(multi->outputs[0].pixels, QSize(3000, 2000));
        fallback = Plan::build(policy, physical(), names(), true);
        QVERIFY(fallback); QCOMPARE(fallback->outputs.first().pixels, policy.fallback);
    }

    void noSnapshotFallsBackToClientAndExtend()
    {
        auto policy = request(); policy.layout = ConsoleVirtualOutputPolicy::Layout::Physical;
        const auto result = Plan::build(policy, {}, {{QStringLiteral("Virtual-foreign"), true, QPoint(0, 0), 1, QSize(1920, 1080), 1.0}});
        QVERIFY(result); QVERIFY(!result->replace); QVERIFY(!result->mirroredPhysical);
        QCOMPARE(result->outputs.size(), 2); QCOMPARE(result->outputs[0].pixels, QSize(1280, 720));
        QCOMPARE(result->outputs[0].position, QPoint(1920, 160));
    }

    void collisionsForeignOutputsAndLimitsCannotAcquireOwnership()
    {
        auto policy = request();
        QVERIFY(!Plan::build(policy, physical(), names(), false, 3));
        QVERIFY(Plan::build(policy, physical(), names(), true, 3));
        QVERIFY(!Plan::build(policy, physical(), names(), false, 0));
        auto inventory = names(); inventory.append({QStringLiteral("Virtual-krdp-m0-1280x720"), true, QPoint(2816, 0), 3, QSize(1280, 720), 1.0});
        QVERIFY(!Plan::build(policy, physical(), inventory));
        inventory = names();
        for (int i = 0; i < 13; ++i) inventory.append({QStringLiteral("Virtual-foreign-%1").arg(i), true, QPoint(2816, 0), i + 3, QSize(1280, 720), 1.0});
        QVERIFY(!Plan::build(policy, physical(), inventory));
        auto panels = physical(); panels[0].name = QStringLiteral("Virtual-foreign");
        QVERIFY(!Plan::build(policy, panels, names()));
        panels = physical(); panels[0].scale = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!Plan::build(policy, panels, names()));
        panels = physical(); panels[1].position = QPoint(32767, 0);
        QVERIFY(!Plan::build(policy, panels, panels)); // Extend parking would exceed coordinate bounds.
        inventory = names(); inventory.append({QStringLiteral("Virtual-foreign"), true, QPoint(2816, 0), 3, QSize(1280, 720), 1.0});
        const auto besideForeign = Plan::build(policy, physical(), inventory);
        QVERIFY(besideForeign); QCOMPARE(besideForeign->outputs[0].parkPosition, QPoint(4096, 160));
        auto changed = physical(); changed[0].scale = 2.0;
        QVERIFY(!Plan::build(policy, changed, names())); // A stale snapshot cannot authorize the plan.
        inventory = names(); inventory[0].name = QStringLiteral("DP-1.position.0,0");
        QVERIFY(!Plan::build(policy, {}, inventory));
        inventory = names(); inventory[1].priority = 1;
        QVERIFY(!Plan::build(policy, physical(), inventory));
        policy.enabled = false; QVERIFY(!Plan::build(policy, physical(), names()));
    }

    void invalidPeerTupleUsesAnAtomicFallback()
    {
        auto policy = request();
        policy.client = policy.normalize({QSize(0, 0), {{QRect(std::numeric_limits<int>::min(), 0, 1280, 720), true}}});
        QVERIFY(policy.isValid()); QVERIFY(policy.client.monitors.isEmpty()); QCOMPARE(policy.client.desktopSize, policy.fallback);
        policy.client = policy.normalize({QSize(2000, 1000), {{QRect(0, 0, 1280, 720), true}, {QRect(100, 0, 1280, 720), false}}});
        QVERIFY(policy.isValid()); QVERIFY(policy.client.monitors.isEmpty()); QCOMPARE(policy.client.desktopSize, QSize(2000, 1000));
        QVERIFY(!ConsoleVirtualOutputPolicy::parse(true, QStringLiteral("typo"), QStringLiteral("client"), QSize(1920, 1080), {}));
        QVERIFY(!ConsoleVirtualOutputPolicy::parse(true, QStringLiteral("replace"), QStringLiteral("typo"), QSize(1920, 1080), {}));
        QVERIFY(!ConsoleVirtualOutputPolicy::parse(true, QStringLiteral("replace"), QStringLiteral("client"), QSize(1919, 1080), {}));
        policy.layout = ConsoleVirtualOutputPolicy::Layout(255); QVERIFY(!policy.isValid());
    }
private:
    // ---- OPT-060 M-1: Layout::Mapped (one output per HOST screen, sized for its client monitor) ----
    using MM = ConsoleVirtualOutputPolicy::MappedMonitor;
    using ME = ConsoleVirtualOutputPolicy::MapEntry;
    static ConsoleVirtualOutputPolicy mapped(const QVector<MM> &monitors, const QVector<ME> &mapping = {},
        ConsoleVirtualOutputPolicy::Policy policy = ConsoleVirtualOutputPolicy::Policy::Replace)
    {
        auto result = request();
        result.client = {QSize(1920, 1080), {}};
        result.layout = ConsoleVirtualOutputPolicy::Layout::Mapped;
        result.policy = policy;
        result.mappedMonitors = monitors;
        result.mapping = mapping;
        return result;
    }
    static MM monitor(const QString &id, int x, int w, int h, int scalePercent = 100, bool primary = false)
    {
        return {id, QRect(x, 0, w, h), scalePercent, primary};
    }
    // Host screens DP-1.. side by side, priority 1 = primary, each w x h at scale.
    static QVector<OutputSnapshot::Output> hostRow(int count, int w = 1920, int h = 1080, qreal scale = 1.0)
    {
        QVector<OutputSnapshot::Output> row;
        for (int i = 0; i < count; ++i)
            row.append({QStringLiteral("DP-%1").arg(i + 1), true, QPoint(int(i * std::ceil(w / scale)), 0), i + 1, QSize(w, h), scale});
        return row;
    }
    static std::optional<Plan::Plan> planMapped(const ConsoleVirtualOutputPolicy &policy, const QVector<OutputSnapshot::Output> &host,
        Plan::MappedReport *report, bool forceSingle = false, const QVector<OutputSnapshot::Output> &extra = {})
    {
        return Plan::build(policy, host, host + extra, forceSingle, ClientDisplay::MaxMonitors, report);
    }

private Q_SLOTS:
    void mappedDefaultMappingTable_data()
    {
        QTest::addColumn<int>("hostCount");
        QTest::addColumn<int>("clientCount");
        for (int n = 1; n <= 4; ++n)
            for (int m = 1; m <= 3; ++m) QTest::addRow("N%1xM%2", n, m) << n << m;
    }
    // Spec 4.2: host[i] -> client[min(i, M-1)], host primary alone on the client primary first.
    void mappedDefaultMappingTable()
    {
        QFETCH(int, hostCount); QFETCH(int, clientCount);
        QVector<MM> monitors;
        for (int j = 0; j < clientCount; ++j) monitors.append(monitor(QStringLiteral("C%1").arg(j), j * 1920, 1920, 1080, 100, j == 0));
        Plan::MappedReport report;
        const auto plan = planMapped(mapped(monitors), hostRow(hostCount), &report);
        QVERIFY(plan); QCOMPARE(report.refusal, Plan::Refusal::None);
        QVERIFY(plan->replace); QVERIFY(!plan->mirroredPhysical); QVERIFY(!plan->singleFallback);
        QCOMPARE(plan->outputs.size(), hostCount);
        QCOMPARE(report.screens.size(), hostCount);
        QVERIFY(!report.packed);
        for (int i = 0; i < hostCount; ++i) {
            const auto &out = plan->outputs[i];
            QCOMPARE(out.name, QStringLiteral("Virtual-krdp-h%1-1920x1080").arg(i));
            QCOMPARE(out.pixels, QSize(1920, 1080));
            QCOMPARE(out.primary, i == 0);
            QCOMPARE(out.position, QPoint(i * 1920, 0)); // The host's own relative positions (rule A).
            QCOMPARE(report.screens[i].host, QStringLiteral("DP-%1").arg(i + 1));
            QCOMPARE(report.screens[i].monitor, QStringLiteral("C%1").arg(std::min(i, clientCount - 1)));
            QVERIFY(report.screens[i].isDefault); QVERIFY(!report.screens[i].clamped);
        }
        QStringList unmapped;
        for (int j = hostCount; j < clientCount; ++j) unmapped << QStringLiteral("C%1").arg(j);
        QCOMPARE(report.unmappedMonitors, unmapped);
        QVERIFY(report.unknownHosts.isEmpty());
        // Park positions sit right of the physical desktop (nothing replaced yet) and keep the relative layout.
        QCOMPARE(plan->outputs[0].parkPosition, QPoint(hostCount * 1920, 0));
    }

    // Hal's two 2560x1440 screens onto Buzz's one 1920x1080 @1.25 monitor: both there, k = 2, scale from the monitor.
    void halTwoScreensOnOneBuzzMonitor()
    {
        Plan::MappedReport report;
        const auto plan = planMapped(mapped({monitor(QStringLiteral("eDP-1"), 0, 1920, 1080, 125, true)}),
            {{QStringLiteral("DP-1"), true, QPoint(0, 0), 1, QSize(2560, 1440), 1.0},
             {QStringLiteral("HDMI-A-1"), true, QPoint(2560, 0), 2, QSize(2560, 1440), 1.0}}, &report);
        QVERIFY(plan); QCOMPARE(plan->outputs.size(), 2);
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(plan->outputs[i].pixels, QSize(1920, 1080));
            QCOMPARE(plan->outputs[i].scale, 1.25);
            QCOMPARE(report.screens[i].monitor, QStringLiteral("eDP-1"));
        }
        // Logical 1536x864 each at the host's own x: 0 and 2560 (the mechanism-B rule), a gap between them.
        QCOMPARE(plan->outputs[0].position, QPoint(0, 0));
        QCOMPARE(plan->outputs[1].position, QPoint(2560, 0));
        QVERIFY(!report.packed);
        QCOMPARE(plan->outputs[0].name, QStringLiteral("Virtual-krdp-h0-1920x1080"));
        QCOMPARE(plan->outputs[1].name, QStringLiteral("Virtual-krdp-h1-1920x1080"));
        QVERIFY(plan->outputs[0].primary); QVERIFY(!plan->outputs[1].primary);
        QVERIFY(report.unmappedMonitors.isEmpty());
    }

    // Buzz with two different monitors: host[0] -> monitor 0, host[1] -> monitor 1, each at its own size and scale.
    void halTwoScreensOnTwoDifferentBuzzMonitors()
    {
        Plan::MappedReport report;
        const auto plan = planMapped(mapped({monitor(QStringLiteral("eDP-1"), 0, 1920, 1080, 100, true),
                                             monitor(QStringLiteral("DP-3"), 1920, 1536, 864, 125)}),
            {{QStringLiteral("DP-1"), true, QPoint(0, 0), 1, QSize(2560, 1440), 1.0},
             {QStringLiteral("HDMI-A-1"), true, QPoint(2560, 0), 2, QSize(2560, 1440), 1.0}}, &report);
        QVERIFY(plan); QCOMPARE(plan->outputs.size(), 2);
        QCOMPARE(plan->outputs[0].pixels, QSize(1920, 1080)); QCOMPARE(plan->outputs[0].scale, 1.0);
        QCOMPARE(plan->outputs[1].pixels, QSize(1536, 864)); QCOMPARE(plan->outputs[1].scale, 1.25);
        QCOMPARE(plan->outputs[1].name, QStringLiteral("Virtual-krdp-h1-1536x864"));
        QCOMPARE(report.screens[0].monitor, QStringLiteral("eDP-1"));
        QCOMPARE(report.screens[1].monitor, QStringLiteral("DP-3"));
        QVERIFY(report.unmappedMonitors.isEmpty());
        QCOMPARE(plan->outputs[1].position, QPoint(2560, 0));
    }

    void mappedExplicitMappingOverridesDefault()
    {
        Plan::MappedReport report;
        // Host DP-2 is the primary here (priority 1) and is pinned to C1; DP-1 follows the default.
        auto host = hostRow(2);
        host[0].priority = 2; host[1].priority = 1;
        const auto plan = planMapped(mapped({monitor(QStringLiteral("C0"), 0, 1920, 1080, 100, true), monitor(QStringLiteral("C1"), 1920, 1280, 720)},
                                            {{QStringLiteral("DP-2"), QStringLiteral("C1")}, {QStringLiteral("NOPE-9"), QStringLiteral("C0")}}), host, &report);
        QVERIFY(plan);
        // Order: the primary first (DP-2), then DP-1. Primary output = the host primary's.
        QCOMPARE(report.screens[0].host, QStringLiteral("DP-2")); QVERIFY(report.screens[0].primary);
        QCOMPARE(report.screens[0].monitor, QStringLiteral("C1")); QVERIFY(!report.screens[0].isDefault);
        QCOMPARE(report.screens[1].host, QStringLiteral("DP-1")); QCOMPARE(report.screens[1].monitor, QStringLiteral("C1")); // i = 1 -> min(1, 1)
        QVERIFY(report.screens[1].isDefault);
        QCOMPARE(plan->outputs[0].pixels, QSize(1280, 720));
        QCOMPARE(plan->outputs[0].position, QPoint(1920, 0)); // DP-2 sits right of DP-1 on the host.
        QCOMPARE(plan->outputs[1].position, QPoint(0, 0));
        QVERIFY(plan->outputs[0].primary);
        QCOMPARE(report.unknownHosts, QStringList{QStringLiteral("NOPE-9")}); // Ignored and reported, not refused.
        QCOMPARE(report.unmappedMonitors, QStringList{QStringLiteral("C0")}); // The client draws "No host screen mapped".
    }

    void mappedOverlapIsPacked()
    {
        Plan::MappedReport report;
        // Host screens 1920 wide side by side; the client monitor is 2560 wide, so rule A would overlap.
        auto plan = planMapped(mapped({monitor(QStringLiteral("C0"), 0, 2560, 1440, 100, true)}), hostRow(2), &report);
        QVERIFY(plan); QVERIFY(report.packed);
        QCOMPARE(plan->outputs[0].position, QPoint(0, 0));
        QCOMPARE(plan->outputs[1].position, QPoint(2560, 0));
        // Vertical arrangement: rows stacked, each as tall as its tallest output.
        QVector<OutputSnapshot::Output> stacked = {{QStringLiteral("DP-1"), true, QPoint(0, 0), 1, QSize(1920, 1080), 1.0},
                                                   {QStringLiteral("DP-2"), true, QPoint(0, 1080), 2, QSize(1920, 1080), 1.0}};
        plan = planMapped(mapped({monitor(QStringLiteral("C0"), 0, 1920, 1440, 100, true)}), stacked, &report);
        QVERIFY(plan); QVERIFY(report.packed);
        QCOMPARE(plan->outputs[0].position, QPoint(0, 0));
        QCOMPARE(plan->outputs[1].position, QPoint(0, 1440));
        // A 2x2 grid keeps rows and the order inside them.
        QVector<OutputSnapshot::Output> grid = {{QStringLiteral("A"), true, QPoint(0, 0), 1, QSize(1920, 1080), 1.0},
                                                {QStringLiteral("B"), true, QPoint(1920, 0), 2, QSize(1920, 1080), 1.0},
                                                {QStringLiteral("C"), true, QPoint(0, 1080), 3, QSize(1920, 1080), 1.0},
                                                {QStringLiteral("D"), true, QPoint(1920, 1080), 4, QSize(1920, 1080), 1.0}};
        plan = planMapped(mapped({monitor(QStringLiteral("C0"), 0, 2560, 1440, 100, true)}), grid, &report);
        QVERIFY(plan); QVERIFY(report.packed);
        QCOMPARE(plan->outputs.size(), 4);
        // Output order is primary first, then x, then y: A, C, B, D (the order of the screens, not of the rows).
        QCOMPARE(plan->outputs[0].position, QPoint(0, 0)); QCOMPARE(plan->outputs[1].position, QPoint(0, 1440));
        QCOMPARE(plan->outputs[2].position, QPoint(2560, 0)); QCOMPARE(plan->outputs[3].position, QPoint(2560, 1440));
    }

    void mappedCapsAndRefusals()
    {
        Plan::MappedReport report;
        const auto one = mapped({monitor(QStringLiteral("C0"), 0, 1920, 1080, 100, true)});
        // N = 5 > the hard cap of 4: refused, nothing replaced.
        QVERIFY(!planMapped(one, hostRow(5), &report)); QCOMPARE(report.refusal, Plan::Refusal::TooManyScreens);
        QVERIFY(planMapped(one, hostRow(4), &report)); QCOMPARE(report.refusal, Plan::Refusal::None);
        // No enabled host screen.
        auto none = hostRow(2); none[0].enabled = none[1].enabled = false;
        QVERIFY(!planMapped(one, none, &report)); QCOMPARE(report.refusal, Plan::Refusal::NoHostScreens);
        // Disabled host screens are not mapped.
        none[1].enabled = true;
        const auto half = planMapped(one, none, &report);
        QVERIFY(half); QCOMPARE(half->outputs.size(), 1); QCOMPARE(report.screens[0].host, QStringLiteral("DP-2"));
        // Union above 8192 even when packed edge to edge: two 4096 fit, three do not.
        const auto wide = mapped({monitor(QStringLiteral("C0"), 0, 4096, 2160, 100, true)});
        QVERIFY(planMapped(wide, hostRow(2), &report)); QVERIFY(report.packed);
        QVERIFY(!planMapped(wide, hostRow(3), &report)); QCOMPARE(report.refusal, Plan::Refusal::DesktopTooLarge);
        // The creation bound counts every existing output (spec 4.1: |inventory| + |H| <= 16).
        QVector<OutputSnapshot::Output> extra;
        for (int i = 0; i < 13; ++i) extra.append({QStringLiteral("Virtual-other%1").arg(i), false, QPoint(), 0, QSize(1920, 1080), 1.0});
        QVERIFY(!planMapped(one, hostRow(2), &report, false, extra)); QCOMPARE(report.refusal, Plan::Refusal::TooManyOutputs);
        extra.removeLast();
        QVERIFY(planMapped(one, hostRow(2), &report, false, extra));
        // A stale name collision cannot claim an existing output.
        QVector<OutputSnapshot::Output> clash = {{QStringLiteral("Virtual-krdp-h0-1920x1080"), false, QPoint(), 0, QSize(1920, 1080), 1.0}};
        QVERIFY(!planMapped(one, hostRow(1), &report, false, clash));
        // Without a physical screen there is nothing to map (extend/park on an empty seat is Layout::Client's job).
        QVERIFY(!Plan::build(one, {}, {}));
    }

    void mappedSizeClampScaleAndRounding()
    {
        Plan::MappedReport report;
        // Larger than 4096: both sides scaled to fit, aspect kept, even; the scale of the monitor stays.
        auto plan = planMapped(mapped({monitor(QStringLiteral("C0"), 0, 5120, 2880, 150, true)}), hostRow(1), &report);
        QVERIFY(plan);
        QCOMPARE(plan->outputs[0].pixels, QSize(4096, 2304));
        QCOMPARE(plan->outputs[0].scale, 1.5);
        QVERIFY(report.screens[0].clamped);
        QCOMPARE(plan->outputs[0].name, QStringLiteral("Virtual-krdp-h0-4096x2304"));
        // 8192x640 would clamp below the 640 minimum: refused.
        QVERIFY(!planMapped(mapped({monitor(QStringLiteral("C0"), 0, 8192, 640, 100, true)}), hostRow(1), &report));
        QCOMPARE(report.refusal, Plan::Refusal::BadMonitorSize);
        // Odd pixel sizes are floored to even.
        plan = planMapped(mapped({monitor(QStringLiteral("C0"), 0, 1921, 1081, 100, true)}), hostRow(1), &report);
        QVERIFY(plan); QCOMPARE(plan->outputs[0].pixels, QSize(1920, 1080)); QVERIFY(!report.screens[0].clamped);
        // Mixed scales per output; positions stay in logical space (1.25 -> 1536, 1.5 -> 1280 logical).
        plan = planMapped(mapped({monitor(QStringLiteral("C0"), 0, 1920, 1080, 125, true), monitor(QStringLiteral("C1"), 1920, 1920, 1080, 150)}),
            {{QStringLiteral("DP-1"), true, QPoint(0, 0), 1, QSize(1920, 1080), 1.0}, {QStringLiteral("DP-2"), true, QPoint(1536, 0), 2, QSize(1920, 1080), 1.0}}, &report);
        QVERIFY(plan);
        QCOMPARE(plan->outputs[0].scale, 1.25); QCOMPARE(plan->outputs[1].scale, 1.5);
        QCOMPARE(plan->outputs[1].position, QPoint(1536, 0)); QVERIFY(!report.packed); // 1536 logical wide: touches, no overlap.
    }

    void mappedForceSingleExtendAndOrdering()
    {
        Plan::MappedReport report;
        const auto policy = mapped({monitor(QStringLiteral("C1"), 1920, 1280, 720), monitor(QStringLiteral("C0"), 0, 1920, 1080, 125, true)});
        // forceSingle: one output of the client primary's size, at the anchor, scale kept; everything else unmapped.
        const auto single = planMapped(policy, hostRow(2), &report, true);
        QVERIFY(single); QVERIFY(single->singleFallback); QCOMPARE(single->outputs.size(), 1);
        QCOMPARE(single->outputs[0].pixels, QSize(1920, 1080)); QCOMPARE(single->outputs[0].scale, 1.25);
        QCOMPARE(single->outputs[0].position, QPoint(0, 0)); QVERIFY(single->outputs[0].primary);
        QCOMPARE(report.unmappedMonitors, QStringList{QStringLiteral("C1")});
        // Extend keeps the screens on: positions are the park positions and nothing is replaced.
        const auto extend = planMapped(mapped(policy.mappedMonitors, {}, ConsoleVirtualOutputPolicy::Policy::Extend), hostRow(2), &report);
        QVERIFY(extend); QVERIFY(!extend->replace);
        QCOMPARE(extend->outputs[0].position, extend->outputs[0].parkPosition);
        QCOMPARE(extend->outputs[0].position, QPoint(3840, 0));
        // Client order is primary first even when listed last: host[1] lands on the non-primary monitor C1.
        QCOMPARE(report.screens[0].monitor, QStringLiteral("C0")); QCOMPARE(report.screens[1].monitor, QStringLiteral("C1"));
    }

    void mappedHostOrderIsPrimaryFirstThenSpatial()
    {
        Plan::MappedReport report;
        // The leftmost screen is NOT the primary: the primary is output 0, keeping its own relative position.
        QVector<OutputSnapshot::Output> host = {{QStringLiteral("L"), true, QPoint(0, 0), 2, QSize(1920, 1080), 1.0},
                                                {QStringLiteral("M"), true, QPoint(1920, 0), 1, QSize(1920, 1080), 1.0},
                                                {QStringLiteral("R"), true, QPoint(3840, 0), 3, QSize(1920, 1080), 1.0}};
        const auto plan = planMapped(mapped({monitor(QStringLiteral("C0"), 0, 1920, 1080, 100, true), monitor(QStringLiteral("C1"), 1920, 1920, 1080)}), host, &report);
        QVERIFY(plan);
        QCOMPARE(report.screens[0].host, QStringLiteral("M")); QCOMPARE(report.screens[1].host, QStringLiteral("L")); QCOMPARE(report.screens[2].host, QStringLiteral("R"));
        QCOMPARE(plan->outputs[0].position, QPoint(1920, 0)); QCOMPARE(plan->outputs[1].position, QPoint(0, 0)); QCOMPARE(plan->outputs[2].position, QPoint(3840, 0));
        QCOMPARE(report.screens[0].monitor, QStringLiteral("C0")); QCOMPARE(report.screens[1].monitor, QStringLiteral("C1")); QCOMPARE(report.screens[2].monitor, QStringLiteral("C1"));
        QVERIFY(plan->outputs[0].primary);
    }

    void mappedPolicyValidation_data()
    {
        QTest::addColumn<int>("variant");
        QTest::addColumn<bool>("valid");
        QTest::newRow("valid") << 0 << true;
        QTest::newRow("no monitors") << 1 << false;
        QTest::newRow("two primaries") << 2 << false;
        QTest::newRow("no primary") << 3 << false;
        QTest::newRow("duplicate id") << 4 << false;
        QTest::newRow("scale off the 5% grid") << 5 << false;
        QTest::newRow("scale below 100") << 6 << false;
        QTest::newRow("scale above 400") << 7 << false;
        QTest::newRow("too small") << 8 << false;
        QTest::newRow("too large") << 9 << false;
        QTest::newRow("mapping to an unknown monitor") << 10 << false;
        QTest::newRow("duplicate mapped host") << 11 << false;
        QTest::newRow("empty id") << 12 << false;
        QTest::newRow("more than 16 monitors") << 13 << false;
        QTest::newRow("mapping on a non-mapped layout") << 14 << false;
        QTest::newRow("monitors on a non-mapped layout") << 15 << false;
        QTest::newRow("control character in a host name") << 16 << false;
        QTest::newRow("unknown host names are tolerated") << 17 << true;
    }
    void mappedPolicyValidation()
    {
        QFETCH(int, variant); QFETCH(bool, valid);
        auto p = mapped({monitor(QStringLiteral("C0"), 0, 1920, 1080, 100, true), monitor(QStringLiteral("C1"), 1920, 1280, 720)},
                        {{QStringLiteral("DP-1"), QStringLiteral("C1")}});
        switch (variant) {
        case 1: p.mappedMonitors.clear(); break;
        case 2: p.mappedMonitors[1].primary = true; break;
        case 3: p.mappedMonitors[0].primary = false; break;
        case 4: p.mappedMonitors[1].id = QStringLiteral("C0"); break;
        case 5: p.mappedMonitors[1].scalePercent = 112; break;
        case 6: p.mappedMonitors[1].scalePercent = 95; break;
        case 7: p.mappedMonitors[1].scalePercent = 405; break;
        case 8: p.mappedMonitors[1].geometry.setWidth(639); break;
        case 9: p.mappedMonitors[1].geometry.setHeight(16385); break;
        case 10: p.mapping[0].monitor = QStringLiteral("nope"); break;
        case 11: p.mapping.append({QStringLiteral("DP-1"), QStringLiteral("C0")}); break;
        case 12: p.mappedMonitors[1].id.clear(); break;
        case 13: for (int i = 0; i < 15; ++i) p.mappedMonitors.append(monitor(QStringLiteral("X%1").arg(i), 0, 1920, 1080)); break;
        case 14: p.layout = ConsoleVirtualOutputPolicy::Layout::Client; p.mappedMonitors.clear(); break;
        case 15: p.layout = ConsoleVirtualOutputPolicy::Layout::Client; p.mapping.clear(); break;
        case 16: p.mapping[0].host = QStringLiteral("DP\n1"); break;
        case 17: p.mapping[0].host = QStringLiteral("no-such-connector"); break;
        }
        QCOMPARE(p.isValid(), valid);
        if (!valid) { Plan::MappedReport report; QVERIFY(!planMapped(p, hostRow(1), &report)); QCOMPARE(report.refusal, Plan::Refusal::Invalid); }
    }

    // The default layout is untouched: a request that is not Mapped still plans per client monitor.
    void nonMappedLayoutsAreUnchanged()
    {
        Plan::MappedReport report;
        const auto plan = Plan::build(request(), physical(), names(), false, ClientDisplay::MaxMonitors, &report);
        QVERIFY(plan); QCOMPARE(plan->outputs[0].name, QStringLiteral("Virtual-krdp-m0-1280x720"));
        QCOMPARE(report.refusal, Plan::Refusal::None); QVERIFY(report.screens.isEmpty());
    }
};
QTEST_GUILESS_MAIN(ConsoleVirtualOutputPlanTest)
#include "ConsoleVirtualOutputPlanTest.moc"
