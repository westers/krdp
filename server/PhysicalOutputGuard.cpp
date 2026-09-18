// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PhysicalOutputGuard.h"

#include <algorithm>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <unistd.h>

using namespace KRdp::OutputSnapshot;
using namespace Qt::StringLiterals;

namespace
{
constexpr int KscreenTimeoutMs = 5000;
constexpr int RestoreRetryMs = 2000;
// The settle wait after a restore's enable command; see waitForPhysical().
// Panels that were in standby make KWin remove and re-add the physical
// outputs ~1-3.5 s after they are enabled (hw batch v6, runs 1/4/5); the
// budget covers that with margin, and the stability window catches a
// read-back that happened to land before the removal (run 4).
constexpr int SettlePollMs = 500;
constexpr int SettleTimeoutMs = 6000;
constexpr int SettleStableMs = 1500;
const QString KscreenDoctor = u"kscreen-doctor"_s;
// Steve's layout, for the messages that have no snapshot to derive it from.
const QString FallbackRecovery =
    u"kscreen-doctor output.DP-1.enable output.HDMI-A-1.enable output.DP-1.position.0,0 output.HDMI-A-1.position.2560,0 output.DP-1.priority.1 output.HDMI-A-1.priority.2"_s;
}

PhysicalOutputGuard::PhysicalOutputGuard(QObject *parent)
    : QObject(parent)
{
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(RestoreRetryMs);
    connect(&m_retryTimer, &QTimer::timeout, this, [this]() {
        m_retrying = true;
        restore();
        m_retrying = false;
    });
}

PhysicalOutputGuard::~PhysicalOutputGuard()
{
    if (m_held) {
        qWarning() << "Physical outputs still replaced at shutdown; restoring";
        restore();
    }
}

QString PhysicalOutputGuard::stateFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::StateLocation) + u"/physical-outputs.json"_s;
}

bool PhysicalOutputGuard::available() const
{
    return !QStandardPaths::findExecutable(KscreenDoctor).isEmpty();
}

bool PhysicalOutputGuard::run(const QStringList &args, QByteArray *output)
{
    QProcess process;
    process.setProgram(KscreenDoctor);
    process.setArguments(args);
    process.start();
    if (!process.waitForFinished(KscreenTimeoutMs)) {
        if (process.error() == QProcess::FailedToStart) {
            qWarning() << "kscreen-doctor" << args.join(u' ') << "could not be started:" << process.errorString();
            return false;
        }
        process.kill();
        process.waitForFinished(1000);
        qWarning() << "kscreen-doctor" << args.join(u' ') << "timed out";
        return false;
    }
    const QByteArray errors = process.readAllStandardError().trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qWarning() << "kscreen-doctor" << args.join(u' ') << "failed:" << errors;
        return false;
    }
    // Exit 0 says nothing about whether the change was applied; the refusal
    // reason, if any, is only ever on stderr.
    if (!errors.isEmpty()) {
        qDebug() << "kscreen-doctor" << args.join(u' ') << "said:" << errors;
    }
    if (output) {
        *output = process.readAllStandardOutput();
    }
    return true;
}

QVector<Output> PhysicalOutputGuard::current(QString *error)
{
    QByteArray json;
    if (!run({u"-j"_s}, &json)) {
        if (error) {
            *error = u"kscreen-doctor -j failed"_s;
        }
        return {};
    }
    return parse(json, error);
}

