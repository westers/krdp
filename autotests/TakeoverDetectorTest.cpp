// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "TakeoverDetector.h"

using namespace KRdp::Takeover;

/**
 * The console-takeover rule (OPT-041 Task 6c) as a pure function of time
 * stamps and pointer positions: a cursor sample counts as local motion only
 * once the detector is armed, after at least one injected move, outside the
 * quiet window after the last injection, and further from the last injected
 * position than any injected move could explain.
 */
class TakeoverDetectorTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void notBeforeArmDelay()
    {
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 100);
        QVERIFY(!detector.observed(QPoint(900, 900), 500));
    }

    void injectedMotionIsNotLocal()
    {
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 3000);
        // The sample that reflects our own injection, a few pixels off.
        QVERIFY(!detector.observed(QPoint(110, 105), 3050));
    }

    void quietWindowSuppresses()
    {
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 3000);
        // Far away, but too soon after an injection to be sure it is not ours.
        QVERIFY(!detector.observed(QPoint(800, 800), 3100));
    }

    void localMotionFires()
    {
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 3000);
        QVERIFY(detector.observed(QPoint(800, 800), 3400));
    }

    void firesOnce()
    {
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 3000);
        QVERIFY(detector.observed(QPoint(800, 800), 3400));
        QVERIFY(!detector.observed(QPoint(1500, 200), 3800));
    }

    void needsOneInjection()
    {
        Detector detector;
        detector.armed(0);
        QVERIFY(!detector.observed(QPoint(800, 800), 5000));
    }

    void notBeforeArmed()
    {
        Detector detector;
        detector.injected(QPoint(100, 100), 3000);
        QVERIFY(!detector.observed(QPoint(800, 800), 3400));
    }

    void thresholdIsExclusive()
    {
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 3000);
        // Exactly DistanceThresholdPx away (Manhattan) is still "ours".
        QVERIFY(!detector.observed(QPoint(100 + DistanceThresholdPx, 100), 3400));
        QVERIFY(detector.observed(QPoint(100 + DistanceThresholdPx + 1, 100), 3401));
    }

    void quietWindowIsExclusive()
    {
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 3000);
        QVERIFY(!detector.observed(QPoint(800, 800), 3000 + QuietWindowMs));
        QVERIFY(detector.observed(QPoint(800, 800), 3000 + QuietWindowMs + 1));
    }

    void armDelayIsInclusive()
    {
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 100);
        QVERIFY(!detector.observed(QPoint(800, 800), ArmDelayMs - 1));
        QVERIFY(detector.observed(QPoint(800, 800), ArmDelayMs));
    }

    void latestInjectionCounts()
    {
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 3000);
        detector.injected(QPoint(800, 800), 3200);
        // Near the latest injection: ours.
        QVERIFY(!detector.observed(QPoint(805, 802), 3600));
        // Near the earlier one: the pointer has left where we last put it.
        QVERIFY(detector.observed(QPoint(100, 100), 3700));
    }
};

QTEST_GUILESS_MAIN(TakeoverDetectorTest)

#include "TakeoverDetectorTest.moc"
