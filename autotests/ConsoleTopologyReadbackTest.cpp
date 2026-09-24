// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QFile>
#include <QTest>

#include "ConsoleTopologyReadback.h"

class ConsoleTopologyReadbackTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
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
};

QTEST_GUILESS_MAIN(ConsoleTopologyReadbackTest)
#include "ConsoleTopologyReadbackTest.moc"
