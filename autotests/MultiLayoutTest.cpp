// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "MultiLayout.h"

using KRdp::VideoMonitor;
using KRdp::MultiLayout::MaxMonitorCount;
using KRdp::MultiLayout::ScreenInfo;
using KRdp::MultiLayout::selectMultiLayout;

namespace
{
ScreenInfo screen(const QString &name, const QRect &geometry, qreal dpr = 1.0, bool primary = false)
{
    return ScreenInfo{
        .name = name,
        .logicalGeometry = geometry,
        .devicePixelRatio = dpr,
        .primary = primary,
    };
}

qsizetype primaryCount(const QVector<VideoMonitor> &layout)
{
    return std::count_if(layout.cbegin(), layout.cend(), [](const VideoMonitor &monitor) {
        return monitor.primary;
    });
}
}

class MultiLayoutTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // This box: DP-1 at the origin as primary, HDMI-A-1 to its right, both at
    // scale 1, so the pixel rects are the logical ones unchanged.
    void twoScaleOneScreens()
    {
        const QVector<ScreenInfo> screens{
            screen(u"DP-1"_qs, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"HDMI-A-1"_qs, QRect(2560, 0, 2560, 1440)),
        };

        QStringList dropped;
        QList<qsizetype> kept;
        const auto layout = selectMultiLayout(screens, &dropped, &kept);

        QCOMPARE(layout.size(), 2);
        QCOMPARE(layout.at(0).geometry, QRect(0, 0, 2560, 1440));
        QVERIFY(layout.at(0).primary);
        QCOMPARE(layout.at(1).geometry, QRect(2560, 0, 2560, 1440));
        QVERIFY(!layout.at(1).primary);
        QVERIFY(dropped.isEmpty());
        QCOMPARE(kept, QList<qsizetype>({0, 1}));
    }

    // KWin reports a null geometry while it re-adds its outputs after a DPMS
    // wake; that screen is left out instead of poisoning the whole layout.
    void emptyGeometryIsSkipped()
    {
        const QVector<ScreenInfo> screens{
            screen(u"DP-1"_qs, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"DP-2"_qs, QRect()),
            screen(u"HDMI-A-1"_qs, QRect(2560, 0, 2560, 1440)),
        };

        QStringList dropped;
        QList<qsizetype> kept;
        const auto layout = selectMultiLayout(screens, &dropped, &kept);

        QCOMPARE(layout.size(), 2);
        QCOMPARE(layout.at(0).geometry, QRect(0, 0, 2560, 1440));
        QCOMPARE(layout.at(1).geometry, QRect(2560, 0, 2560, 1440));
        QCOMPARE(dropped, QStringList({u"DP-2"_qs}));
        // The surviving entries still point at screens 0 and 2, not 0 and 1.
        QCOMPARE(kept, QList<qsizetype>({0, 2}));
    }

    // No encoder on this box can make a surface wider than 4096 px.
    void oversizedScreenIsDropped()
    {
        const QVector<ScreenInfo> screens{
            screen(u"DP-1"_qs, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"HDMI-A-1"_qs, QRect(2560, 0, 2560, 1440)),
            screen(u"DP-3"_qs, QRect(5120, 0, 5120, 1440)),
        };

        QStringList dropped;
        QList<qsizetype> kept;
        const auto layout = selectMultiLayout(screens, &dropped, &kept);

        QCOMPARE(layout.size(), 2);
        QCOMPARE(dropped, QStringList({u"DP-3"_qs}));
        QCOMPARE(kept, QList<qsizetype>({0, 1}));
    }

    // A screen only just over the limit in one direction still goes.
    void oversizedHeightIsDropped()
    {
        const QVector<ScreenInfo> screens{
            screen(u"DP-1"_qs, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"DP-2"_qs, QRect(2560, 0, 1000, 4097)),
            screen(u"DP-3"_qs, QRect(3560, 0, 1000, 4096)),
        };

        QStringList dropped;
        const auto layout = selectMultiLayout(screens, &dropped);

        QCOMPARE(layout.size(), 2);
        QCOMPARE(dropped, QStringList({u"DP-2"_qs}));
        QCOMPARE(layout.at(1).geometry, QRect(3560, 0, 1000, 4096));
    }

    // RDPGFX_RESET_GRAPHICS carries at most 16 monitor definitions.
    void atMostSixteenMonitors()
    {
        QVector<ScreenInfo> screens;
        for (int i = 0; i < 17; ++i) {
            screens.push_back(screen(QStringLiteral("DP-%1").arg(i), QRect(i * 1920, 0, 1920, 1080), 1.0, i == 0));
        }

        QStringList dropped;
        QList<qsizetype> kept;
        const auto layout = selectMultiLayout(screens, &dropped, &kept);

        QCOMPARE(layout.size(), MaxMonitorCount);
        QCOMPARE(kept.size(), MaxMonitorCount);
        QCOMPARE(dropped, QStringList({u"DP-16"_qs}));
        QCOMPARE(primaryCount(layout), 1);
    }

    // setMonitorLayout() rejects a layout that does not have exactly one
    // primary, so the selection always produces exactly one.
    void exactlyOnePrimary()
    {
        const QVector<ScreenInfo> none{
            screen(u"DP-1"_qs, QRect(0, 0, 2560, 1440)),
            screen(u"HDMI-A-1"_qs, QRect(2560, 0, 2560, 1440)),
        };
        const auto promoted = selectMultiLayout(none);
        QCOMPARE(promoted.size(), 2);
        QCOMPARE(primaryCount(promoted), 1);
        QVERIFY(promoted.at(0).primary);

        const QVector<ScreenInfo> both{
            screen(u"DP-1"_qs, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"HDMI-A-1"_qs, QRect(2560, 0, 2560, 1440), 1.0, true),
        };
        const auto firstWins = selectMultiLayout(both);
        QCOMPARE(firstWins.size(), 2);
        QCOMPARE(primaryCount(firstWins), 1);
        QVERIFY(firstWins.at(0).primary);
        QVERIFY(!firstWins.at(1).primary);

        // The configured primary was dropped for being too large, so the first
        // surviving monitor takes the flag.
        const QVector<ScreenInfo> primaryDropped{
            screen(u"DP-1"_qs, QRect(0, 0, 5120, 1440), 1.0, true),
            screen(u"HDMI-A-1"_qs, QRect(5120, 0, 2560, 1440)),
            screen(u"DP-2"_qs, QRect(7680, 0, 2560, 1440)),
        };
        const auto rescued = selectMultiLayout(primaryDropped);
        QCOMPARE(rescued.size(), 2);
        QCOMPARE(primaryCount(rescued), 1);
        QVERIFY(rescued.at(0).primary);
        QCOMPARE(rescued.at(0).geometry, QRect(5120, 0, 2560, 1440));
    }

    // The surface has to be the capture size, which is the logical geometry
    // scaled by the monitor's device pixel ratio.
    void scaledScreenIsInPixels()
    {
        const QVector<ScreenInfo> screens{
            screen(u"DP-1"_qs, QRect(0, 0, 1920, 1080), 2.0, true),
            screen(u"HDMI-A-1"_qs, QRect(1920, 0, 1280, 720), 2.0),
        };

        const auto layout = selectMultiLayout(screens);

        QCOMPARE(layout.size(), 2);
        QCOMPARE(layout.at(0).geometry, QRect(0, 0, 3840, 2160));
        QCOMPARE(layout.at(1).geometry, QRect(3840, 0, 2560, 1440));
    }

    // Scaling is applied before the encode limit, so a 2560x1440 screen at
    // scale 2 is a 5120x2880 surface and cannot be streamed.
    void scalingIsAppliedBeforeTheEncodeLimit()
    {
        const QVector<ScreenInfo> screens{
            screen(u"DP-1"_qs, QRect(0, 0, 1920, 1080), 2.0, true),
            screen(u"HDMI-A-1"_qs, QRect(1920, 0, 2560, 1440), 2.0),
            screen(u"DP-2"_qs, QRect(4480, 0, 1920, 1080), 2.0),
        };

        QStringList dropped;
        const auto layout = selectMultiLayout(screens, &dropped);

        QCOMPARE(layout.size(), 2);
        QCOMPARE(dropped, QStringList({u"HDMI-A-1"_qs}));
    }

    // Fewer than the minimum means "multi is not usable"; the caller then falls
    // back to a single session.
    void tooFewUsableMonitorsYieldsNothing()
    {
        const QVector<ScreenInfo> one{
            screen(u"DP-1"_qs, QRect(0, 0, 2560, 1440), 1.0, true),
        };
        QVERIFY(selectMultiLayout(one).isEmpty());

        const QVector<ScreenInfo> oneUsable{
            screen(u"DP-1"_qs, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"HDMI-A-1"_qs, QRect(2560, 0, 5120, 1440)),
        };
        QStringList dropped;
        QVERIFY(selectMultiLayout(oneUsable, &dropped).isEmpty());
        // Still reported, so the caller can say why multi was refused.
        QCOMPARE(dropped, QStringList({u"HDMI-A-1"_qs}));

        QVERIFY(selectMultiLayout({}).isEmpty());

        // A session already running in multi mode keeps its single surface
        // rather than switching a live client to the other code path.
        const auto single = selectMultiLayout(one, nullptr, nullptr, 1);
        QCOMPARE(single.size(), 1);
        QVERIFY(single.at(0).primary);
    }

    // The layout is handed to VideoStream untranslated; SurfaceLayout owns the
    // move into RDP desktop space and inverts it for input.
    void geometriesAreNotTranslated()
    {
        const QVector<ScreenInfo> screens{
            screen(u"HDMI-A-1"_qs, QRect(-2560, 0, 2560, 1440)),
            screen(u"DP-1"_qs, QRect(0, 0, 2560, 1440), 1.0, true),
        };

        const auto layout = selectMultiLayout(screens);

        QCOMPARE(layout.size(), 2);
        QCOMPARE(layout.at(0).geometry, QRect(-2560, 0, 2560, 1440));
        QCOMPARE(layout.at(1).geometry, QRect(0, 0, 2560, 1440));
        QCOMPARE(KRdp::SurfaceLayout::originOf(layout), QPoint(-2560, 0));

        // ... and the entries it produces are anchored at the union.
        const auto entries = KRdp::SurfaceLayout::fromMonitors(layout);
        QCOMPARE(entries.at(0).origin, QPoint(0, 0));
        QCOMPARE(entries.at(1).origin, QPoint(2560, 0));
    }
};

QTEST_GUILESS_MAIN(MultiLayoutTest)

#include "MultiLayoutTest.moc"
