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

    void latchStopsIt()
    {
        // A takeover by the tray action or the shortcut: nothing left to
        // detect, and the output churn that follows a restore must not read
        // as a second one.
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 3000);
        QVERIFY(!detector.fired());
        detector.latch();
        QVERIFY(detector.fired());
        QVERIFY(!detector.observed(QPoint(800, 800), 3400));
        // Nor does a later injection revive it.
        detector.injected(QPoint(100, 100), 5000);
        QVERIFY(!detector.observed(QPoint(800, 800), 5400));
    }

    void suspendIsTemporary()
    {
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 3000);
        detector.suspend(6000);
        // Would fire, but a park/restore is in progress.
        QVERIFY(!detector.observed(QPoint(800, 800), 3400));
        QVERIFY(!detector.observed(QPoint(800, 800), 5999));
        // Over at exactly the deadline.
        QVERIFY(detector.observed(QPoint(800, 800), 6000));
    }

    void suspendDoesNotLatch()
    {
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 3000);
        detector.suspend(6000);
        QVERIFY(!detector.observed(QPoint(800, 800), 3400));
        QVERIFY(!detector.fired());
    }

    void twoFarSamplesFireWithoutAnyInjection()
    {
        // The console mouse moves while the remote side has not touched its
        // pointer since the replace (keyboard work): two samples far apart
        // with no injection anywhere near them can only be local motion.
        Detector detector;
        detector.armed(0);
        QVERIFY(!detector.observed(QPoint(100, 100), 3000));
        QVERIFY(detector.observed(QPoint(800, 800), 3400));
        QVERIFY(detector.fired());
    }

    void oneSampleIsNotMotion()
    {
        Detector detector;
        detector.armed(0);
        QVERIFY(!detector.observed(QPoint(800, 800), 5000));
        QVERIFY(!detector.fired());
    }

    void nearSamplesDoNotFire()
    {
        Detector detector;
        detector.armed(0);
        QVERIFY(!detector.observed(QPoint(100, 100), 3000));
        QVERIFY(!detector.observed(QPoint(100 + DistanceThresholdPx, 100), 3400));
        QVERIFY(!detector.observed(QPoint(100 + DistanceThresholdPx, 100 + DistanceThresholdPx), 3800));
    }

    void injectionBetweenSamplesSuppresses()
    {
        // The displacement is ours: we moved the pointer between the samples.
        Detector detector;
        detector.armed(0);
        QVERIFY(!detector.observed(QPoint(100, 100), 3000));
        detector.injected(QPoint(800, 800), 3100);
        QVERIFY(!detector.observed(QPoint(800, 800), 3500));
        QVERIFY(!detector.fired());
    }

    void staleSampleRightAfterInjectionDoesNotFire()
    {
        // The frame captured just after our move still shows the old place;
        // the next one shows the new place. Both are ours.
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(800, 800), 3000);
        QVERIFY(!detector.observed(QPoint(100, 100), 3050));
        QVERIFY(!detector.observed(QPoint(800, 800), 3400));
        QVERIFY(!detector.fired());
    }

    void forgetInjectedKeepsTheSampleRule()
    {
        // The output moved (geometry update) after an injection: the injected
        // reference is dropped, but two samples far apart still count.
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(100, 100), 2500);
        detector.forgetInjected();
        QVERIFY(!detector.observed(QPoint(100, 100), 3000));
        QVERIFY(detector.observed(QPoint(800, 800), 3400));
    }

    void samplesInsideArmDelayOrSuspendAreNotReferences()
    {
        // A warp sample during the arm delay must not become the "previous
        // sample" a later, legitimate sample is compared against.
        Detector detector;
        detector.armed(0);
        QVERIFY(!detector.observed(QPoint(100, 100), 1000));
        QVERIFY(!detector.observed(QPoint(800, 800), 2000));
        detector.suspend(5000);
        QVERIFY(!detector.observed(QPoint(100, 100), 4000));
        QVERIFY(!detector.observed(QPoint(800, 800), 5000));
        QVERIFY(!detector.fired());
        QVERIFY(detector.observed(QPoint(100, 100), 5400));
    }

    void outputMoveSuspendsAndDropsBothReferences()
    {
        // The compositor moved the virtual output (a park, a replayed
        // arrangement): the injected reference and the previous sample were
        // both mapped through the old origin, and the churn that follows
        // warps the pointer, so nothing is judged for a while and nothing
        // from before the move is a reference afterwards.
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(5220, 100), 2500);
        QVERIFY(!detector.observed(QPoint(5220, 100), 3000));
        detector.outputMoved(3100);
        // Would fire by both rules (far from the injection and from the
        // previous sample), but the move is in progress.
        QVERIFY(!detector.observed(QPoint(100, 100), 3400));
        QVERIFY(!detector.observed(QPoint(100, 100), 3100 + OutputMoveSuspendMs - 1));
        QVERIFY(!detector.fired());
        // After the window: the pre-move sample is not a reference, so the
        // first sample only becomes one, and motion from there fires.
        QVERIFY(!detector.observed(QPoint(100, 100), 3100 + OutputMoveSuspendMs));
        QVERIFY(detector.observed(QPoint(800, 800), 3100 + OutputMoveSuspendMs + 400));
    }

    void forgetInjectedNeedsANewReference()
    {
        // The output moved after an injection was recorded against its old
        // origin: that reference is wrong by the output's displacement, so
        // it is dropped and the next injection becomes the reference.
        Detector detector;
        detector.armed(0);
        detector.injected(QPoint(5220, 100), 3000);
        detector.forgetInjected();
        QVERIFY(!detector.observed(QPoint(100, 100), 6000));
        detector.injected(QPoint(100, 100), 6100);
        QVERIFY(!detector.observed(QPoint(105, 100), 6500));
        QVERIFY(detector.observed(QPoint(800, 800), 6600));
    }
};

QTEST_GUILESS_MAIN(TakeoverDetectorTest)

#include "TakeoverDetectorTest.moc"
