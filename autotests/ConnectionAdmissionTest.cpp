// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// REVIEW-S1: the pure admission policy (caps on unauthenticated connections and
// the per-source failure backoff). Time is an argument, so nothing sleeps.

#include <QTest>

#include "ConnectionAdmission.h"

using namespace KRdp;
using namespace std::chrono_literals;
using Verdict = ConnectionAdmission::Verdict;

class ConnectionAdmissionTest : public QObject
{
    Q_OBJECT
    const ConnectionAdmission::Clock::time_point t0 = ConnectionAdmission::Clock::time_point{} + 1000s;

private Q_SLOTS:
    void defaults()
    {
        const ConnectionAdmission::Limits limits;
        QCOMPARE(limits.maxUnauthenticated, 16);
        QCOMPARE(limits.maxUnauthenticatedPerSource, 8);
        QCOMPARE(limits.failuresBeforeBackoff, 3);
        QCOMPARE(limits.failureWindow, 60s);
        QCOMPARE(limits.backoffStart, 2s);
        QCOMPARE(limits.backoffMax, 60s);
        QCOMPARE(limits.maxSources, std::size_t(1024));
    }

    void perSourceCap()
    {
        ConnectionAdmission a;
        // Eight from one address are admitted (e.g. two saved host cards, or several clients behind one NAT).
        for (quint64 i = 1; i <= 8; ++i) {
            QCOMPARE(a.admit(i, QStringLiteral("10.0.0.1"), t0), Verdict::Admit);
        }
        QCOMPARE(a.admit(9, QStringLiteral("10.0.0.1"), t0), Verdict::RefuseSourceCap);
        QCOMPARE(a.pending(), 8); // the refused one is not counted
        QCOMPARE(a.admit(10, QStringLiteral("10.0.0.2"), t0), Verdict::Admit); // another source is unaffected
        a.release(1); // authenticated or closed
        QCOMPARE(a.admit(11, QStringLiteral("10.0.0.1"), t0), Verdict::Admit);
    }

    void totalCap()
    {
        ConnectionAdmission a;
        quint64 id = 1;
        for (int source = 0; source < 4; ++source) {
            for (int n = 0; n < 4; ++n) {
                QCOMPARE(a.admit(id++, QStringLiteral("10.0.1.%1").arg(source), t0), Verdict::Admit);
            }
        }
        QCOMPARE(a.pending(), 16);
        QCOMPARE(a.admit(id++, QStringLiteral("10.0.1.9"), t0), Verdict::RefuseTotalCap);
        a.release(3);
        QCOMPARE(a.admit(id++, QStringLiteral("10.0.1.9"), t0), Verdict::Admit);
    }

    void authenticatedSessionsDoNotCount()
    {
        ConnectionAdmission a;
        // Many sessions that authenticated and were released never fill the cap.
        for (quint64 i = 1; i <= 100; ++i) {
            QCOMPARE(a.admit(i, QStringLiteral("10.0.0.1"), t0), Verdict::Admit);
            a.release(i);
        }
        QCOMPARE(a.pending(), 0);
        a.release(1); // idempotent
        a.release(999); // unknown id
        QCOMPARE(a.pending(), 0);
    }

    void capsCanBeDisabled()
    {
        ConnectionAdmission::Limits limits;
        limits.maxUnauthenticated = 0;
        limits.maxUnauthenticatedPerSource = 0;
        ConnectionAdmission a(limits);
        for (quint64 i = 1; i <= 100; ++i) {
            QCOMPARE(a.admit(i, QStringLiteral("10.0.0.1"), t0), Verdict::Admit);
        }
    }

    void backoffStartsAtThirdFailureAndDoubles()
    {
        ConnectionAdmission a;
        const QString s = QStringLiteral("10.0.0.1");
        QCOMPARE(a.recordFailure(s, t0), 0s);
        QCOMPARE(a.recordFailure(s, t0 + 1s), 0s);
        QCOMPARE(a.admit(1, s, t0 + 1s), Verdict::Admit); // two failures: no delay yet
        a.release(1);
        QCOMPARE(a.recordFailure(s, t0 + 2s), 2s);
        QCOMPARE(a.admit(2, s, t0 + 3s), Verdict::RefuseBackoff);
        QCOMPARE(a.admit(2, s, t0 + 4s), Verdict::Admit); // 2 s after the third failure
        a.release(2);
        QCOMPARE(a.recordFailure(s, t0 + 4s), 4s);
        QCOMPARE(a.recordFailure(s, t0 + 8s), 8s);
        QCOMPARE(a.recordFailure(s, t0 + 16s), 16s);
        QCOMPARE(a.recordFailure(s, t0 + 32s), 32s);
        QCOMPARE(a.recordFailure(s, t0 + 64s), 60s); // capped
        QVERIFY(a.blocked(s, t0 + 123s));
        QVERIFY(!a.blocked(s, t0 + 124s));
        // Once the 60 s block has run out, the earlier failures have left the window: three
        // more are needed, and the delay has not been forgiven, so it is 60 s again.
        QCOMPARE(a.recordFailure(s, t0 + 124s), 0s);
        QCOMPARE(a.recordFailure(s, t0 + 125s), 0s);
        QCOMPARE(a.recordFailure(s, t0 + 126s), 60s);
        QVERIFY(a.blocked(s, t0 + 185s));
        QVERIFY(!a.blocked(s, t0 + 186s));
        QCOMPARE(a.admit(3, QStringLiteral("10.0.0.2"), t0 + 100s), Verdict::Admit); // other sources never blocked
    }

