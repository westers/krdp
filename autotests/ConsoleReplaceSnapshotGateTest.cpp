// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "ConsoleReplaceSnapshotGate.h"
#include <QTest>

using namespace KRdp;
using namespace Qt::StringLiterals;
namespace Gate = ConsoleReplaceSnapshotGate;
using Reading = std::optional<QVector<OutputSnapshot::Output>>;

// OPT-082: the worker waits for the outputs it captures to be back in kscreen before it snapshots them.
class ConsoleReplaceSnapshotGateTest : public QObject
{
    Q_OBJECT
    static OutputSnapshot::Output out(const QString &name, int x, int priority, bool enabled = true)
    {
        return {name, enabled, QPoint(x, 0), enabled ? priority : 0, QSize(2560, 1440), 1.0};
    }
    static Reading both() { return QVector<OutputSnapshot::Output>{out(u"DP-1"_s, 0, 1), out(u"HDMI-A-1"_s, 2560, 2)}; }
    static Reading onlyHdmi() { return QVector<OutputSnapshot::Output>{out(u"HDMI-A-1"_s, 0, 1)}; }
    static QStringList captured() { return {u"DP-1"_s, u"HDMI-A-1"_s}; }

private Q_SLOTS:
    void allPresentAndStableProceeds()
    {
        Gate::Waiter gate(captured());
        // The first complete reading only starts the stability check.
        QCOMPARE(gate.observe(both(), 0).state, Gate::State::Wait);
        const auto second = gate.observe(both(), Gate::PollMs);
        QCOMPARE(second.state, Gate::State::Ready);
        QVERIFY(second.missing.isEmpty());
        QVERIFY(second.unexpected.isEmpty());
    }

    void oneMissingPastTheTimeoutFailsOpenAndNamesIt()
    {
        Gate::Waiter gate(captured());
        for (int t = 0; t < Gate::TimeoutMs; t += Gate::PollMs) {
            const auto verdict = gate.observe(onlyHdmi(), t);
            QCOMPARE(verdict.state, Gate::State::Wait);
            QCOMPARE(verdict.missing, QStringList{u"DP-1"_s});
        }
        const auto last = gate.observe(onlyHdmi(), Gate::TimeoutMs);
        QCOMPARE(last.state, Gate::State::TimedOut);
        QCOMPARE(last.missing, QStringList{u"DP-1"_s});
        QVERIFY(Gate::timeoutReason(last, false).contains(u"DP-1"_s));
        QVERIFY(!Gate::timeoutReason(last, false).contains(u"HDMI-A-1"_s));
    }

    void anOutputThatAppearsLateProceeds()
    {
        Gate::Waiter gate(captured());
        QCOMPARE(gate.observe(onlyHdmi(), 0).state, Gate::State::Wait);
        QCOMPARE(gate.observe(onlyHdmi(), 250).state, Gate::State::Wait);
        QCOMPARE(gate.observe(both(), 3500).state, Gate::State::Wait); // back, but not yet stable
        QCOMPARE(gate.observe(both(), 3750).state, Gate::State::Ready);
    }

    void aSetThatKeepsChangingIsNotStable()
    {
        Gate::Waiter gate(captured());
        QCOMPARE(gate.observe(both(), 0).state, Gate::State::Wait);
        // An unrelated third output appears between the polls: the set changed, so one more poll is needed.
        auto three = *both();
        three.append(out(u"DP-3"_s, 5120, 3));
        QCOMPARE(gate.observe(Reading(three), 250).state, Gate::State::Wait);
        const auto settled = gate.observe(Reading(three), 500);
        QCOMPARE(settled.state, Gate::State::Ready);
        QCOMPARE(settled.unexpected, QStringList{u"DP-3"_s}); // reported, never waited for
    }

    void anOutputThatDropsOutAgainRestartsTheStabilityCheck()
    {
        Gate::Waiter gate(captured());
        QCOMPARE(gate.observe(both(), 0).state, Gate::State::Wait);
        QCOMPARE(gate.observe(onlyHdmi(), 250).state, Gate::State::Wait); // KWin removed DP-1 again
        QCOMPARE(gate.observe(both(), 500).state, Gate::State::Wait);
        QCOMPARE(gate.observe(both(), 750).state, Gate::State::Ready);
    }

    void aSetStillChangingAtTheTimeoutFailsOpenWithoutNames()
    {
        Gate::Waiter gate(captured());
        QCOMPARE(gate.observe(both(), 0).state, Gate::State::Wait);
        auto three = *both();
        three.append(out(u"DP-3"_s, 5120, 3));
        const auto verdict = gate.observe(Reading(three), Gate::TimeoutMs); // complete, but the set just changed
        QCOMPARE(verdict.state, Gate::State::TimedOut);
        QVERIFY(verdict.missing.isEmpty());
        QVERIFY(Gate::timeoutReason(verdict, false).contains(u"still changing"_s));
    }

