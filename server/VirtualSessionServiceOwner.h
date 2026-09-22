// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionServicePlan.h"
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>

namespace KRdp {
// Lifetime owner in the dedicated root service. Only the exec'd keeper opens
// PAM; the desktop is its sibling. Hooks are trusted executor checks, not RPC.
class VirtualSessionServiceOwner : public QObject {
public:
    struct Checks {
        std::function<bool(pid_t, const QString &)> keeper;
        std::function<std::optional<bool>()> descendantsGone;
        std::function<bool(int)> signalDescendants;
        // Begin failed service teardown, never report successful PAM cleanup.
        std::function<void()> emergency;
    };
    struct Timing { int opening = 35000, graceful = 8000, closing = 12000, poll = 200, monitor = 2000, emergency = 20000; };
    enum class Phase { Idle, Opening, Running, StoppingDesktop, ClosingKeeper, Finished };
    VirtualSessionServiceOwner(Checks checks, std::function<void(int)> finished, QObject *parent = nullptr);
    ~VirtualSessionServiceOwner() override;
    bool start(const VirtualSessionJournal::Record &record, const QString &keeper,
        const VirtualSessionServicePlan &desktop, Timing timing);
    void stop();
    Phase phase() const { return m_phase; }
private:
    void readyRead();
    void startDesktop();
    void stopDesktop(bool failed);
    void closeKeeper();
    void tick();
    void complete();
    void emergency();
    void closeCredential();
    Checks m_checks;
    std::function<void(int)> m_finished;
    VirtualSessionJournal::Record m_record;
    VirtualSessionServicePlan m_plan;
    Timing m_timing;
    QProcess m_keeper, m_desktop;
    QTimer m_tick, m_monitor;
    QElapsedTimer m_clock;
    Phase m_phase = Phase::Idle;
    QByteArray m_ready;
    QString m_login;
    bool m_failed = false, m_stopRequested = false, m_desktopAttempted = false;
    bool m_emergencyRequested = false;
    int m_credential = -1;
};
}
