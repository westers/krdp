// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "OutputSnapshot.h"

/**
 * Switches Steve's physical monitors off while a virtual-monitor session runs
 * (`VirtualMonitorPolicy=replace`) and puts them back exactly as they were,
 * using `kscreen-doctor` as a child process (OPT-041).
 *
 * Every mutation is one kscreen-doctor invocation followed by a read-back and
 * comparison against the snapshot, so a failure is a logged fact, not a
 * silent wrong layout. applyReplace() writes the snapshot to a state file
 * (`QStandardPaths::StateLocation`, i.e. `$XDG_STATE_HOME/<app name>/physical-outputs.json`)
 * just before its first mutation, so it exists exactly while the physical
 * outputs are held; restoreFromStateFile() replays it after a crash (at
 * server start and via `krdpserver --restore-outputs`).
 *
 * All calls are synchronous on the main thread (a kscreen-doctor run takes
 * well under a second); the only asynchronous part is the single retry a
 * failed restore schedules.
 */
class PhysicalOutputGuard : public QObject
{
    Q_OBJECT
public:
    explicit PhysicalOutputGuard(QObject *parent = nullptr);
    ~PhysicalOutputGuard() override;

    static QString stateFilePath();
    static bool restoreFromStateFile();

    bool available() const;
    /**
     * Read the physical outputs as they are now, as the layout to restore.
     * Refused (false, logged) while a previous replace is not verifiably
     * restored yet, and when no physical output is enabled; neither is a
     * layout the user wants back. Writes no state file: applyReplace() does.
     */
    bool snapshot();
    bool hasSnapshot() const;
    /**
     * Whether the physical outputs were, or may have been, changed by
     * applyReplace() (or by a failed reconcileExtend() re-apply) and not
     * verifiably restored since: the state in which release() restores and
     * a console takeover has something to give back.
     */
    bool held() const;
    QVector<KRdp::OutputSnapshot::Output> physicalOutputs() const;

    /**
     * Disable the physical outputs in favour of \a virtualOutputs. Waits
     * first for every snapshotted physical output to be present and to stay
     * present (creating the virtual output can make KWin remove and re-add
     * them), and counts as applied only when every one of them reads back
     * present AND disabled. False without a mutation (not held) when the
     * outputs never settled; false while held when the disable was issued
     * but could not be verified, in which case release() restores.
     */
    bool applyReplace(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
    /**
     * Where the virtual outputs belong while the physical ones are enabled
     * (beside them, at the extend anchor). Remembered so that restore()
     * parks them itself once the physical outputs are verifiably back -
     * including a retry that succeeds after the caller has moved on - and
     * so teardown and takeover park through one path. Cleared by the
     * session that set them before its virtual outputs go away.
     */
    void setParkPlacements(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
    void clearParkPlacements();
    bool hasParkPlacements() const;
    /**
     * Move the remembered virtual outputs to their extend places now,
     * behind the physical outputs in priority, verified by read-back. Only
     * sticks while a physical output is enabled: KWin pins a lone enabled
     * output to (0,0).
     */
    bool parkVirtualOutputs();
    bool reconcileExtend();
    /**
     * Put the physical outputs back as snapshotted and wait for them to
     * settle (a panel that was in standby makes KWin remove and re-add the
     * output a few seconds after it is enabled), verified by read-back once
     * the snapshot has held still. A verified restore drops the snapshot and
     * the state file, then parks the remembered virtual outputs. Blocks the
     * caller for the settle wait (bounded, a few seconds); an unverified
     * restore keeps the outputs held and schedules one retry.
     */
    bool restore();
    /**
     * End of a virtual session: restore() if the physical outputs were (or
     * may have been) touched by applyReplace(), otherwise just drop the
     * snapshot and its state file without running kscreen-doctor, so an
     * extend session leaves whatever the user changed meanwhile alone.
     */
    bool release();

Q_SIGNALS:
    void restored(bool verified);

private:
    static bool run(const QStringList &args, QByteArray *output = nullptr);
    static QVector<KRdp::OutputSnapshot::Output> current(QString *error = nullptr);
    static bool restoreSnapshot(const QVector<KRdp::OutputSnapshot::Output> &physical);
    /** What waitForPhysical() waits for. */
    enum class SettleGoal {
        /** Every physical output of the snapshot is present, whatever its state (before a replace). */
        Present,
        /** Every physical output of the snapshot is present and as snapshotted (after a restore). */
        Matching,
    };
    /** Poll until \a physical meets \a goal and has for a stability window; false on timeout. */
    static bool waitForPhysical(const QVector<KRdp::OutputSnapshot::Output> &physical, SettleGoal goal);
    /** The disable-then-place form of the replace, for when kscreen refuses the combined change. */
    bool applyReplaceInTwoSteps(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
    /** Poll until every snapshotted physical output reads back present and disabled; re-issues the two-step form once. */
    bool settleReplace(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
    static bool positionOutputs(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
    /** Write the snapshot (and this PID) for restoreFromStateFile(); false, logged, on failure. */
    bool writeStateFile() const;
    /** Whether \a pid is a live process other than this one (a running krdpserver, if /proc can tell). */
    static bool ownerAlive(qint64 pid);

    QVector<KRdp::OutputSnapshot::Output> m_physical;
    bool m_held = false;
    // See setParkPlacements().
    QVector<KRdp::OutputSnapshot::Placement> m_parkPlacements;
    QString m_parkPrimary;
    // The single retry a failed restore() schedules; stopped by a verified
    // restore so it cannot fire (and re-emit restored()) afterwards.
    QTimer m_retryTimer;
    bool m_retrying = false;
};