    void aDisabledOutputIsNotPresent()
    {
        Gate::Waiter gate(captured());
        const Reading reading = QVector<OutputSnapshot::Output>{out(u"DP-1"_s, 0, 1, false), out(u"HDMI-A-1"_s, 2560, 2)};
        const auto verdict = gate.observe(reading, 0);
        QCOMPARE(verdict.state, Gate::State::Wait);
        QCOMPARE(verdict.missing, QStringList{u"DP-1"_s});
    }

    void virtualOutputsAreIgnoredOnBothSides()
    {
        // A leftover Virtual- screen in the worker's own list is not waited for ...
        Gate::Waiter gate({u"DP-1"_s, u"Virtual-krdp-h0-1920x1080"_s});
        QCOMPARE(gate.expected(), QStringList{u"DP-1"_s});
        // ... and one in kscreen neither satisfies nor disturbs the wait.
        const Reading withVirtual = QVector<OutputSnapshot::Output>{out(u"Virtual-krdp-h0-1920x1080"_s, 0, 1)};
        QCOMPARE(gate.observe(withVirtual, 0).missing, QStringList{u"DP-1"_s});
        const Reading real = QVector<OutputSnapshot::Output>{out(u"DP-1"_s, 0, 1), out(u"Virtual-krdp-h0-1920x1080"_s, 2560, 2)};
        QCOMPARE(gate.observe(real, 250).state, Gate::State::Wait);
        const auto verdict = gate.observe(real, 500);
        QCOMPARE(verdict.state, Gate::State::Ready);
        QVERIFY(verdict.unexpected.isEmpty());
    }

    void nothingCapturedProceedsAtOnceAfterOneStableReading()
    {
        Gate::Waiter gate({});
        QVERIFY(gate.expected().isEmpty());
        QCOMPARE(gate.observe(both(), 0).state, Gate::State::Wait);
        QCOMPARE(gate.observe(both(), 250).state, Gate::State::Ready);
    }

    void anUnreadableKScreenWaitsThenTimesOutWithAllNames()
    {
        Gate::Waiter gate(captured());
        QCOMPARE(gate.observe(std::nullopt, 0).state, Gate::State::Wait);
        const auto verdict = gate.observe(std::nullopt, Gate::TimeoutMs);
        QCOMPARE(verdict.state, Gate::State::TimedOut);
        QCOMPARE(verdict.missing, (QStringList{u"DP-1"_s, u"HDMI-A-1"_s}));
        QVERIFY(Gate::timeoutReason(verdict, true).contains(u"could not be read"_s));
    }

    void missingNamesAgainstASnapshot()
    {
        // The worker's last check before it holds anything: the snapshot it is about to plan from.
        const auto snapshot = OutputSnapshot::physicalOnly(*onlyHdmi());
        QCOMPARE(Gate::missingNames(captured(), snapshot), QStringList{u"DP-1"_s});
        QVERIFY(Gate::missingNames(captured(), *both()).isEmpty());
    }

    // ---- the broker's check of what the worker created --------------------------------------------------------------
    void agreementCases()
    {
        const QStringList predicted{u"Virtual-krdp-h0-1920x1080"_s, u"Virtual-krdp-h1-1920x1080"_s};
        QCOMPARE(Gate::agreement(predicted, predicted), Gate::Agreement::Consistent);
        // The worker reports names without the prefix in some paths.
        QCOMPARE(Gate::agreement(predicted, {u"krdp-h0-1920x1080"_s, u"krdp-h1-1920x1080"_s}), Gate::Agreement::Consistent);
        // The silent bug: a snapshot of one host screen made a one-screen plan for a two-screen host.
        QCOMPARE(Gate::agreement(predicted, {u"Virtual-krdp-h0-1920x1080"_s}), Gate::Agreement::CountDiffers);
        QCOMPARE(Gate::agreement(predicted, {u"Virtual-other-0"_s}), Gate::Agreement::CountDiffers);
        // The worker's own bounded single fallback is a different, working state.
        QCOMPARE(Gate::agreement(predicted, {u"Virtual-krdp-m0-1920x1080"_s}), Gate::Agreement::SingleFallback);
        QCOMPARE(Gate::agreement(predicted, {u"Virtual-a"_s, u"Virtual-b"_s}), Gate::Agreement::NamesDiffer);
    }
};

QTEST_GUILESS_MAIN(ConsoleReplaceSnapshotGateTest)
#include "ConsoleReplaceSnapshotGateTest.moc"
