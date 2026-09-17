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

    bool applyReplace(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
    bool positionOutputs(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
    bool reconcileExtend();
    /**
     * Put the physical outputs back as snapshotted, verified by read-back.
     * A verified restore drops the snapshot and the state file.
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
    /** Write the snapshot (and this PID) for restoreFromStateFile(); false, logged, on failure. */
    bool writeStateFile() const;
    /** Whether \a pid is a live process other than this one (a running krdpserver, if /proc can tell). */
    static bool ownerAlive(qint64 pid);

    QVector<KRdp::OutputSnapshot::Output> m_physical;
    bool m_held = false;
    // The single retry a failed restore() schedules; stopped by a verified
    // restore so it cannot fire (and re-emit restored()) afterwards.
    QTimer m_retryTimer;
    bool m_retrying = false;
};