bool PhysicalOutputGuard::snapshot()
{
    if (m_held) {
        // The physical outputs are still (or may still be) replaced by an
        // earlier session whose restore did not verify; what -j reports now
        // is that state, not the user's layout, and snapshotting it would
        // make "restore" mean "put the monitors back off". Checked before
        // anything is cleared: the snapshot being held is the only thing
        // the pending retry, the tray action and the teardown can restore
        // from (Task 6c review, Critical 1).
        qWarning() << "Cannot snapshot the physical outputs: the previous replace is not verifiably restored yet";
        return false;
    }
    // Never keep a previous session's list around: a failed read here must
    // leave hasSnapshot() false, or a replace would run on stale outputs
    // with no state file behind it.
    m_physical.clear();
    QString error;
    const auto outputs = current(&error);
    if (outputs.isEmpty()) {
        qWarning() << "Cannot snapshot the physical outputs:" << error;
        return false;
    }
    m_physical = physicalOnly(outputs);
    if (m_physical.isEmpty()) {
        qWarning() << "No physical outputs to snapshot";
        return false;
    }
    const QRect enabled = enabledUnion(m_physical);
    if (!enabled.isValid()) {
        // Every physical output is off (a DPMS-off console does not do this;
        // a leftover replace from another instance does). Restoring this
        // would be a no-op and the extend anchor derived from it is garbage.
        qWarning() << "Cannot snapshot the physical outputs: none of them is enabled";
        m_physical.clear();
        return false;
    }
    // The state file is written by applyReplace(), just before the first
    // mutation, so a crash during an extend session (which never touches the
    // physical outputs) has nothing to restore at the next start.
    qInfo() << "Physical outputs snapshot:" << m_physical.size() << "outputs, enabled union" << enabled;
    return true;
}

bool PhysicalOutputGuard::writeStateFile() const
{
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::StateLocation));
    QFile file(stateFilePath());
    const QByteArray json = toStateJson(m_physical, qint64(getpid()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(json) != json.size() || !file.flush()) {
        qWarning() << "Cannot write" << file.fileName() << ":" << file.errorString();
        return false;
    }
    return true;
}

bool PhysicalOutputGuard::hasSnapshot() const
{
    return !m_physical.isEmpty();
}

bool PhysicalOutputGuard::held() const
{
    return m_held;
}

QVector<Output> PhysicalOutputGuard::physicalOutputs() const
{
    return m_physical;
}

bool PhysicalOutputGuard::applyReplace(const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName)
{
    if (!hasSnapshot()) {
        qWarning() << "applyReplace without a snapshot; refusing to touch the physical outputs";
        return false;
    }
    if (virtualOutputs.isEmpty()) {
        // Nothing would be left enabled; never ask for that, whatever KWin
        // would make of it.
        qWarning() << "applyReplace with no virtual output; refusing to disable the physical outputs";
        return false;
    }
    const auto anyPhysicalEnabled = [](const QVector<Output> &outputs) {
        return std::any_of(outputs.cbegin(), outputs.cend(), [](const Output &o) {
            return !isVirtual(o.name) && o.enabled;
        });
    };

    // No state file, no replace: without crash recovery a crash here could
    // leave the monitors off for good, so the caller stays on extend.
    if (!writeStateFile()) {
        qWarning() << "Refusing to replace the physical outputs without crash recovery";
        return false;
    }

    // From here on the physical outputs may have been touched, so the
    // destructor restores even if this call ends up reporting failure.
    m_held = true;

    // kscreen-doctor exits 0 whether or not it applied anything, so the
    // read-back is what tells a refused change from an applied one.
    const bool combinedRan = run(replaceArgs(m_physical, virtualOutputs, primaryVirtualName));
    QVector<Output> after = combinedRan ? current() : QVector<Output>();
    if (!combinedRan || after.isEmpty() || anyPhysicalEnabled(after)) {
        // kscreen may refuse the combined change (priorities colliding with
        // outputs that are being disabled in the same config); apply it as
        // two steps instead. The virtual outputs already exist and are
        // enabled, so disabling the physical ones first never leaves KWin
        // without an enabled output, and the virtual outputs are placed and
        // prioritised only once nothing is left for them to overlap.
        if (!combinedRan) {
            qInfo() << "Combined replace did not run; applying in two steps";
        } else if (after.isEmpty()) {
            qInfo() << "Combined replace applied but the read-back failed; applying in two steps";
        } else {
            qInfo() << "Combined replace refused; applying in two steps";
        }
        if (!run(replaceArgs(m_physical, {}, QString())) || !run(positionArgs(virtualOutputs, primaryVirtualName, 1))) {
            return false;
        }
        after = current();
    }

    if (after.isEmpty()) {
        qWarning() << "replace policy applied but the read-back failed; the physical outputs may be in any state";
        return false;
    }
    if (anyPhysicalEnabled(after)) {
        qWarning() << "replace policy applied but a physical output is still enabled:" << after;
        return false;
    }
    qInfo() << "Physical outputs replaced by" << virtualOutputs.size() << "virtual output(s)";
    return true;
}

