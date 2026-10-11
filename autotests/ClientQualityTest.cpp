// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-066: the pure client-quality planner: the ceiling, the clamp, the request range and the debounce of a
// re-sent quality (settle time and the minimum interval between applies).

#include "ClientQuality.h"

#include <QTest>

using namespace KRdp::ClientQuality;
using namespace std::chrono_literals;

class ClientQualityTest : public QObject
{
    Q_OBJECT

    const Clock::time_point T0 = Clock::time_point(1h);

private Q_SLOTS:
    void theCeilingIsClientQualityMaxOrTheHostsQuality()
    {
        QCOMPARE(ceiling(75, 0), 75);
        QCOMPARE(ceiling(75, 90), 90);
        QCOMPARE(ceiling(75, 40), 40); // a lower maximum is honoured literally
        QCOMPARE(ceiling(75, 250), 100); // never above 100
    }

    void requestRange()
    {
        QVERIFY(!validRequest(9));
        QVERIFY(validRequest(10));
        QVERIFY(validRequest(100));
        QVERIFY(!validRequest(101));
        QVERIFY(!validRequest(-1));
    }

    void noRequestIsTheHostsDefault()
    {
        const auto p = plan(75, 0, std::nullopt);
        QCOMPARE(p.cap, 75);
        QVERIFY(!p.clamped);
        QCOMPARE(plan(60, 90, std::nullopt).cap, 60); // the default stays Quality even when clients may ask for more
    }

    void aRequestIsClampedToTheCeiling()
    {
        QCOMPARE(plan(75, 0, 60).cap, 60);
        QVERIFY(!plan(75, 0, 60).clamped);
        QCOMPARE(plan(75, 0, 75).cap, 75);
        const auto over = plan(75, 0, 90);
        QCOMPARE(over.cap, 75); // a client may NOT exceed the host's allowed maximum
        QVERIFY(over.clamped);
        QCOMPARE(over.ceiling, 75);
        QCOMPARE(plan(60, 90, 85).cap, 85);
        QCOMPARE(plan(60, 90, 100).cap, 90);
        QVERIFY(plan(60, 90, 100).clamped);
        QCOMPARE(plan(80, 0, 10).cap, 10); // the floor is a valid request
    }

    void theResultNeverExceedsTheCeilingEvenForOddHostValues()
    {
        for (int host : {0, 5, 10, 50, 100}) {
            for (int max : {0, 20, 100}) {
                for (int request : {10, 11, 50, 100}) {
                    const auto p = plan(host, max, request);
                    QVERIFY2(p.cap <= p.ceiling, qPrintable(QStringLiteral("host %1 max %2 request %3").arg(host).arg(max).arg(request)));
                }
            }
        }
        QCOMPARE(plan(5, 0, 50).cap, 5); // Quality below the request floor: the ceiling still wins
    }

    void theFirstRequestAppliesAtOnce()
    {
        QCOMPARE(applyDelay(T0, T0, std::nullopt), Clock::duration::zero());
    }

    void aLaterRequestWaitsForTheValueToSettle()
    {
        const auto lastApply = T0 - 10s; // long ago: only the settle time counts
        QCOMPARE(applyDelay(T0, T0, lastApply), Clock::duration(SettleTime));
        QCOMPARE(applyDelay(T0 + 200ms, T0, lastApply), Clock::duration(300ms));
        QCOMPARE(applyDelay(T0 + SettleTime, T0, lastApply), Clock::duration::zero());
        QCOMPARE(applyDelay(T0 + 5s, T0, lastApply), Clock::duration::zero()); // never negative
    }

    void appliesAreSpacedByTheMinimumInterval()
    {
        const auto lastApply = T0;
        // A slider that stopped 600 ms after the previous apply: settled, but the interval is not over.
        QCOMPARE(applyDelay(T0 + 1100ms, T0 + 600ms, lastApply), Clock::duration(900ms));
        QCOMPARE(applyDelay(T0 + MinApplyInterval, T0 + 600ms, lastApply), Clock::duration::zero());
    }

    void aDragReopensOnceNotOnEveryTick()
    {
        // 40 ticks 50 ms apart after an apply at T0: the delay keeps sliding with the last change and the apply happens
        // once, MinApplyInterval after the previous one.
        auto lastChange = T0;
        auto now = T0;
        for (int i = 0; i < 40; ++i) {
            now = T0 + 50ms * (i + 1);
            lastChange = now;
            QVERIFY(applyDelay(now, lastChange, T0) > Clock::duration::zero());
        }
        QCOMPARE(applyDelay(now, lastChange, T0), Clock::duration(SettleTime));
    }

    void effectNamesAreTheWireNames()
    {
        QCOMPARE(QString::fromLatin1(effectName(Effect::None)), QStringLiteral("none"));
        QCOMPARE(QString::fromLatin1(effectName(Effect::Applied)), QStringLiteral("applied"));
        QCOMPARE(QString::fromLatin1(effectName(Effect::Pending)), QStringLiteral("pending"));
        QCOMPARE(QString::fromLatin1(effectName(Effect::IgnoredBitrateMode)), QStringLiteral("ignoredBitrateMode"));
        QCOMPARE(QString::fromLatin1(effectName(Effect::NotController)), QStringLiteral("notController"));
    }
};

QTEST_GUILESS_MAIN(ClientQualityTest)
#include "ClientQualityTest.moc"
