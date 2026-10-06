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
};
QTEST_GUILESS_MAIN(ConsoleVirtualOutputPlanTest)
#include "ConsoleVirtualOutputPlanTest.moc"
