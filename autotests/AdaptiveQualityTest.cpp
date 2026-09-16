#include <QTest>
#include "AdaptiveQuality.h"

using namespace KRdp::AdaptiveQuality;
using namespace std::chrono_literals;

class AdaptiveQualityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void fullQualityAnchorsScaleLinearly()
    {
        QCOMPARE(fullQualityKbit(2560.0 * 1440.0), 4500.0);
        QCOMPARE(fullQualityKbit(1920.0 * 1080.0), 3110.0);
        // Halfway between anchors: nearest anchor, scaled by pixel ratio.
        QVERIFY(fullQualityKbit(2560.0 * 1080.0) > 3110.0);
        QVERIFY(fullQualityKbit(2560.0 * 1080.0) < 4500.0);
    }
    void stepsUpSlowlyTowardsTarget()
    {
        const auto r = step({.current = 50, .cap = 100, .goodputKbit = 9000, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms});
        QCOMPARE(r.target, 100);
        QCOMPARE(r.next, 55);
        QVERIFY(!r.congested);
    }
    void stepsDownFasterTowardsTarget()
    {
        const auto r = step({.current = 80, .cap = 100, .goodputKbit = 900, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms});
        QCOMPARE(r.target, 20); // 900/4500*100
        QCOMPARE(r.next, 70);
    }
    void neverExceedsCapOrDropsBelowMinimum()
    {
        QCOMPARE(step({.current = 80, .cap = 80, .goodputKbit = 90000, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms}).next, 80);
        QCOMPARE(step({.current = 12, .cap = 100, .goodputKbit = 1, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms}).next, MinQuality);
    }
    void congestionForcesAStepDownAndBlocksStepUp()
    {
        const auto r = step({.current = 60, .cap = 100, .goodputKbit = 9000, .pixels = 2560.0 * 1440.0, .averageRtt = 60ms, .minimumRtt = 20ms});
        QVERIFY(r.congested);
        QCOMPARE(r.next, 50);
    }
    void smallJitterIsNotCongestion()
    {
        // 1 ms of RTT jitter (e.g. average 2 ms vs. minimum 1 ms) would look
        // like a 2x spike if RTTs were truncated to milliseconds; it should
        // not trip the congestion gate.
        const auto r = step({.current = 50, .cap = 100, .goodputKbit = 9000, .pixels = 2560.0 * 1440.0, .averageRtt = 2ms, .minimumRtt = 1ms});
        QVERIFY(!r.congested);
        QCOMPARE(r.next, 55);
    }
    void zeroGoodputLeavesQualityAlone()
    {
        const auto r = step({.current = 60, .cap = 100, .goodputKbit = 0, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms});
        QCOMPARE(r.next, 60);
    }
    void idleLinkClimbsTowardsCapDespiteLowGoodput()
    {
        // A scheduled bandwidth window measures bytes actually sent, not link
        // capacity: on an idle desktop the sample is tiny and would otherwise
        // read as "the link can barely carry anything". Without evidence the
        // link was the bottleneck (linkLimited=false), aim for the cap.
        const auto r = step({.current = 30, .cap = 80, .goodputKbit = 80, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms, .linkLimited = false});
        QCOMPARE(r.target, 80);
        QCOMPARE(r.next, 35);
        QVERIFY(!r.congested);
    }
    void limitedLinkStepsDownOnLowGoodput()
    {
        // Same low-goodput sample, but this time frames were actually queuing
        // up (the link really is the bottleneck): trust the goodput-derived
        // target and step down.
        const auto r = step({.current = 30, .cap = 80, .goodputKbit = 80, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms, .linkLimited = true});
        QCOMPARE(r.target, MinQuality);
        QCOMPARE(r.next, 20);
    }
};

QTEST_GUILESS_MAIN(AdaptiveQualityTest)
#include "AdaptiveQualityTest.moc"
