// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "StallDetector.h"

#include <QDebug>

#include <condition_variable>
#include <csignal>
#include <execinfo.h>
#include <mutex>
#include <unistd.h>

namespace KRdp
{

namespace
{
std::atomic<int> g_backtraceFd{2};

long long nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int backtraceSignal()
{
    return SIGRTMIN + 5;
}

// Runs on the (blocked) main thread. backtrace() was called once at start so libgcc is loaded.
void backtraceHandler(int)
{
    const int saved = errno;
    static const char header[] = "farside: main thread backtrace:\n";
    const int fd = g_backtraceFd.load(std::memory_order_relaxed);
    [[maybe_unused]] const auto written = ::write(fd, header, sizeof(header) - 1);
    void *frames[64];
    const int count = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, count, fd);
    errno = saved;
}

double seconds(long long ns)
{
    return double(ns) / 1e9;
}
}

StallDetector::StallDetector(QObject *parent)
    : StallDetector(Config{}, parent)
{
}

StallDetector::StallDetector(const Config &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
}

StallDetector::~StallDetector()
{
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
    if (m_signalInstalled) {
        struct sigaction restore{};
        restore.sa_handler = SIG_DFL;
        sigemptyset(&restore.sa_mask);
        ::sigaction(backtraceSignal(), &restore, nullptr);
    }
}

bool StallDetector::start()
{
    if (m_thread.joinable()) {
        return true;
    }
    m_mainThread = ::pthread_self();

    // C23: install only into a free slot. Anything else (a library's handler, our own from another
    // instance is released in the destructor) keeps the log lines but skips the backtrace.
    struct sigaction current{};
    if (::sigaction(backtraceSignal(), nullptr, &current) == 0 && current.sa_handler == SIG_DFL) {
        void *warm[1];
        ::backtrace(warm, 1); // load libgcc now, not inside the handler
        struct sigaction mine{};
        mine.sa_handler = backtraceHandler;
        sigemptyset(&mine.sa_mask);
        mine.sa_flags = SA_RESTART;
        m_signalInstalled = ::sigaction(backtraceSignal(), &mine, nullptr) == 0;
    } else {
        qWarning() << "main-thread stall detector: SIGRTMIN+5 already has a handler; backtraces disabled";
    }
    g_backtraceFd.store(m_config.backtraceFd);

    m_heartbeatNs = nowNs();
    m_timer.setInterval(m_config.tick);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        m_heartbeatNs.store(nowNs(), std::memory_order_release);
    });
    m_timer.start();

    m_thread = std::jthread([this](std::stop_token stop) {
        run(stop);
    });
    return true;
}

bool StallDetector::backtraceAvailable() const
{
    return m_signalInstalled;
}

void StallDetector::run(std::stop_token stop)
{
    const long long warnNs = std::chrono::duration_cast<std::chrono::nanoseconds>(m_config.warnAfter).count();
    const long long dumpNs = std::chrono::duration_cast<std::chrono::nanoseconds>(m_config.dumpAfter).count();
    std::mutex mutex;
    std::condition_variable_any wakeup;

    bool blocked = false;
    bool dumped = false;
    long long blockedStamp = 0;
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(mutex);
            wakeup.wait_for(lock, stop, m_config.poll, [] {
                return false;
            });
        }
        if (stop.stop_requested()) {
            break;
        }
        const long long stamp = m_heartbeatNs.load(std::memory_order_acquire);
        const long long now = nowNs();
        const long long gap = now - stamp;
        if (!blocked) {
            if (gap >= warnNs) {
                blocked = true;
                dumped = false;
                blockedStamp = stamp;
                qWarning().noquote() << QStringLiteral("main thread blocked for %1 s (phase: %2)").arg(seconds(gap), 0, 'f', 1).arg(QString::fromLatin1(g_stallPhase.load(std::memory_order_relaxed)));
            }
        } else if (stamp != blockedStamp) {
            // The main thread ran its timer again.
            qInfo().noquote() << QStringLiteral("main thread resumed after %1 s").arg(seconds(stamp - blockedStamp), 0, 'f', 1);
            blocked = false;
        } else if (!dumped && m_signalInstalled && gap >= dumpNs) {
            dumped = true;
            qWarning().noquote() << QStringLiteral("main thread still blocked after %1 s; backtrace follows on stderr").arg(seconds(gap), 0, 'f', 1);
            ::pthread_kill(m_mainThread, backtraceSignal());
        }
    }
}

}
