// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>

#include "ConsoleReleaseLockGuard.h"

using namespace KRdp::ConsoleReleaseLock;
Q_DECLARE_METATYPE(std::optional<qint64>)

class ConsoleReleaseLockGuardTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void hold_data()
    {
        QTest::addColumn<std::optional<qint64>>("since");
        QTest::addColumn<qint64>("held");
        QTest::addColumn<qint64>("wait");
        QTest::newRow("no lock edge seen") << std::optional<qint64>() << qint64(0) << qint64(0);
        QTest::newRow("lock 0 ms ago waits the full settle") << std::optional<qint64>(0) << qint64(0) << qint64(2000);
        QTest::newRow("lock 500 ms ago") << std::optional<qint64>(500) << qint64(0) << qint64(1500);
        QTest::newRow("lock 1999 ms ago") << std::optional<qint64>(1999) << qint64(0) << qint64(1);
        QTest::newRow("lock exactly 2 s ago: proceed") << std::optional<qint64>(2000) << qint64(0) << qint64(0);
        QTest::newRow("lock long ago: proceed") << std::optional<qint64>(60000) << qint64(0) << qint64(0);
        QTest::newRow("clock went backwards: proceed") << std::optional<qint64>(-5) << qint64(0) << qint64(0);
        QTest::newRow("total hold is capped") << std::optional<qint64>(0) << qint64(1000) << qint64(1500);
        QTest::newRow("already held to the cap: no more") << std::optional<qint64>(0) << qint64(2500) << qint64(0);
    }
    void hold()
    {
        QFETCH(std::optional<qint64>, since); QFETCH(qint64, held); QFETCH(qint64, wait);
        QCOMPARE(holdBeforeRelease(since, held), wait);
    }

    void verdict_data()
    {
        QTest::addColumn<bool>("before"); QTest::addColumn<bool>("during");
        QTest::addColumn<int>("locked"); // -1 unknown, 0 no, 1 yes
        QTest::addColumn<int>("greeter"); // -1 unknown, 0 dead, 1 alive
        QTest::addColumn<int>("attempts"); QTest::addColumn<qint64>("elapsed");
        QTest::addColumn<int>("step");
        const int done = int(Step::Done), relock = int(Step::Relock), giveUp = int(Step::GiveUp);
        QTest::newRow("never locked, unlocked") << false << false << 0 << 1 << 0 << qint64(0) << done;
        QTest::newRow("never locked, greeter dead is irrelevant") << false << false << 0 << 0 << 0 << qint64(0) << done;
        QTest::newRow("locked before, still locked, greeter alive") << true << false << 1 << 1 << 0 << qint64(0) << done;
        QTest::newRow("locked before, greeter state unknown") << true << false << 1 << -1 << 0 << qint64(0) << done;
        QTest::newRow("locked before, greeter dead (OPT-049)") << true << false << 1 << 0 << 0 << qint64(0) << relock;
        QTest::newRow("locked before, now unlocked") << true << false << 0 << 1 << 0 << qint64(0) << relock;
        QTest::newRow("lock started during the window, greeter killed") << false << true << 0 << 0 << 0 << qint64(300) << relock;
        QTest::newRow("lock started during the window, holds") << false << true << 1 << 1 << 0 << qint64(300) << done;
        QTest::newRow("no answer from any source: try to lock") << true << false << -1 << -1 << 0 << qint64(0) << relock;
        QTest::newRow("second attempt still failing") << true << false << 0 << 0 << 1 << qint64(1500) << relock;
        QTest::newRow("third attempt still failing") << true << false << 0 << 0 << 2 << qint64(3000) << relock;
        QTest::newRow("attempts exhausted: give up") << true << false << 0 << 0 << 3 << qint64(4500) << giveUp;
        QTest::newRow("time bound reached: give up") << true << false << 0 << 0 << 1 << qint64(9000) << giveUp;
        QTest::newRow("holds on the last attempt: done not give up") << true << false << 1 << 1 << 3 << qint64(9500) << done;
    }
    void verdict()
    {
        QFETCH(bool, before); QFETCH(bool, during); QFETCH(int, locked); QFETCH(int, greeter);
        QFETCH(int, attempts); QFETCH(qint64, elapsed); QFETCH(int, step);
        const auto toOptional = [](int value) { return value < 0 ? std::optional<bool>() : std::optional<bool>(value == 1); };
        const auto verdict = afterRestore({before, during}, {toOptional(locked), toOptional(greeter)}, attempts, elapsed);
        QCOMPARE(int(verdict.step), step);
        QVERIFY(qstrlen(verdict.reason) > 0);
    }

    void neverLoopsForever()
    {
        // A permanently failing re-lock ends in GiveUp after exactly MaxRelockAttempts requests.
        int attempts = 0; qint64 elapsed = 0; int guard = 0;
        while (guard++ < 100) {
            const auto verdict = afterRestore({true, false}, {false, false}, attempts, elapsed);
            if (verdict.step == Step::GiveUp) break;
            QCOMPARE(int(verdict.step), int(Step::Relock));
            ++attempts; elapsed += RelockWaitMs;
        }
        QCOMPARE(attempts, MaxRelockAttempts);
        QVERIFY(elapsed <= TotalBoundMs);
    }
};

QTEST_GUILESS_MAIN(ConsoleReleaseLockGuardTest)
#include "ConsoleReleaseLockGuardTest.moc"
