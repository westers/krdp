// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QFile>
#include <QTest>

#include "ConsoleTopologyReadback.h"

class ConsoleTopologyReadbackTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void creatorOwnershipIsExplicitAndComplete()
    {
        KRdp::ConsoleWorkerWire::Topology captured{{
            {QStringLiteral("DP-1"), QSize(1280, 720), QRect(0, 0, 1280, 720), 1, true, 1},
            {QStringLiteral("Virtual-owned"), QSize(1280, 720), QRect(1280, 0, 1280, 720), 1, false, 2}}};
        const auto owned = KRdp::ConsoleTopologyReadback::withOwnedVirtuals(captured,
            {QStringLiteral("Virtual-owned")});
        QVERIFY(owned);
        QVERIFY(owned->outputs[0].physical);
        QVERIFY(!owned->outputs[1].physical);
        QVERIFY(!KRdp::ConsoleTopologyReadback::withOwnedVirtuals(captured,
            {QStringLiteral("Virtual-missing")}));
        const auto none = KRdp::ConsoleTopologyReadback::withOwnedVirtuals(captured, {});
        QVERIFY(none);
        QVERIFY(none->outputs[1].physical); // Names alone never establish ownership.
    }

    void idleKeyframeCanConfirmFreshUnchangedKScreen()
    {
        QFile file(QFINDTESTDATA(QStringLiteral("data/virtual-fit/1280x720.h264")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        KRdp::VideoFrame captured;
        captured.size = QSize(1280, 720);
        captured.data = file.readAll();
        captured.isKeyFrame = true;
        captured.monitors = {{QRect(0, 0, 640, 720), true}, {QRect(640, 0, 640, 720), false}};

        KRdp::ConsoleWorkerWire::Outputs outputs;
        outputs.monitors = {{QStringLiteral("DP-1"), QRect(0, 0, 640, 720), 1, true},
            {QStringLiteral("HDMI-A-1"), QRect(640, 0, 640, 720), 1, false}};
        KRdp::RetainedKScreenReadback::Snapshot fresh;
        fresh.outputs = {
            {.backendKey = QStringLiteral("DP-1"), .name = QStringLiteral("DP-1"), .nativePixels = QSize(640, 720),
                .logicalGeometry = QRect(0, 0, 640, 720), .scale = 1, .enabled = true, .primary = true, .physical = true, .owner = {}},
            {.backendKey = QStringLiteral("HDMI-A-1"), .name = QStringLiteral("HDMI-A-1"), .nativePixels = QSize(640, 720),
                .logicalGeometry = QRect(640, 0, 640, 720), .scale = 1, .enabled = true, .physical = true, .owner = {}},
        };
        const auto confirmed = KRdp::ConsoleTopologyReadback::confirmed(fresh, outputs, captured);
        QVERIFY(confirmed);
        QCOMPARE(confirmed->outputs.size(), 2);
        const QByteArray priorityJson = R"({"outputs":[{"name":"DP-1","priority":1},{"name":"HDMI-A-1","priority":3}]})";
        const auto ordered = KRdp::ConsoleTopologyReadback::withPriorities(*confirmed, priorityJson, fresh);
        QVERIFY(ordered);
        QCOMPARE(ordered->outputs[0].priority, quint8(1));
        QCOMPARE(ordered->outputs[1].priority, quint8(3));
        QVERIFY(!KRdp::ConsoleTopologyReadback::withPriorities(*confirmed,
            R"({"outputs":[{"name":"DP-1","priority":1},{"name":"HDMI-A-1","priority":1}]})", fresh));
        auto stale = *confirmed;
        stale.outputs[1].logical.moveLeft(650);
        QVERIFY(!KRdp::ConsoleTopologyReadback::withPriorities(stale, priorityJson, fresh));

        fresh.outputs[1].logicalGeometry.moveLeft(650);
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmed(fresh, outputs, captured));
        fresh.outputs[1].logicalGeometry.moveLeft(640);
        captured.monitors[1].geometry.moveLeft(650);
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmed(fresh, outputs, captured));
        captured.monitors[1].geometry.moveLeft(640);
        captured.data.truncate(captured.data.size() / 2);
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmed(fresh, outputs, captured));
    }

    void independentPhysicalKeyframesConfirmEveryOutput()
    {
        QFile file(QFINDTESTDATA(QStringLiteral("data/virtual-fit/1280x720.h264")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        KRdp::VideoFrame first;
        first.size = QSize(1280, 720);
        first.data = file.readAll();
        first.isKeyFrame = true;
        first.monitorIndex = 0;
        KRdp::VideoFrame second = first;
        second.monitorIndex = 1;
        KRdp::ConsoleWorkerWire::Outputs outputs;
        outputs.monitors = {
            {QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true},
            {QStringLiteral("HDMI-A-1"), QRect(1280, 0, 1280, 720), 1, false},
        };
        KRdp::RetainedKScreenReadback::Snapshot fresh;
        fresh.outputs = {
            {.backendKey = QStringLiteral("DP-1"), .name = QStringLiteral("DP-1"), .nativePixels = QSize(1280, 720),
                .logicalGeometry = QRect(0, 0, 1280, 720), .scale = 1, .enabled = true, .primary = true, .physical = true, .owner = {}},
            {.backendKey = QStringLiteral("HDMI-A-1"), .name = QStringLiteral("HDMI-A-1"), .nativePixels = QSize(1280, 720),
                .logicalGeometry = QRect(1280, 0, 1280, 720), .scale = 1, .enabled = true, .physical = true, .owner = {}},
        };
        const auto confirmed = KRdp::ConsoleTopologyReadback::confirmedMulti(fresh, outputs, {first, second});
        QVERIFY(confirmed);
        QCOMPARE(confirmed->outputs.size(), 2);
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmedMulti(fresh, outputs, {first}));
        second.data.truncate(second.data.size() / 2);
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmedMulti(fresh, outputs, {first, second}));
        second = first;
        second.monitorIndex = 1;
        fresh.outputs[1].logicalGeometry.moveLeft(1300);
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmedMulti(fresh, outputs, {first, second}));
    }

    void singlePhysicalModeCannotUseAnOldEncodedSize()
    {
        QFile file(QFINDTESTDATA(QStringLiteral("data/virtual-fit/1280x720.h264")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        KRdp::VideoFrame old;
        old.size = QSize(1280, 720);
        old.data = file.readAll();
        old.isKeyFrame = true;
        old.monitors = {{QRect(0, 0, 1280, 720), true}};
        KRdp::ConsoleWorkerWire::Outputs outputs;
        outputs.monitors = {{QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1.5, true}};
        KRdp::RetainedKScreenReadback::Snapshot fresh;
        fresh.outputs = {{.backendKey = QStringLiteral("DP-1"), .name = QStringLiteral("DP-1"),
            .nativePixels = QSize(1920, 1080), .logicalGeometry = QRect(0, 0, 1280, 720),
            .scale = 1.5, .enabled = true, .primary = true, .physical = true, .owner = {}}};
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmed(fresh, outputs, old));
    }

    void singleConsoleOutputNeedsExactCaptureBeforeCreation()
    {
        QFile file(QFINDTESTDATA(QStringLiteral("data/virtual-fit/1280x720.h264")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        KRdp::VideoFrame frame;
        frame.size = QSize(1280, 720);
        frame.data = file.readAll();
        frame.isKeyFrame = true;
        frame.monitorIndex = 0;
        frame.monitors = {{QRect(0, 0, 1280, 720), true}};
        KRdp::ConsoleWorkerWire::Outputs outputs;
        outputs.monitors = {{QStringLiteral("DP-1"), QRect(0, 0, 1280, 720), 1, true}};
        KRdp::RetainedKScreenReadback::Snapshot fresh;
        fresh.outputs = {{.backendKey = QStringLiteral("DP-1"), .name = QStringLiteral("DP-1"),
            .nativePixels = QSize(1280, 720), .logicalGeometry = QRect(0, 0, 1280, 720),
            .scale = 1, .enabled = true, .primary = true, .physical = true, .owner = {}}};
        QVERIFY(KRdp::ConsoleTopologyReadback::confirmed(fresh, outputs, frame));
        fresh.outputs[0].logicalGeometry.moveLeft(100);
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmed(fresh, outputs, frame));
        outputs.compositorOrigin = QPoint(100, 0);
        QVERIFY(KRdp::ConsoleTopologyReadback::confirmed(fresh, outputs, frame));
        outputs.compositorOrigin = QPoint(0, 0);
        fresh.outputs[0].logicalGeometry.moveLeft(0);
        frame.isKeyFrame = false;
        QVERIFY(!KRdp::ConsoleTopologyReadback::confirmed(fresh, outputs, frame));
    }
};

QTEST_GUILESS_MAIN(ConsoleTopologyReadbackTest)
#include "ConsoleTopologyReadbackTest.moc"
