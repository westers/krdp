// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-055 K5.1 (T-K5a): LogThrottle and LogEdge (src/LogThrottle.h), with an injected clock.

#include "LogThrottle.h"

#include <QTest>
#include <atomic>
#include <thread>
#include <vector>

using namespace KRdp;
using namespace std::chrono_literals;

class LogThrottleTest : public QObject
{
    Q_OBJECT
    using TP = LogThrottle::Clock::time_point;
    const TP T0 = TP(1h);

private Q_SLOTS:
    void firstCallAllowed()
    {
        LogThrottle t(10s);
        quint64 s = 99;
        QVERIFY(t.allow(T0, &s));
        QCOMPARE(s, quint64(0));
    }
    void suppressesWithinIntervalAndReportsCount()
    {
        LogThrottle t(10s);
        QVERIFY(t.allow(T0));
        quint64 s = 0;
        for (int i = 0; i < 999; ++i) {
            QVERIFY(!t.allow(T0 + 1s, &s));
        }
        QCOMPARE(t.suppressedTotal(), quint64(999));
        QVERIFY(t.allow(T0 + 10s, &s));
        QCOMPARE(s, quint64(999));
        QVERIFY(!t.allow(T0 + 10s, &s)); // counter restarted
        QVERIFY(t.allow(T0 + 20s, &s));
        QCOMPARE(s, quint64(1));
        QCOMPARE(t.suppressedTotal(), quint64(1000));
    }
    void burstAllowsExactlyN()
    {
        LogThrottle t(10s, 3);
        int allowed = 0;
        for (int i = 0; i < 50; ++i) {
            allowed += t.allow(T0 + 1s) ? 1 : 0;
        }
        QCOMPARE(allowed, 3);
        allowed = 0;
        for (int i = 0; i < 50; ++i) {
            allowed += t.allow(T0 + 11s) ? 1 : 0;
        }
        QCOMPARE(allowed, 3);
    }
    void resetReallowsImmediately()
    {
        LogThrottle t(10s);
        QVERIFY(t.allow(T0));
        QVERIFY(!t.allow(T0 + 1s));
        t.reset();
        QVERIFY(t.allow(T0 + 2s));
    }
    void concurrentFrozenClock()
    {
        LogThrottle t(10s);
        std::atomic<int> allowed{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                for (int n = 0; n < 100000; ++n) {
                    if (t.allow(T0)) {
                        ++allowed;
                    }
                }
            });
        }
        for (auto &th : threads) {
            th.join();
        }
        QCOMPARE(allowed.load(), 1);
        QCOMPARE(t.suppressedTotal(), quint64(399999));
    }
    void macroAppendsSuppressedNote()
    {
        LogThrottle t(10s);
        QStringList out;
        for (int i = 0; i < 3; ++i) {
            quint64 s = 0;
            if (t.allow(T0, &s)) {
                out << QStringLiteral("x%1").arg(s);
            }
        }
        QCOMPARE(out, QStringList{QStringLiteral("x0")});
    }
};

class LogEdgeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void enterOnlyOnFirstOfRun()
    {
        LogEdge e;
        QVERIFY(e.enter());
        QVERIFY(!e.enter());
        QVERIFY(!e.enter());
    }
    void leaveReportsCountAndDuration()
    {
        LogEdge e;
        quint64 count = 0;
        std::chrono::milliseconds d{0};
        QVERIFY(!e.leave(&count, &d)); // never entered
        const auto t0 = LogEdge::Clock::time_point(1h);
        QVERIFY(e.enter(t0));
        e.enter(t0 + 1s);
        e.enter(t0 + 2s);
        QVERIFY(e.leave(&count, &d, t0 + 5s));
        QCOMPARE(count, quint64(3));
        QCOMPARE(d, 5000ms);
        QVERIFY(!e.leave(&count, &d, t0 + 6s));
        QVERIFY(e.enter(t0 + 7s)); // a new run
    }
};

// T-K5b: farside.server is Info by default (no QT_LOGGING_RULES).
#include "krdp_logging.h"
class KrdpDefaultSeverityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void defaultIsInfo()
    {
        QVERIFY2(qEnvironmentVariableIsEmpty("QT_LOGGING_RULES"), "run without QT_LOGGING_RULES");
        QVERIFY(!KRDP().isDebugEnabled());
        QVERIFY(KRDP().isInfoEnabled());
        QVERIFY(KRDP().isWarningEnabled());
    }
};

int main(int argc, char **argv)
{
    qunsetenv("QT_LOGGING_RULES");
    qunsetenv("QT_LOGGING_CONF");
    QCoreApplication app(argc, argv);
    int rc = 0;
    {
        LogThrottleTest a;
        rc |= QTest::qExec(&a, argc, argv);
    }
    {
        LogEdgeTest b;
        rc |= QTest::qExec(&b, argc, argv);
    }
    {
        KrdpDefaultSeverityTest c;
        rc |= QTest::qExec(&c, argc, argv);
    }
    return rc;
}
#include "LogThrottleTest.moc"
