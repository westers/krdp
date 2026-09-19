// SPDX-FileCopyrightText: 2026 KDE Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QObject>

/**
 * Wakes the local display when the first RDP session starts streaming and
 * keeps it awake until the last one stops.
 *
 * KWin does not render outputs that are DPMS-off, so a screencast of a
 * sleeping monitor delivers no frames and the remote client only ever sees a
 * blank surface. On the first active session this asks PowerDevil to wake the
 * display and takes an org.freedesktop.ScreenSaver inhibition; the inhibition
 * is released once no sessions remain (or on destruction).
 *
 * Every D-Bus call is asynchronous and failures are only logged, so this can
 * never block or abort a session.
 */
class DisplayWakeGuard : public QObject
{
    Q_OBJECT
public:
    explicit DisplayWakeGuard(QObject *parent = nullptr);
    ~DisplayWakeGuard() override;

    /**
     * Whether waking/inhibiting is performed at all. Turning this off while
     * an inhibition is held releases it immediately.
     */
    void setEnabled(bool enabled);

    /**
     * Mark one session as streaming. Wakes and inhibits on the 0 -> 1 edge.
     */
    void acquire();
    /**
     * Mark one session as no longer streaming. Releases on the 1 -> 0 edge.
     */
    void release();
    /**
     * Wake the display now, whatever setEnabled() says and without counting
     * as a session: `KRDPCTL` layout control asks for this before it creates
     * a virtual output, because an output created while a panel is in DPMS
     * standby is created into KWin's remove-and-re-add churn (OPT-044;
     * `WakeDisplayOnConnect` only governs configured-mode clients). The
     * inhibition is not taken here; the sessions that follow take it.
     */
    void wakeNow();

Q_SIGNALS:
    /**
     * Emitted once the wake request has been answered, whether PowerDevil or
     * the ScreenSaver fallback handled it and whether or not it succeeded.
     */
    void displayWakeRequested(bool succeeded);

private:
    void wakeDisplay();
    void simulateUserActivity();
    void inhibit();
    void uninhibit();

    bool m_enabled = true;
    int m_activeSessions = 0;
    bool m_inhibitPending = false;
    std::optional<uint> m_inhibitCookie;
};
