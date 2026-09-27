// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "ScreencastTarget.h"

using namespace Qt::StringLiterals;
using namespace KRdp;
using ScreencastTarget::Kind;
using ScreencastTarget::resolve;
using ScreencastTarget::Screen;

namespace
{
const QRect Left(0, 0, 2560, 1440);
const QRect Right(2560, 0, 2560, 1440);

QList<Screen> halScreens(bool dpPrimary = true)
{
    // Qt keeps the primary first, so a primary change reorders the list.
    if (dpPrimary) {
        return {Screen{u"DP-1"_s, Left, true}, Screen{u"HDMI-A-1"_s, Right, false}};
    }
    return {Screen{u"HDMI-A-1"_s, Right, true}, Screen{u"DP-1"_s, Left, false}};
}

// The invariant AUD-P1 is about: the rect input is mapped through is exactly
// the geometry of the screen that is captured.
void verifyAgreement(const QList<Screen> &screens, const ScreencastTarget::Resolution &resolution)
{
    QCOMPARE(resolution.kind, Kind::Output);
    QVERIFY(resolution.screenIndex >= 0 && resolution.screenIndex < screens.size());
    QCOMPARE(resolution.name, screens.at(resolution.screenIndex).name);
    QCOMPARE(resolution.logicalRect, screens.at(resolution.screenIndex).geometry);
    QCOMPARE(resolution.monitors.size(), 1);
    QCOMPARE(resolution.monitors.first().geometry, QRect(QPoint(0, 0), resolution.logicalRect.size()));
    QVERIFY(resolution.monitors.first().primary);
}
}

class ScreencastTargetTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void indexChangeMovesCaptureAndInputTogether()
    {
        const auto screens = halScreens();
        const auto first = resolve(screens, 0, {}, -1, false, true);
        verifyAgreement(screens, first);
        QCOMPARE(first.name, u"DP-1"_s);

        // MonitorIndex 0 -> 1 on a live connection: the remembered DP-1 must
        // not keep the capture on DP-1 while input goes to HDMI-A-1's rect.
        // A refresh ignores the name; so does a recovery, as the name was
        // remembered for another index.
        for (const bool recovery : {false, true}) {
            const auto second = resolve(screens, 1, first.name, 0, recovery, true);
            verifyAgreement(screens, second);
            QCOMPARE(second.name, u"HDMI-A-1"_s);
            QCOMPARE(second.logicalRect, Right);
        }
    }

    void primaryChangeFollowsTheIndex()
    {
        // Primary mode: the controller re-resolves the primary's index (0,
        // as Qt moves the primary to the front). A refresh must capture the
        // new primary, and input must follow the same screen.
        const auto before = resolve(halScreens(true), 0, {}, -1, false, true);
        QCOMPARE(before.name, u"DP-1"_s);
        const auto screens = halScreens(false);
        const auto after = resolve(screens, 0, before.name, 0, false, true);
        verifyAgreement(screens, after);
        QCOMPARE(after.name, u"HDMI-A-1"_s);
        QCOMPARE(after.logicalRect, Right);
    }

    void recoveryHoldsOnToTheScreenByName()
    {
        // A DPMS wake re-adds the outputs, possibly in another order: a
        // recovery keeps the screen it had, and its rect comes from that
        // screen, not from whatever sits at the index now.
        const auto screens = halScreens(false);
        const auto recovered = resolve(screens, 0, u"DP-1"_s, 0, true, false);
        verifyAgreement(screens, recovered);
        QCOMPARE(recovered.name, u"DP-1"_s);
        QCOMPARE(recovered.logicalRect, Left);
    }

    void recoveryWaitsThenFallsBackToTheWorkspace()
    {
        const QList<Screen> screens{Screen{u"HDMI-A-1"_s, Right, true}};
        const auto waiting = resolve(screens, 0, u"DP-1"_s, 0, true, false);
        QCOMPARE(waiting.kind, Kind::Wait);

        const auto fallback = resolve(screens, 0, u"DP-1"_s, 0, true, true);
        QCOMPARE(fallback.kind, Kind::Workspace);
        QCOMPARE(fallback.logicalRect, Right);
        QVERIFY(fallback.name.isEmpty());
    }

    void workspaceSpansEveryScreenWithOnePrimary()
    {
        const auto screens = halScreens(false);
        const auto workspace = resolve(screens, -1, u"DP-1"_s, 0, false, true);
        QCOMPARE(workspace.kind, Kind::Workspace);
        QCOMPARE(workspace.logicalRect, QRect(0, 0, 5120, 1440));
        QCOMPARE(workspace.monitors.size(), 2);
        QCOMPARE(workspace.monitors.at(0).geometry, Right);
        QVERIFY(workspace.monitors.at(0).primary);
        QVERIFY(!workspace.monitors.at(1).primary);

        const auto outOfRange = resolve(screens, 5, {}, -1, false, true);
        QCOMPARE(outOfRange.kind, Kind::Workspace);
        QVERIFY(resolve({}, 0, {}, -1, false, true).logicalRect.isEmpty());
    }
};

QTEST_GUILESS_MAIN(ScreencastTargetTest)

#include "ScreencastTargetTest.moc"