void PhysicalOutputGuard::setParkPlacements(const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName)
{
    m_parkPlacements = virtualOutputs;
    m_parkPrimary = primaryVirtualName;
}

void PhysicalOutputGuard::clearParkPlacements()
{
    m_parkPlacements.clear();
    m_parkPrimary.clear();
}

bool PhysicalOutputGuard::hasParkPlacements() const
{
    return !m_parkPlacements.isEmpty();
}

bool PhysicalOutputGuard::parkVirtualOutputs()
{
    return positionOutputs(m_parkPlacements, m_parkPrimary);
}

bool PhysicalOutputGuard::positionOutputs(const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName)
{
    if (virtualOutputs.isEmpty()) {
        return true;
    }
    // Virtual outputs sit behind the physical ones in priority (extend keeps
    // Steve's primary as the primary). Counted from the live layout rather
    // than the snapshot: this also runs after a verified restore has dropped
    // the snapshot, and a virtual output must never end up at priority 1 by
    // accident.
    const auto before = current();
    const int physicalCount = int(std::count_if(before.cbegin(), before.cend(), [](const Output &o) {
        return !isVirtual(o.name);
    }));
    if (physicalCount == 0) {
        qWarning() << "Cannot place the virtual outputs: no physical output in the read-back";
        return false;
    }
    if (!run(positionArgs(virtualOutputs, primaryVirtualName, physicalCount + 1))) {
        return false;
    }
    // Read back like every other mutation: kscreen-doctor's exit code does
    // not say whether KWin took the placement (it pins a lone enabled output
    // to 0,0, for one).
    const auto now = current();
    for (const auto &placement : virtualOutputs) {
        const auto it = std::find_if(now.cbegin(), now.cend(), [&placement](const Output &o) {
            return o.name == placement.name;
        });
        if (it == now.cend() || it->position != placement.position) {
            qWarning() << "Virtual output" << placement.name << "is not at" << placement.position << "after positioning:"
                       << (it == now.cend() ? QStringLiteral("not present") : QStringLiteral("at %1,%2").arg(it->position.x()).arg(it->position.y()));
            return false;
        }
    }
    return true;
}

bool PhysicalOutputGuard::reconcileExtend()
{
    if (!hasSnapshot()) {
        return false;
    }
    const auto now = current();
    if (now.isEmpty()) {
        return false;
    }
    if (matches(m_physical, now)) {
        return true;
    }
    // KWin replayed a remembered arrangement for this output set (it does that
    // when the same set of outputs reappears); put the physical ones back.
    qInfo() << "Physical outputs drifted after the virtual output appeared; re-applying the snapshot";
    if (run(restoreArgs(m_physical)) && matches(m_physical, current())) {
        return true;
    }
    // The physical outputs were touched and are not verifiably as the user
    // had them: from here on release() restores instead of just dropping the
    // snapshot, and a crash gets the same recovery as a replace.
    m_held = true;
    if (!writeStateFile()) {
        qWarning() << "No crash recovery for the drifted physical outputs; manual recovery: kscreen-doctor" << restoreArgs(m_physical).join(u' ');
    }
    qWarning() << "Physical outputs could not be re-applied after the drift; they will be restored when the session ends";
    return false;
}

