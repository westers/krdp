// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-055 K6.2 (T-K6b): a blocked main thread is reported once, its backtrace is written once,
// and recovery is reported once. Thresholds are shortened through the Config.

#include "StallDetector.h"

#include <QTest>
#include <QThread>

#include <csignal>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace
{
std::mutex g_logMutex;
QStringList g_log;
QtMessageHandler g_previous = nullptr;

void capture(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    {
        std::lock_guard lock(g_logMutex);
        g_log << message;
    }
    if (g_previous) {
        g_previous(type, context, message);
    }
}

QStringList lines()
{
    std::lock_guard lock(g_logMutex);
    return g_log;
}

int count(const QString &needle)
{
    int found = 0;
    for (const auto &line : lines()) {
        if (line.contains(needle)) {
            ++found;
        }
    }
    return found;
}
}

// Exported, never inlined, never tail-called: its symbol must appear in the main-thread backtrace.
extern "C" __attribute__((noinline, used, visibility("default"))) void krdpTestBlockMainThread(int milliseconds)
{
    QThread::msleep(ulong(milliseconds));
    asm volatile("" ::: "memory");
}

class StallDetectorTest : public QObject
{
    Q_OBJECT
private:
    static QByteArray drain(int fd)
    {
        QByteArray out;
        char buffer[4096];
        for (;;) {
            const auto got = ::read(fd, buffer, sizeof(buffer));
            if (got <= 0) {
                break;
            }
            out.append(buffer, qsizetype(got));
        }
        return out;
    }

private Q_SLOTS:
    void initTestCase() { g_previous = qInstallMessageHandler(capture); }
    void init()
    {
        std::lock_guard lock(g_logMutex);
        g_log.clear();
    }
    void cleanupTestCase() { qInstallMessageHandler(g_previous); }

    void aShortBlockIsNotReported()
    {
        using namespace std::chrono_literals;
        KRdp::StallDetector detector(KRdp::StallDetector::Config{.warnAfter = 2000ms, .dumpAfter = 4000ms, .tick = 100ms, .poll = 50ms, .backtraceFd = 2});
        QVERIFY(detector.start());
        QTest::qWait(300);
        krdpTestBlockMainThread(600);
        QTest::qWait(600);
        QCOMPARE(count(QStringLiteral("main thread blocked")), 0);
        QCOMPARE(count(QStringLiteral("resumed after")), 0);
    }

    void aBlockedMainThreadIsReportedOnceWithItsBacktraceAndRecovery()
    {
        using namespace std::chrono_literals;
        int fds[2];
        QCOMPARE(::pipe2(fds, O_CLOEXEC | O_NONBLOCK), 0);
        {
            KRdp::StallDetector detector(KRdp::StallDetector::Config{.warnAfter = 2000ms, .dumpAfter = 4000ms, .tick = 100ms, .poll = 50ms, .backtraceFd = fds[1]});
            QVERIFY(detector.start());
            QVERIFY(detector.backtraceAvailable());
            QTest::qWait(300); // let the heartbeat run first
            {
                KRdp::StallDetector::Phase phase("test-phase");
                krdpTestBlockMainThread(6000);
            }
            QTest::qWait(1500); // back in the event loop: the stamps resume
        }
        const auto backtrace = drain(fds[0]);
        ::close(fds[0]);
        ::close(fds[1]);
        QCOMPARE(count(QStringLiteral("main thread blocked")), 1);
        QVERIFY2(count(QStringLiteral("phase: test-phase")) == 1, qPrintable(lines().join(QLatin1Char('\n'))));
        QCOMPARE(count(QStringLiteral("resumed after")), 1);
        QVERIFY2(backtrace.contains("krdpTestBlockMainThread"), backtrace.constData());
        QCOMPARE(backtrace.count("krdpTestBlockMainThread"), 1); // once per stall
    }

    void aTakenSignalSlotSkipsOnlyTheBacktrace()
    {
        using namespace std::chrono_literals;
        struct sigaction mine{};
        mine.sa_handler = [](int) {};
        sigemptyset(&mine.sa_mask);
        struct sigaction old{};
        QCOMPARE(::sigaction(SIGRTMIN + 5, &mine, &old), 0);
        {
            KRdp::StallDetector detector(KRdp::StallDetector::Config{.warnAfter = 500ms, .dumpAfter = 1000ms, .tick = 100ms, .poll = 50ms, .backtraceFd = 2});
            QVERIFY(detector.start());
            QVERIFY(!detector.backtraceAvailable());
            QTest::qWait(300);
            krdpTestBlockMainThread(1800);
            QTest::qWait(500);
        }
        struct sigaction now{};
        QCOMPARE(::sigaction(SIGRTMIN + 5, nullptr, &now), 0);
        QVERIFY(now.sa_handler == mine.sa_handler); // left exactly as it was found
        ::sigaction(SIGRTMIN + 5, &old, nullptr);
        QCOMPARE(count(QStringLiteral("main thread blocked")), 1);
    }
};

QTEST_MAIN(StallDetectorTest)
#include "StallDetectorTest.moc"
