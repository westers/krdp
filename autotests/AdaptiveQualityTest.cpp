#include <QTest>
#include "AdaptiveQuality.h"

using namespace KRdp::AdaptiveQuality;
using namespace std::chrono_literals;

class AdaptiveQualityTest : public QObject
{
    Q_OBJECT
private:
    static Input clear(int current, int cap = 80)
    {
        return {.current = current, .cap = cap, .averageRtt = 10ms, .minimumRtt = 10ms, .backlogged = false, .climbAllowed = true};
    }

private Q_SLOTS:
    void audioPriorityPreservesClearLinkCap()
    {
        auto in = clear(80);
        in.preferAudioQuality = true;
        QCOMPARE(step(in).next, 80);
        QVERIFY(!step(in).congested);
        in.current = 60;
        QCOMPARE(step(in).next, 62);
        in.climbAllowed = false;
        QCOMPARE(step(in).next, 60);
    }

    void audioPriorityPressureAndLiveDisable()
    {
        auto in = clear(80);
        in.preferAudioQuality = true;
        in.averageRtt = 20ms;
        QCOMPARE(step(in).next, 60);
        QVERIFY(step(in).congested);
        in.preferAudioQuality = false;
        QCOMPARE(step(in).next, 70);
        in.preferAudioQuality = true;
        in.averageRtt = in.minimumRtt;
        in.backlogged = true;
        QCOMPARE(step(in).next, 60);
        in.current = 15;
        QCOMPARE(step(in).next, MinQuality);
    }

    void audioPriorityShedsChromaAndQualityTogether()
    {
        auto in = clear(80);
        in.preferAudioQuality = true;
        in.chromaAvailable = true;
        in.backlogged = true;
        const auto result = step(in);
        QVERIFY(!result.chromaEnabled);
        QCOMPARE(result.next, 60);
        in.preferAudioQuality = false;
        QCOMPARE(step(in).next, 80);
        QVERIFY(!step(in).chromaEnabled);
    }

    void clearIntervalClimbsByStepUp()
    {
        const auto r = step(clear(60));
        QCOMPARE(r.next, 65);
        QVERIFY(!r.congested);
    }

    void neverExceedsCap()
    {
        QCOMPARE(step(clear(78)).next, 80);
        QCOMPARE(step(clear(80)).next, 80);
    }

    void capBelowCurrentClampsDown()
    {
        QCOMPARE(step(clear(80, 50)).next, 50);
    }

    void climbHoldKeepsQuality()
    {
        auto in = clear(60);
        in.climbAllowed = false;
        QCOMPARE(step(in).next, 60);
    }

    void backlogStepsDownEvenDuringHold()
    {
        auto in = clear(80);
        in.backlogged = true;
        QCOMPARE(step(in).next, 70);
        in.climbAllowed = false;
        QCOMPARE(step(in).next, 70);
        QVERIFY(!step(in).congested);
    }

    void startupKeyframeBurstIsNotBacklogPressure()
    {
        constexpr int burstDepth = BacklogFrames + 2;
        QVERIFY(!backlogIsPressure(19s, burstDepth, burstDepth));
        QVERIFY(!backlogIsPressure(19s, std::numeric_limits<int>::max(), burstDepth));
        QVERIFY(backlogIsPressure(BacklogWarmupAfterStreamStart, burstDepth, burstDepth));
        QVERIFY(backlogIsPressure(21s, std::numeric_limits<int>::max(), burstDepth));
    }

    void rttCongestionStepsDown()
    {
        auto in = clear(60);
        in.averageRtt = 20ms;
        in.minimumRtt = 10ms;
        const auto r = step(in);
        QVERIFY(r.congested);
        QCOMPARE(r.next, 50);
    }

    void smallJitterIsNotCongestion()
    {
        auto in = clear(60);
        in.averageRtt = 3000us; // 2 ms above the minimum: below the 5 ms margin
        in.minimumRtt = 1000us;
        QVERIFY(!step(in).congested);
        QCOMPARE(step(in).next, 65);

        in.averageRtt = 106ms; // 6 ms above the minimum but only 1.06x it
        in.minimumRtt = 100ms;
        QVERIFY(!step(in).congested);
        QCOMPARE(step(in).next, 65);
    }

    void noRttBaselineIsNotCongestion()
    {
        auto in = clear(60);
        in.averageRtt = 50ms;
        in.minimumRtt = 0us;
        QVERIFY(!step(in).congested);
        QCOMPARE(step(in).next, 65);
    }

    void neverDropsBelowMinimum()
    {
        auto in = clear(12);
        in.backlogged = true;
        QCOMPARE(step(in).next, MinQuality);
        in.current = MinQuality;
        QCOMPARE(step(in).next, MinQuality);
    }

    static Input chroma(int current, bool enabled, int cap = 80)
    {
        auto in = clear(current, cap);
        in.chromaAvailable = true;
        in.chromaEnabled = enabled;
        return in;
    }
    void withoutChromaTheRungDoesNotExist()
    {
        auto in = clear(80);
        in.backlogged = true;
        const auto r = step(in);
        QCOMPARE(r.next, 70);
        QVERIFY(r.chromaEnabled); // reported as-is, never flipped
    }
    void firstStepDownShedsChromaAndKeepsQp()
    {
        auto in = chroma(80, true);
        in.backlogged = true;
        const auto r = step(in);
        QCOMPARE(r.next, 80);
        QVERIFY(!r.chromaEnabled);
    }
    void furtherStepDownsLowerQp()
    {
        auto in = chroma(80, false);
        in.backlogged = true;
        QCOMPARE(step(in).next, 70);
        QVERIFY(!step(in).chromaEnabled);
    }
    void climbRaisesQpFirstThenChromaLast()
    {
        auto in = chroma(70, false);
        auto r = step(in);
        QCOMPARE(r.next, 75);
        QVERIFY(!r.chromaEnabled);
        in.current = 75;
        r = step(in);
        QCOMPARE(r.next, 80);
        QVERIFY(!r.chromaEnabled);
        in.current = 80;
        r = step(in); // at the cap: chroma comes back
        QCOMPARE(r.next, 80);
        QVERIFY(r.chromaEnabled);
        in.chromaEnabled = true;
        r = step(in); // top state: nothing to do
        QCOMPARE(r.next, 80);
        QVERIFY(r.chromaEnabled);
    }
    void climbHoldBlocksChromaToo()
    {
        auto in = chroma(80, false);
        in.climbAllowed = false;
        const auto r = step(in);
        QCOMPARE(r.next, 80);
        QVERIFY(!r.chromaEnabled);
    }
    void shedBelowCapWhenChromaStillOn()
    {
        // The cap moved down while chroma was on: pressure still sheds chroma before touching QP.
        auto in = chroma(60, true);
        in.backlogged = true;
        const auto r = step(in);
        QCOMPARE(r.next, 60);
        QVERIFY(!r.chromaEnabled);
    }
    void capClampStillApplies()
    {
        QCOMPARE(step(chroma(80, false, 50)).next, 50);
        auto in = chroma(12, false);
        in.backlogged = true;
        QCOMPARE(step(in).next, MinQuality);
    }
};

QTEST_GUILESS_MAIN(AdaptiveQualityTest)
#include "AdaptiveQualityTest.moc"
