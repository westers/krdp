// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "StallPhase.h"
#include "krdp_export.h"

#include <QObject>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <pthread.h>
#include <thread>

namespace KRdp
{

/**
 * K6.2: main-thread stall detector.
 *
 * A main-thread QTimer stamps an atomic heartbeat; a helper thread watches it. After `warnAfter`
 * without a stamp it logs once "main thread blocked for N s (phase: X)"; after `dumpAfter` it
 * sends a real-time signal to the main thread whose handler writes the main thread's backtrace
 * to `backtraceFd`, once per stall. When the stamps resume it logs once "resumed after N s".
 *
 * Construct and start() it on the main thread. The signal is SIGRTMIN+5; if a handler is
 * already installed for it, the backtrace is skipped (the log lines remain).
 */
class KRDP_EXPORT StallDetector : public QObject
{
    Q_OBJECT
public:
    struct Config {
        std::chrono::milliseconds warnAfter{5000};
        std::chrono::milliseconds dumpAfter{45000};
        std::chrono::milliseconds tick{1000}; ///< main-thread stamp interval
        std::chrono::milliseconds poll{500}; ///< helper-thread check interval
        int backtraceFd = 2;
    };

    explicit StallDetector(QObject *parent = nullptr);
    explicit StallDetector(const Config &config, QObject *parent = nullptr);
    ~StallDetector() override;

    /// Returns false (and stays inert) when the signal slot is taken.
    bool start();

    /// What the main thread is doing, for the "blocked" line. The string must be a literal.
    static void setPhase(const char *phase)
    {
        g_stallPhase.store(phase, std::memory_order_relaxed);
    }

    using Phase = StallPhase;

    /// True when the backtrace signal handler could be installed.
    bool backtraceAvailable() const;

private:
    void run(std::stop_token stop);

    Config m_config;
    QTimer m_timer;
    std::atomic<long long> m_heartbeatNs{0};
    pthread_t m_mainThread{};
    bool m_signalInstalled = false;
    std::jthread m_thread;
};

}
