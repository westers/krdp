// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// GAP-G5: the hand-over of a close request to the session thread (no FreeRDP).

#include <QTest>

#include <atomic>
#include <thread>
#include <vector>

#include "SessionCloseGate.h"

using namespace KRdp;
using Route = SessionCloseGate::Route;

class SessionCloseGateTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void beforeTheLoopStartsARequestIsInline()
    {
        SessionCloseGate gate;
        QCOMPARE(gate.submit({}, false), Route::Inline);
        QVERIFY(gate.accepted());
        QVERIFY(!gate.hasPending());
    }

    void whileTheLoopRunsAForeignRequestIsQueuedForTheSessionThread()
    {
        SessionCloseGate gate;
        gate.beginLoop();
        QVERIFY(!gate.take());
        QCOMPARE(gate.submit({ERRINFO_VALUE, true}, false), Route::Queued);
        QVERIFY(gate.hasPending());
        const auto taken = gate.take();
        QVERIFY(taken);
        QCOMPARE(*taken->errorInfo, ERRINFO_VALUE);
        QVERIFY(taken->sendErrorInfoNow);
        QVERIFY(!gate.take()); // once
    }

    void theSessionThreadItselfRunsItInline()
    {
        SessionCloseGate gate;
        gate.beginLoop();
        QCOMPARE(gate.submit({}, true), Route::Inline);
        QVERIFY(!gate.hasPending());
    }

    void theFirstRequestWins()
    {
        SessionCloseGate gate;
        gate.beginLoop();
        QCOMPARE(gate.submit({5u, false}, false), Route::Queued);
        QCOMPARE(gate.submit({9u, true}, false), Route::Ignored);
        QCOMPARE(gate.submit({}, true), Route::Ignored);
        const auto taken = gate.take();
        QVERIFY(taken);
        QCOMPARE(*taken->errorInfo, 5u);
        QVERIFY(!taken->sendErrorInfoNow);
        // Still ignored after it was taken and after the loop ended.
        QCOMPARE(gate.submit({7u, false}, false), Route::Ignored);
        QVERIFY(!gate.finishLoop());
        QCOMPARE(gate.submit({7u, false}, false), Route::Ignored);
    }

    void aRequestThatRacesTheEndOfTheLoopIsHandedToTheSessionThread()
    {
        SessionCloseGate gate;
        gate.beginLoop();
        QCOMPARE(gate.submit({3u, true}, false), Route::Queued);
        const auto last = gate.finishLoop(); // the loop never looked again
        QVERIFY(last);
        QCOMPARE(*last->errorInfo, 3u);
        QVERIFY(!gate.hasPending());
    }

    void afterTheLoopEndedANewRequestIsInline()
    {
        SessionCloseGate gate;
        gate.beginLoop();
        QVERIFY(!gate.finishLoop());
        QCOMPARE(gate.submit({}, false), Route::Inline);
    }

    void aLoopThatCouldNotStartTakesRequestsInline()
    {
        SessionCloseGate gate;
        gate.beginLoop();
        gate.abortLoop();
        QCOMPARE(gate.submit({}, false), Route::Inline);
    }

    // Many threads submit at once: exactly one is accepted, and exactly one
    // party (the queue, or that caller) is told to perform it.
    void concurrentSubmissionsAcceptExactlyOne()
    {
        for (int round = 0; round < 200; ++round) {
            SessionCloseGate gate;
            gate.beginLoop();
            std::atomic<int> queued{0}, inlined{0}, ignored{0};
            std::atomic<bool> go{false};
            std::vector<std::thread> threads;
            for (int i = 0; i < 8; ++i) {
                threads.emplace_back([&, i] {
                    while (!go.load()) {
                    }
                    switch (gate.submit({quint32(i), false}, i == 0)) {
                    case Route::Queued: ++queued; break;
                    case Route::Inline: ++inlined; break;
                    case Route::Ignored: ++ignored; break;
                    }
                });
            }
            go = true;
            for (auto &thread : threads) thread.join();
            QCOMPARE(queued + inlined, 1);
            QCOMPARE(ignored, 7);
            QCOMPARE(gate.hasPending(), queued.load() == 1);
        }
    }

private:
    static constexpr quint32 ERRINFO_VALUE = 0x408;
};

QTEST_GUILESS_MAIN(SessionCloseGateTest)
#include "SessionCloseGateTest.moc"
