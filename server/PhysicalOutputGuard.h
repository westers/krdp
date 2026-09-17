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
 * silent wrong layout. The snapshot is also written to a state file
 * (`QStandardPaths::StateLocation`, i.e. `$XDG_STATE_HOME/<app name>/physical-outputs.json`)
 * before anything is disabled; restoreFromStateFile() replays it after a
 * crash (at server start and via `krdpserver --restore-outputs`).
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
    bool snapshot();
    bool hasSnapshot() const;
    QVector<KRdp::OutputSnapshot::Output> physicalOutputs() const;

    bool applyReplace(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
    bool positionOutputs(const QVector<KRdp::OutputSnapshot::Placement> &virtualOutputs, const QString &primaryVirtualName);
    bool reconcileExtend();
    bool restore();

Q_SIGNALS:
    void restored(bool verified);

private:
    static bool run(const QStringList &args, QByteArray *output = nullptr);
    static QVector<KRdp::OutputSnapshot::Output> current(QString *error = nullptr);
    static bool restoreSnapshot(const QVector<KRdp::OutputSnapshot::Output> &physical);

    QVector<KRdp::OutputSnapshot::Output> m_physical;
    bool m_held = false;
    // The single retry a failed restore() schedules; stopped by a verified
    // restore so it cannot fire (and re-emit restored()) afterwards.
    QTimer m_retryTimer;
    bool m_retrying = false;
};