    void failuresOutsideTheWindowDoNotAccumulate()
    {
        ConnectionAdmission a;
        const QString s = QStringLiteral("10.0.0.1");
        QCOMPARE(a.recordFailure(s, t0), 0s);
        QCOMPARE(a.recordFailure(s, t0 + 40s), 0s);
        QCOMPARE(a.recordFailure(s, t0 + 90s), 0s); // the first is 90 s old: only two in the window
        QVERIFY(!a.blocked(s, t0 + 90s));
    }

    void quietWindowResetsTheDoubling()
    {
        ConnectionAdmission a;
        const QString s = QStringLiteral("10.0.0.1");
        a.recordFailure(s, t0);
        a.recordFailure(s, t0 + 1s);
        QCOMPARE(a.recordFailure(s, t0 + 2s), 2s);
        QCOMPARE(a.recordFailure(s, t0 + 4s), 4s);
        // A long quiet time, then three more: the delay starts over at 2 s.
        a.recordFailure(s, t0 + 200s);
        a.recordFailure(s, t0 + 201s);
        QCOMPARE(a.recordFailure(s, t0 + 202s), 2s);
    }

    void successClearsTheSource()
    {
        ConnectionAdmission a;
        const QString s = QStringLiteral("10.0.0.1");
        a.recordFailure(s, t0);
        a.recordFailure(s, t0 + 1s);
        a.recordFailure(s, t0 + 2s);
        QVERIFY(a.blocked(s, t0 + 3s));
        a.recordSuccess(s);
        QVERIFY(!a.blocked(s, t0 + 3s));
        QCOMPARE(a.trackedSources(), std::size_t(0));
        QCOMPARE(a.recordFailure(s, t0 + 4s), 0s); // counting starts over
    }

    void backoffCanBeDisabled()
    {
        ConnectionAdmission::Limits limits;
        limits.failuresBeforeBackoff = 0;
        ConnectionAdmission a(limits);
        for (int i = 0; i < 20; ++i) {
            QCOMPARE(a.recordFailure(QStringLiteral("10.0.0.1"), t0 + std::chrono::seconds(i)), 0s);
        }
        QCOMPARE(a.admit(1, QStringLiteral("10.0.0.1"), t0 + 21s), Verdict::Admit);
        QCOMPARE(a.trackedSources(), std::size_t(0));
    }

    void failureTableIsBoundedAndDropsTheOldest()
    {
        ConnectionAdmission::Limits limits;
        limits.maxSources = 8;
        ConnectionAdmission a(limits);
        for (int i = 0; i < 8; ++i) {
            // Three failures each: all blocked.
            const QString s = QStringLiteral("10.1.0.%1").arg(i);
            a.recordFailure(s, t0 + std::chrono::seconds(i));
            a.recordFailure(s, t0 + std::chrono::seconds(i));
            a.recordFailure(s, t0 + std::chrono::seconds(i));
        }
        QCOMPARE(a.trackedSources(), std::size_t(8));
        a.recordFailure(QStringLiteral("10.9.9.9"), t0 + 20s);
        QCOMPARE(a.trackedSources(), std::size_t(8)); // not 9
        // The oldest source (10.1.0.0, last failure at t0) was dropped: it is no longer blocked.
        QCOMPARE(a.admit(1, QStringLiteral("10.1.0.0"), t0 + 1s), Verdict::Admit);
        QCOMPARE(a.admit(2, QStringLiteral("10.1.0.7"), t0 + 8s), Verdict::RefuseBackoff); // the newest is kept
        for (int i = 0; i < 5000; ++i) {
            a.recordFailure(QStringLiteral("flood-%1").arg(i), t0 + 30s + std::chrono::seconds(i));
        }
        QVERIFY(a.trackedSources() <= 8);
    }

    void sourceKeyNormalisesAddresses()
    {
        QCOMPARE(ConnectionAdmission::sourceKey(QHostAddress(QStringLiteral("127.0.0.1"))), QStringLiteral("127.0.0.1"));
        QCOMPARE(ConnectionAdmission::sourceKey(QHostAddress(QStringLiteral("::ffff:192.168.1.7"))), QStringLiteral("192.168.1.7"));
        const auto a = ConnectionAdmission::sourceKey(QHostAddress(QStringLiteral("2001:db8:1:2:aaaa:bbbb:cccc:dddd")));
        const auto b = ConnectionAdmission::sourceKey(QHostAddress(QStringLiteral("2001:db8:1:2:1:2:3:4%eth0")));
        const auto c = ConnectionAdmission::sourceKey(QHostAddress(QStringLiteral("2001:db8:1:3::1")));
        QCOMPARE(a, b); // same /64, scope id dropped
        QVERIFY(a != c);
        QVERIFY(a.endsWith(QStringLiteral("/64")));
        QCOMPARE(ConnectionAdmission::sourceKey(QHostAddress()), QStringLiteral("unknown"));
    }

    void refusalLogIsRateLimited()
    {
        ConnectionAdmission a;
        QCOMPARE(a.takeRefusalLog(t0).value_or(-1), 1);
        QVERIFY(!a.takeRefusalLog(t0 + 1s));
        QVERIFY(!a.takeRefusalLog(t0 + 9s));
        QCOMPARE(a.takeRefusalLog(t0 + 10s).value_or(-1), 3); // two suppressed plus this one
        QCOMPARE(a.takeRefusalLog(t0 + 25s).value_or(-1), 1);
    }
};

QTEST_GUILESS_MAIN(ConnectionAdmissionTest)
#include "ConnectionAdmissionTest.moc"
