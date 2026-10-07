// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "StreamResumePolicy.h"

#include <QTest>

using namespace KRdp::StreamResumePolicy;

class StreamResumePolicyTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // The 2026-10-07 Sol freeze: stopped consumer (node 0), live KWin screencast on node 50.
    void stoppedStreamWithoutNodeReattachesTheRememberedNode()
    {
        QCOMPARE(decide(0, State::Idle, false, 50, true), (Decision{Action::Reattach, 50}));
    }
    void idleStreamWithItsNodeJustStarts()
    {
        QCOMPARE(decide(50, State::Idle, false, 50, true), (Decision{Action::Start, 50}));
    }
    void streamStillShuttingDownReattachesThroughTheDeferredRestart()
    {
        QCOMPARE(decide(50, State::Rendering, false, 50, true), (Decision{Action::Reattach, 50}));
    }
    void runningStreamIsLeftAlone()
    {
        QCOMPARE(decide(50, State::Recording, false, 50, true), (Decision{}));
    }
    void pendingRestartWins()
    {
        QCOMPARE(decide(0, State::Idle, true, 50, true), (Decision{}));
        QCOMPARE(decide(50, State::Idle, true, 50, true), (Decision{}));
    }
    void noScreencastCreatesANewOne()
    {
        QCOMPARE(decide(0, State::Idle, false, 0, false), (Decision{Action::Recreate, 0}));
        QCOMPARE(decide(0, State::Idle, false, 50, false), (Decision{Action::Recreate, 0}));
    }
    void requestWithoutNodeYetWaitsForCreated()
    {
        QCOMPARE(decide(0, State::Idle, false, 0, true), (Decision{}));
    }
};

QTEST_GUILESS_MAIN(StreamResumePolicyTest)
#include "StreamResumePolicyTest.moc"
