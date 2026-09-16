// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "SurfaceLayout.h"

using namespace KRdp;
using KRdp::SurfaceLayout::Entry;
using KRdp::SurfaceLayout::fromMonitors;

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

    // RDP desktop space puts the primary's top-left at (0, 0), so a primary
    // that KWin placed to the right of another output pushes that output to a
    // negative x. mstsc accepts negative coordinates as long as the primary
    // contains (0, 0).
    void primaryNotAtOriginTranslates()
    {
        const QVector<VideoMonitor> monitors{
            {.geometry = QRect(0, 0, 1920, 1080), .primary = false},
            {.geometry = QRect(1920, 0, 2560, 1440), .primary = true},
        };

        const auto entries = fromMonitors(monitors);

        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).size, QSize(1920, 1080));
        QCOMPARE(entries.at(0).origin, QPoint(-1920, 0));
        QVERIFY(!entries.at(0).primary);
        QCOMPARE(entries.at(1).size, QSize(2560, 1440));
        QCOMPARE(entries.at(1).origin, QPoint(0, 0));
        QVERIFY(entries.at(1).primary);
    }

    void emptyLayoutHasNoSurfaces()
    {
        QVERIFY(fromMonitors({}).isEmpty());
    }

    // VideoStream rejects a layout without exactly one primary, but the helper
    // stays total: it anchors on the first monitor and marks that one primary,
    // so the result always has exactly one primary and it contains (0, 0).
    void layoutWithoutPrimaryAnchorsOnFirst()
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
