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
