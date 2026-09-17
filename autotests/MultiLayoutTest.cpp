// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "MultiLayout.h"

using namespace Qt::StringLiterals;

using KRdp::VideoMonitor;
using KRdp::MultiLayout::MaxMonitorCount;
using KRdp::MultiLayout::dropMonitor;
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
            screen(u"DP-1"_s, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"HDMI-A-1"_s, QRect(2560, 0, 2560, 1440)),
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
            screen(u"DP-1"_s, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"DP-2"_s, QRect()),
            screen(u"HDMI-A-1"_s, QRect(2560, 0, 2560, 1440)),
        };

        QStringList dropped;
        QList<qsizetype> kept;
        const auto layout = selectMultiLayout(screens, &dropped, &kept);

        QCOMPARE(layout.size(), 2);
        QCOMPARE(layout.at(0).geometry, QRect(0, 0, 2560, 1440));
        QCOMPARE(layout.at(1).geometry, QRect(2560, 0, 2560, 1440));
        QCOMPARE(dropped, QStringList({u"DP-2"_s}));
        // The surviving entries still point at screens 0 and 2, not 0 and 1.
        QCOMPARE(kept, QList<qsizetype>({0, 2}));
    }

    // No encoder on this box can make a surface wider than 4096 px.
    void oversizedScreenIsDropped()
    {
        const QVector<ScreenInfo> screens{
            screen(u"DP-1"_s, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"HDMI-A-1"_s, QRect(2560, 0, 2560, 1440)),
            screen(u"DP-3"_s, QRect(5120, 0, 5120, 1440)),
        };

        QStringList dropped;
        QList<qsizetype> kept;
        const auto layout = selectMultiLayout(screens, &dropped, &kept);

        QCOMPARE(layout.size(), 2);
        QCOMPARE(dropped, QStringList({u"DP-3"_s}));
        QCOMPARE(kept, QList<qsizetype>({0, 1}));
    }

    // A screen only just over the limit in one direction still goes.
    void oversizedHeightIsDropped()
    {
        const QVector<ScreenInfo> screens{
            screen(u"DP-1"_s, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"DP-2"_s, QRect(2560, 0, 1000, 4097)),
            screen(u"DP-3"_s, QRect(3560, 0, 1000, 4096)),
        };

        QStringList dropped;
        const auto layout = selectMultiLayout(screens, &dropped);

        QCOMPARE(layout.size(), 2);
        QCOMPARE(dropped, QStringList({u"DP-2"_s}));
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
        QCOMPARE(dropped, QStringList({u"DP-16"_s}));
        QCOMPARE(primaryCount(layout), 1);
    }

    // setMonitorLayout() rejects a layout that does not have exactly one
    // primary, so the selection always produces exactly one.
    void exactlyOnePrimary()
    {
        const QVector<ScreenInfo> none{
            screen(u"DP-1"_s, QRect(0, 0, 2560, 1440)),
            screen(u"HDMI-A-1"_s, QRect(2560, 0, 2560, 1440)),
        };
        const auto promoted = selectMultiLayout(none);
        QCOMPARE(promoted.size(), 2);
        QCOMPARE(primaryCount(promoted), 1);
        QVERIFY(promoted.at(0).primary);

        const QVector<ScreenInfo> both{
            screen(u"DP-1"_s, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"HDMI-A-1"_s, QRect(2560, 0, 2560, 1440), 1.0, true),
        };
        const auto firstWins = selectMultiLayout(both);
        QCOMPARE(firstWins.size(), 2);
        QCOMPARE(primaryCount(firstWins), 1);
        QVERIFY(firstWins.at(0).primary);
        QVERIFY(!firstWins.at(1).primary);

        // The configured primary was dropped for being too large, so the first
        // surviving monitor takes the flag.
        const QVector<ScreenInfo> primaryDropped{
            screen(u"DP-1"_s, QRect(0, 0, 5120, 1440), 1.0, true),
            screen(u"HDMI-A-1"_s, QRect(5120, 0, 2560, 1440)),
            screen(u"DP-2"_s, QRect(7680, 0, 2560, 1440)),
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
            screen(u"DP-1"_s, QRect(0, 0, 1920, 1080), 2.0, true),
            screen(u"HDMI-A-1"_s, QRect(1920, 0, 1280, 720), 2.0),
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
            screen(u"DP-1"_s, QRect(0, 0, 1920, 1080), 2.0, true),
            screen(u"HDMI-A-1"_s, QRect(1920, 0, 2560, 1440), 2.0),
            screen(u"DP-2"_s, QRect(4480, 0, 1920, 1080), 2.0),
        };

        QStringList dropped;
        const auto layout = selectMultiLayout(screens, &dropped);

        QCOMPARE(layout.size(), 2);
        QCOMPARE(dropped, QStringList({u"HDMI-A-1"_s}));
    }

    // Fewer than the minimum means "multi is not usable"; the caller then falls
    // back to a single session.
    void tooFewUsableMonitorsYieldsNothing()
    {
        const QVector<ScreenInfo> one{
            screen(u"DP-1"_s, QRect(0, 0, 2560, 1440), 1.0, true),
        };
        QVERIFY(selectMultiLayout(one).isEmpty());

        const QVector<ScreenInfo> oneUsable{
            screen(u"DP-1"_s, QRect(0, 0, 2560, 1440), 1.0, true),
            screen(u"HDMI-A-1"_s, QRect(2560, 0, 5120, 1440)),
        };
        QStringList dropped;
        QVERIFY(selectMultiLayout(oneUsable, &dropped).isEmpty());
        // Still reported, so the caller can say why multi was refused.
        QCOMPARE(dropped, QStringList({u"HDMI-A-1"_s}));

        QVERIFY(selectMultiLayout({}).isEmpty());

        // A session already running in multi mode keeps its single surface
        // rather than switching a live client to the other code path.
        const auto single = selectMultiLayout(one, nullptr, nullptr, 1);
        QCOMPARE(single.size(), 1);
        QVERIFY(single.at(0).primary);
    }

    // A fractional scale makes the derived pixel size disagree with the size
    // the compositor actually captures: 1707 logical at 1.5 rounds to 2561,
    // while the capture is 2560, and VideoStream drops every frame of that
    // monitor for not matching its surface. The selection cannot know better -
    // it only has the logical geometry and the ratio - so SessionWrapper::
    // correctSurfaceSize() patches the entry from the started session's own
    // pixelSize(). This pins the rounding that makes that path necessary.
    void fractionalScaleRoundsThePixelSize()
    {
        const QVector<ScreenInfo> screens{
            screen(u"DP-1"_s, QRect(0, 0, 1707, 960), 1.5, true),
            screen(u"HDMI-A-1"_s, QRect(1707, 0, 1707, 960), 1.5),
        };

        const auto layout = selectMultiLayout(screens);

        QCOMPARE(layout.size(), 2);
        // 1707 * 1.5 = 2560.5, rounded up; the real capture is 2560.
        QCOMPARE(layout.at(0).geometry.width(), 2561);
        QCOMPARE(layout.at(0).geometry.height(), 1440);
        // ... and the second monitor's origin is rounded the same way, so the
        // correction must keep the origin and replace only the size.
        QCOMPARE(layout.at(1).geometry.topLeft(), QPoint(2561, 0));
    }

    // dropSession() re-stamps the survivors' surface indices: they have to stay
    // 0..N-1 and line up with the layout handed to setMonitorLayout().
    void droppingAMonitorReindexesTheSurvivors()
    {
        const auto outcome = dropMonitor(3, 0, 0);

        // The sessions that were at 1 and 2 now feed surfaces 0 and 1.
        QCOMPARE(outcome.survivors, QList<qsizetype>({1, 2}));
        // The primary was the monitor that went away, so the first survivor
        // takes the flag; setMonitorLayout() rejects a layout without one.
        QCOMPARE(outcome.primary, qsizetype(0));
    }

    // Dropping a non-primary leaves the primary where it is, at its new index.
    void droppingANonPrimaryMovesThePrimaryDown()
    {
        const auto first = dropMonitor(3, 0, 2);
        QCOMPARE(first.survivors, QList<qsizetype>({1, 2}));
        QCOMPARE(first.primary, qsizetype(1));

        const auto last = dropMonitor(3, 2, 1);
        QCOMPARE(last.survivors, QList<qsizetype>({0, 1}));
        QCOMPARE(last.primary, qsizetype(1));

        const auto middle = dropMonitor(3, 1, 0);
        QCOMPARE(middle.survivors, QList<qsizetype>({0, 2}));
        QCOMPARE(middle.primary, qsizetype(0));
    }

    // No primary at all (or an index outside the set) still yields exactly one.
    void dropWithoutAPrimaryPromotesTheFirstSurvivor()
    {
        const auto outcome = dropMonitor(3, 1, -1);
        QCOMPARE(outcome.survivors, QList<qsizetype>({0, 2}));
        QCOMPARE(outcome.primary, qsizetype(0));
    }

    // The last monitor failing leaves nothing to promote; dropSession() closes
    // the connection on that, like every other mode does for its one session.
    void droppingTheLastMonitorLeavesNoSurvivors()
    {
        const auto outcome = dropMonitor(1, 0, 0);
        QVERIFY(outcome.survivors.isEmpty());
        QCOMPARE(outcome.primary, qsizetype(-1));

        const auto none = dropMonitor(0, 0, -1);
        QVERIFY(none.survivors.isEmpty());
        QCOMPARE(none.primary, qsizetype(-1));
    }

    // The layout is handed to VideoStream untranslated; SurfaceLayout owns the
    // move into RDP desktop space and inverts it for input.
    void geometriesAreNotTranslated()
    {
        const QVector<ScreenInfo> screens{
            screen(u"HDMI-A-1"_s, QRect(-2560, 0, 2560, 1440)),
            screen(u"DP-1"_s, QRect(0, 0, 2560, 1440), 1.0, true),
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
