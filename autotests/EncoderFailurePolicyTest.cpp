// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "EncoderFailurePolicy.h"

#include <QTest>

using namespace KRdp;
using namespace std::chrono_literals;
using Action = EncoderFailurePolicy::Action;

class EncoderFailurePolicyTest : public QObject
{
    Q_OBJECT
private:
    static EncoderFailurePolicy::Clock::time_point at(std::chrono::seconds s)
    {
        return EncoderFailurePolicy::Clock::time_point{} + 1000s + s;
    }
private Q_SLOTS:
    void restartRestartClose()
    {
        EncoderFailurePolicy p;
        QCOMPARE(p.onFailure(at(0s), false, true), Action::Restart);
        QCOMPARE(p.onFailure(at(5s), false, true), Action::Restart);
        QCOMPARE(p.onFailure(at(10s), false, true), Action::Close);
        QCOMPARE(p.onFailure(at(11s), false, true), Action::Close);
    }
    void budgetResetsAfterSixtySeconds()
    {
        EncoderFailurePolicy p;
        QCOMPARE(p.onFailure(at(0s), false, true), Action::Restart);
        QCOMPARE(p.onFailure(at(5s), false, true), Action::Restart);
        QCOMPARE(p.onFailure(at(64s), false, true), Action::Restart); // the first aged out (64 - 0 >= 60), the second did not
        QCOMPARE(p.onFailure(at(64s), false, true), Action::Close);
        QCOMPARE(p.onFailure(at(200s), false, true), Action::Restart);
    }
    void ignoredWhileRestartingOrNotStreaming()
    {
        EncoderFailurePolicy p;
        QCOMPARE(p.onFailure(at(0s), true, true), Action::Ignore);
        QCOMPARE(p.onFailure(at(0s), false, false), Action::Ignore);
        // Ignored failures spend no budget.
        QCOMPARE(p.onFailure(at(1s), false, true), Action::Restart);
        QCOMPARE(p.onFailure(at(2s), false, true), Action::Restart);
        QCOMPARE(p.onFailure(at(3s), true, true), Action::Ignore);
        QCOMPARE(p.onFailure(at(4s), false, true), Action::Close);
    }
    void resetForgivesEverything()
    {
        EncoderFailurePolicy p;
        p.onFailure(at(0s), false, true);
        p.onFailure(at(1s), false, true);
        p.reset();
        QCOMPARE(p.onFailure(at(2s), false, true), Action::Restart);
    }
    void idleRestartDecider()
    {
        QVERIFY(!IdleRestartPolicy::shouldArm(0, 0));
        QVERIFY(!IdleRestartPolicy::shouldArm(1, 1));
        QVERIFY(IdleRestartPolicy::shouldArm(0, 1));
        QVERIFY(IdleRestartPolicy::shouldExit(0, 3));
        QVERIFY(!IdleRestartPolicy::shouldExit(1, 3)); // a client came back inside the delay
        QVERIFY(!IdleRestartPolicy::shouldExit(0, 0));
        QCOMPARE(IdleRestartPolicy::ExitCode, 70);
        QCOMPARE(IdleRestartPolicy::Delay.count(), 10);
    }
};

QTEST_GUILESS_MAIN(EncoderFailurePolicyTest)
#include "EncoderFailurePolicyTest.moc"
