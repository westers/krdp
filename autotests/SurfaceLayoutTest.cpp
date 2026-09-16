// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "SurfaceLayout.h"

using namespace KRdp;
using KRdp::SurfaceLayout::Entry;
using KRdp::SurfaceLayout::fromMonitors;
using KRdp::SurfaceLayout::originOf;

class SurfaceLayoutTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // The single-monitor case every existing mode uses: nothing to translate.
    void singleMonitorAtOrigin()
    {
        const QVector<VideoMonitor> monitors{
            {.geometry = QRect(0, 0, 2560, 1440), .primary = true},
        };

        const auto entries = fromMonitors(monitors);

        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.at(0).size, QSize(2560, 1440));
        QCOMPARE(entries.at(0).origin, QPoint(0, 0));
        QVERIFY(entries.at(0).primary);
    }

    // hal9000's desktop: DP-1 primary at the origin, HDMI-A-1 to its right.
    void twoMonitorsSideBySide()
    {
        const QVector<VideoMonitor> monitors{
            {.geometry = QRect(0, 0, 2560, 1440), .primary = true},
            {.geometry = QRect(2560, 0, 2560, 1440), .primary = false},
        };

        const auto entries = fromMonitors(monitors);

        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).size, QSize(2560, 1440));
        QCOMPARE(entries.at(0).origin, QPoint(0, 0));
        QVERIFY(entries.at(0).primary);
        QCOMPARE(entries.at(1).size, QSize(2560, 1440));
        QCOMPARE(entries.at(1).origin, QPoint(2560, 0));
        QVERIFY(!entries.at(1).primary);
    }

    // The anchor is the union's top-left, not the primary's, because
    // MapSurfaceToOutput's origins are unsigned on the wire. A primary placed
    // to the right of another output therefore keeps its own offset instead of
    // pushing that output negative.
    void primaryAwayFromOriginKeepsItsOffset()
    {
        const QVector<VideoMonitor> monitors{
            {.geometry = QRect(0, 0, 1920, 1080), .primary = false},
            {.geometry = QRect(1920, 0, 2560, 1440), .primary = true},
        };

        const auto entries = fromMonitors(monitors);

        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).size, QSize(1920, 1080));
        QCOMPARE(entries.at(0).origin, QPoint(0, 0));
        QVERIFY(!entries.at(0).primary);
        QCOMPARE(entries.at(1).size, QSize(2560, 1440));
        QCOMPARE(entries.at(1).origin, QPoint(1920, 0));
        QVERIFY(entries.at(1).primary);

        for (const auto &entry : entries) {
            QVERIFY(entry.origin.x() >= 0);
            QVERIFY(entry.origin.y() >= 0);
        }
    }

    // The same, vertically: KWin puts an output above the primary at a
    // negative y, and the whole desktop shifts down instead.
    void monitorAbovePrimaryShiftsTheDesktopDown()
    {
        const QVector<VideoMonitor> monitors{
            {.geometry = QRect(0, -1080, 1920, 1080), .primary = false},
            {.geometry = QRect(0, 0, 2560, 1440), .primary = true},
        };

        const auto entries = fromMonitors(monitors);

        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).size, QSize(1920, 1080));
        QCOMPARE(entries.at(0).origin, QPoint(0, 0));
        QVERIFY(!entries.at(0).primary);
        QCOMPARE(entries.at(1).size, QSize(2560, 1440));
        QCOMPARE(entries.at(1).origin, QPoint(0, 1080));
        QVERIFY(entries.at(1).primary);
    }

    // originOf() is the offset fromMonitors() removed, so adding it back to an
    // entry's origin returns the monitor's own top-left. The input path in
    // MonitorMode=multi needs exactly that to map a client pointer position
    // back into KWin coordinates.
    void unionOriginInvertsTheTranslation()
    {
        const QVector<VideoMonitor> monitors{
            {.geometry = QRect(0, -1080, 1920, 1080), .primary = false},
            {.geometry = QRect(2560, 0, 2560, 1440), .primary = true},
        };

        const auto anchor = originOf(monitors);
        QCOMPARE(anchor, QPoint(0, -1080));

        const auto entries = fromMonitors(monitors);
        QCOMPARE(entries.size(), monitors.size());
        for (qsizetype i = 0; i < entries.size(); ++i) {
            QCOMPARE(entries.at(i).origin + anchor, monitors.at(i).geometry.topLeft());
        }
    }

    void unionOriginOfEmptyLayoutIsZero()
    {
        QCOMPARE(originOf({}), QPoint(0, 0));
    }

    void emptyLayoutHasNoSurfaces()
    {
        QVERIFY(fromMonitors({}).isEmpty());
    }

    // VideoStream rejects a layout without exactly one primary, but the helper
    // stays total: it marks the first monitor primary, so the result always
    // has exactly one.
    void layoutWithoutPrimaryMarksTheFirstOne()
    {
        const QVector<VideoMonitor> monitors{
            {.geometry = QRect(100, 200, 640, 480), .primary = false},
            {.geometry = QRect(740, 200, 640, 480), .primary = false},
        };

        const auto entries = fromMonitors(monitors);

        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).origin, QPoint(0, 0));
        QVERIFY(entries.at(0).primary);
        QCOMPARE(entries.at(1).origin, QPoint(640, 0));
        QVERIFY(!entries.at(1).primary);
    }

    // Only the first monitor flagged primary stays primary, matching the
    // normalisation the frame-derived layout already does.
    void extraPrimariesAreDropped()
    {
        const QVector<VideoMonitor> monitors{
            {.geometry = QRect(0, 0, 800, 600), .primary = true},
            {.geometry = QRect(800, 0, 800, 600), .primary = true},
        };

        const auto entries = fromMonitors(monitors);

        QCOMPARE(entries.size(), 2);
        QVERIFY(entries.at(0).primary);
        QVERIFY(!entries.at(1).primary);
    }
};

QTEST_GUILESS_MAIN(SurfaceLayoutTest)
#include "SurfaceLayoutTest.moc"