bool PhysicalOutputGuard::waitForPhysical(const QVector<Output> &physical)
{
    // Blocking on purpose: the callers (teardown, takeover, the retry, the
    // start-up restore) must not run the event loop here, or cursor samples,
    // input and a connection's destruction would re-enter them mid-restore.
    QElapsedTimer timer;
    timer.start();
    qint64 stableSince = -1;
    bool reapplied = false;
    bool churned = false;
    for (;;) {
        const auto now = current();
        if (!now.isEmpty() && matches(physical, now)) {
            if (stableSince < 0) {
                stableSince = timer.elapsed();
            }
            if (timer.elapsed() - stableSince >= SettleStableMs) {
                if (churned) {
                    qInfo() << "Physical outputs settled after" << timer.elapsed() << "ms";
                }
                return true;
            }
        } else {
            churned = true;
            stableSince = -1;
            if (timer.elapsed() >= SettleTimeoutMs) {
                return false;
            }
            // Back, but not as snapshotted: the re-add replayed a stored
            // arrangement (the virtual output on top, say). Nothing is still
            // missing, so the command applies now; once, so a layout KWin
            // keeps overriding ends in a timeout rather than a fight, and
            // not on the first read-back, which the enable itself answers.
            if (!reapplied && timer.elapsed() >= SettlePollMs && !now.isEmpty() && allPresent(physical, now)) {
                reapplied = true;
                qInfo() << "Physical outputs are back but not as snapshotted; re-applying the restore";
                if (!run(restoreArgs(physical))) {
                    return false;
                }
            }
        }
        QThread::msleep(SettlePollMs);
    }
}

bool PhysicalOutputGuard::restoreSnapshot(const QVector<Output> &physical)
{
    if (physical.isEmpty()) {
        return true;
    }
    if (!run(restoreArgs(physical))) {
        return false;
    }
    // The enable command is accepted at once, but a panel that was in
    // standby makes KWin remove the output and re-add it a few seconds
    // later, replaying whatever arrangement it has stored for the set that
    // reappears. Verifying on the first read-back races that (hw batch v6,
    // findings A/B); wait until the snapshot is back and stays back.
    return waitForPhysical(physical);
}

bool PhysicalOutputGuard::restore()
{
    if (m_held && !hasSnapshot()) {
        // Should be unreachable (snapshot() refuses while held), but the
        // consequence of getting here would be monitors that stay off while
        // every recovery path reports success, so never report success on
        // the flag alone: fall back to the state file applyReplace() wrote.
        QFile file(stateFilePath());
        if (file.open(QIODevice::ReadOnly)) {
            m_physical = fromStateJson(file.readAll());
        }
        if (!hasSnapshot()) {
            qCritical().noquote() << "Physical outputs are held but the snapshot is gone and" << stateFilePath() << "is unreadable; run:" << FallbackRecovery
                                  << "(Steve's layout) or krdpserver --restore-outputs";
            Q_EMIT restored(false);
            return false;
        }
        qWarning() << "Physical outputs held with no snapshot in memory; restoring from" << stateFilePath();
    }
    if (!hasSnapshot()) {
        return true;
    }
    const bool verified = restoreSnapshot(m_physical);
    if (verified) {
        m_held = false;
        // A retry still pending from an earlier failure has nothing left to
        // do and must not fire restored() a second time.
        m_retryTimer.stop();
        // The snapshot is spent: the next session takes its own, and nothing
        // may restore this one again over a layout the user changed since.
        m_physical.clear();
        QFile::remove(stateFilePath());
        qInfo() << "Physical outputs restored";
        // Now, after the outputs have settled, the virtual outputs go to
        // their extend places: KWin's re-add replays the recorded
        // arrangement and drops them at their replace-time place over the
        // physical origin, so a park before the settle is undone (finding
        // A). Here rather than in the caller so a retry that succeeds after
        // the takeover has moved on parks as well. Best effort: the physical
        // outputs are restored whether or not the park sticks.
        if (hasParkPlacements() && !parkVirtualOutputs()) {
            qWarning() << "Could not park the virtual output beside the physical desktop; KWin's placement stands";
        }
        Q_EMIT restored(true);
        return true;
    }

    // Still held: the enable ran but the outputs did not come back as
    // snapshotted within the settle budget (or the command failed).
    const QString recovery = u"kscreen-doctor "_s + restoreArgs(m_physical).join(u' ');
    if (m_retrying) {
        qCritical().noquote() << "Physical outputs still not restored after the retry (not back within" << SettleTimeoutMs << "ms). Run:" << recovery
                              << "(or krdpserver --restore-outputs)";
        Q_EMIT restored(false);
    } else if (!m_retryTimer.isActive()) {
        qCritical().noquote() << "Physical outputs NOT restored (not back within" << SettleTimeoutMs << "ms); retrying in" << RestoreRetryMs
                              << "ms. Manual recovery:" << recovery;
        m_retryTimer.start();
    } else {
        qCritical().noquote() << "Physical outputs NOT restored; a retry is already pending. Manual recovery:" << recovery;
    }
    return false;
}

