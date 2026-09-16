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
};

QTEST_GUILESS_MAIN(AdaptiveQualityTest)
#include "AdaptiveQualityTest.moc"
