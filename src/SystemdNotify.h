// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "krdp_export.h"

#include <QObject>
#include <QTimer>

#include <chrono>

namespace KRdp
{

/**
 * K6.1: systemd watchdog heartbeat.
 *
 * The ping comes from a QTimer on the thread that owns the object (the main thread), so a main
 * thread that stops running its event loop stops pinging and `WatchdogSec=` fires. Pings are
 * sent only when systemd armed the watchdog (WATCHDOG_USEC in the environment, and WATCHDOG_PID
 * absent or ours); otherwise everything here is a no-op, as it is without libsystemd.
 */
class KRDP_EXPORT SystemdNotify : public QObject
{
    Q_OBJECT
public:
    explicit SystemdNotify(QObject *parent = nullptr);

    /// The watchdog period systemd configured for this process, or zero when it is not armed.
    static std::chrono::microseconds watchdogPeriod();

    /// Ping interval for a watchdog period: min(10 s, period / 3).
    static std::chrono::milliseconds pingInterval(std::chrono::microseconds period);

    /// Send one WATCHDOG=1 now if the watchdog is armed. Safe before QApplication exists.
    static bool ping();

    /// Ping now and then every pingInterval(). Returns false when the watchdog is not armed.
    bool start();

private:
    QTimer m_timer;
};

}