bool PhysicalOutputGuard::release()
{
    if (m_held) {
        return restore();
    }
    if (hasSnapshot()) {
        // No state file to remove: only applyReplace() (or a failed
        // reconcileExtend(), which holds) writes one.
        m_physical.clear();
        qInfo() << "Physical outputs were never replaced; snapshot dropped, layout left as it is";
    }
    return true;
}

bool PhysicalOutputGuard::ownerAlive(qint64 pid)
{
    if (pid <= 0 || pid == qint64(getpid())) {
        return false;
    }
    if (::kill(pid_t(pid), 0) != 0 && errno != EPERM) {
        return false;
    }
    // The PID may have been reused since the crash; when /proc can name the
    // process, only a krdpserver counts.
    QFile comm(u"/proc/%1/comm"_s.arg(pid));
    if (comm.open(QIODevice::ReadOnly)) {
        const QByteArray name = comm.readAll().trimmed();
        return name.isEmpty() || name.startsWith("krdp");
    }
    return true;
}

bool PhysicalOutputGuard::restoreFromStateFile()
{
    QFile file(stateFilePath());
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot read" << file.fileName() << ":" << file.errorString();
        return false;
    }
    qint64 ownerPid = 0;
    const auto physical = fromStateJson(file.readAll(), &ownerPid);
    if (physical.isEmpty()) {
        // The file only exists because a session had replaced the outputs and
        // did not get to restore them, so its being unreadable is not a
        // reason to forget that: keep it under another name and say so.
        const QString original = file.fileName();
        const QString corrupt = original + u".corrupt"_s;
        QFile::remove(corrupt);
        const bool kept = file.rename(corrupt);
        qCritical().noquote() << "State file" << original << "is unreadable" << (kept ? u"(kept as %1)"_s.arg(corrupt) : u"(and could not be renamed)"_s)
                              << "- the physical outputs may still be off. Check: kscreen-doctor -o ; if so run:" << FallbackRecovery << "(Steve's layout)";
        return false;
    }
    if (ownerAlive(ownerPid)) {
        // A live session holds the outputs replaced on purpose; restoring
        // under it would re-enable the physical outputs over the virtual one
        // and delete its crash recovery. Its own teardown restores.
        qInfo().noquote() << "State file" << file.fileName() << "belongs to running krdpserver PID" << ownerPid << "(a virtual-monitor session is live); not restoring."
                          << "Stop that server first if the outputs really are stuck, or run: kscreen-doctor" << restoreArgs(physical).join(u' ');
        return false;
    }
    qWarning() << "A previous krdpserver" << (ownerPid > 0 ? u"(PID %1, gone)"_s.arg(ownerPid) : u"(unknown PID)"_s)
               << "left the physical outputs replaced (state file present); restoring" << physical.size() << "outputs";
    if (!restoreSnapshot(physical)) {
        qCritical().noquote() << "Restore from" << file.fileName() << "failed. Run: kscreen-doctor" << restoreArgs(physical).join(u' ');
        return false;
    }
    file.remove();
    qInfo() << "Physical outputs restored from the state file";
    return true;
}
