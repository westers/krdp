// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "SystemdNotify.h"

#ifdef KRDP_HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include <algorithm>

namespace KRdp
{

SystemdNotify::SystemdNotify(QObject *parent)
    : QObject(parent)
{
    m_timer.setTimerType(Qt::CoarseTimer);
}

std::chrono::microseconds SystemdNotify::watchdogPeriod()
{
#ifdef KRDP_HAVE_SYSTEMD
    uint64_t usec = 0;
    // 0: keep the variables, so later checks and child processes still see them.
    if (sd_watchdog_enabled(0, &usec) > 0 && usec > 0) {
        return std::chrono::microseconds(static_cast<std::chrono::microseconds::rep>(usec));
    }
#endif
    return std::chrono::microseconds::zero();
}

std::chrono::milliseconds SystemdNotify::pingInterval(std::chrono::microseconds period)
{
    using namespace std::chrono_literals;
    const auto third = std::chrono::duration_cast<std::chrono::milliseconds>(period / 3);
    return std::clamp(third, 1ms, 10000ms);
}

bool SystemdNotify::ping()
{
#ifdef KRDP_HAVE_SYSTEMD
    if (watchdogPeriod() == std::chrono::microseconds::zero()) {
        return false;
    }
    return sd_notify(0, "WATCHDOG=1") > 0;
#else
    return false;
#endif
}

bool SystemdNotify::start()
{
    const auto period = watchdogPeriod();
    if (period == std::chrono::microseconds::zero()) {
        return false;
    }
    ping();
    m_timer.setInterval(pingInterval(period));
    connect(&m_timer, &QTimer::timeout, this, [] {
        SystemdNotify::ping();
    });
    m_timer.start();
    return true;
}

}
