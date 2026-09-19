// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PhysicalOutputGuard.h"

#include <algorithm>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QProcess>
#include <QRegularExpression>
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
// The settle wait before a replace and after a restore's enable command;
// see waitForPhysical(). Panels that have been in standby make KWin remove
// and re-add the physical outputs when the virtual output is created and
// when they are enabled: DP-1 gone at +0.25 s, HDMI-A-1 at +1.1 s, both back
// by +3.5 s, nothing moves afterwards (hw standby batch v6c, run 4 table).
// The budget bounds the time to the first good read-back with ~2.5 s of
// margin; the stability window catches a read-back that lands before the
// removal starts (a match at +0.1 s is not the end of it).
constexpr int SettlePollMs = 500;
constexpr int SettleTimeoutMs = 6000;
constexpr int SettleStableMs = 1000;
// After the disable: how long an output that vanished under it (a churn
// that started late) is given to come back before the replace is failed.
// The re-add lands at +3.15 s / +3.45 s after the removal (same table), so
// 3 s would miss it by the poll grid when the churn starts at the disable.
constexpr int ReplaceSettleMs = 4000;
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

QVector<Output> PhysicalOutputGuard::readOutputs(QString *error)
{
    return current(error);
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

    // Settle before touching anything. When the panels have been in standby,
    // creating the virtual output (and the display wake at connect, when it
    // is on) makes KWin remove and re-add the physical outputs over ~3.5 s;
    // a disable issued in that gap is refused ("Output ... not found") and
    // a read-back cannot tell an absent output from a disabled one, so the
    // replace was reported applied while the panels came back lit (hw
    // standby batch v6c, finding F). Nothing has been mutated on a timeout,
    // so nothing is held: the caller continues as extend.
    if (!waitForPhysical(m_physical, SettleGoal::Present)) {
        qWarning() << "Physical outputs not all present within" << SettleTimeoutMs
                   << "ms of the virtual output appearing (the compositor is still re-adding them); not replacing them";
        return false;
    }

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
    if (!run(replaceArgs(m_physical, virtualOutputs, primaryVirtualName))) {
        qInfo() << "Combined replace did not run; applying in two steps";
        if (!applyReplaceInTwoSteps(virtualOutputs, primaryVirtualName)) {
            return false;
        }
    }
    if (!settleReplace(virtualOutputs, primaryVirtualName)) {
        return false;
    }
    qInfo() << "Physical outputs replaced by" << virtualOutputs.size() << "virtual output(s)";
    return true;
}

bool PhysicalOutputGuard::beginLayoutControl()
{
    if (m_held && m_layoutControl && hasSnapshot()) {
        // Already ours (a later apply of the same layout session): the
        // snapshot on file is the layout to restore, and it is not retaken.
        return true;
    }
    if (m_held) {
        // A configured virtual session's replace holds the outputs (or its
        // restore is still unverified). Its teardown restores its snapshot
        // and drops the state file; layout control must not build on top
        // of that, and must not adopt a snapshot it did not take.
        qWarning() << "Layout control refused: the physical outputs are held by a configured virtual-monitor session";
        return false;
    }
    if (!available()) {
        qWarning() << "kscreen-doctor not found; layout control cannot change the physical outputs";
        return false;
    }
    // snapshot() refuses while a previous restore is unverified (m_held
    // without a snapshot only happens after that), and when nothing is
    // enabled to come back to.
    if (!snapshot()) {
        return false;
    }
    // No state file, no hold: same rule as applyReplace().
    if (!writeStateFile()) {
        qWarning() << "Refusing layout control without crash recovery";
        m_physical.clear();
        return false;
    }
    m_held = true;
    m_layoutControl = true;
    qInfo() << "Layout control: physical outputs held (snapshot on file)";
    return true;
}

bool PhysicalOutputGuard::layoutControlHeld() const
{
    return m_held && m_layoutControl;
}

bool PhysicalOutputGuard::waitForPhysicalPresent() const
{
    if (!hasSnapshot()) {
        return false;
    }
    return waitForPhysical(m_physical, SettleGoal::Present);
}

QStringList PhysicalOutputGuard::dpmsOffOutputs() const
{
    QByteArray output;
    if (!run({u"--dpms"_s, u"show"_s}, &output)) {
        return {};
    }
    // One line per output: "dpms mode for screen DP-1: off".
    static const QRegularExpression line(uR"(dpms mode for screen (\S+): (\S+))"_s);
    QStringList off;
    for (const auto &text : QString::fromUtf8(output).split(u'\n', Qt::SkipEmptyParts)) {
        const auto match = line.match(text);
        if (!match.hasMatch()) {
            continue;
        }
        const QString name = match.captured(1);
        const bool enabled = std::any_of(m_physical.cbegin(), m_physical.cend(), [&name](const Output &candidate) {
            return candidate.name == name && candidate.enabled;
        });
        if (enabled && match.captured(2).compare(u"on"_s, Qt::CaseInsensitive) != 0) {
            off.push_back(name);
        }
    }
    return off;
}

bool PhysicalOutputGuard::applyArrangement(const QList<Arrangement> &entries)
{
    if (!m_held || !hasSnapshot()) {
        qWarning() << "applyArrangement without beginLayoutControl(); refusing to touch the outputs";
        return false;
    }
    if (entries.isEmpty()) {
        return true;
    }
    const bool anyEnabled = std::any_of(entries.cbegin(), entries.cend(), [](const Arrangement &entry) {
        return entry.enabled;
    });
    if (!anyEnabled) {
        // Never ask KWin for a desktop with no output; the planner's
        // sanitiser should have caught this (an empty layout has no primary).
        qWarning() << "applyArrangement would leave no output enabled; refusing";
        return false;
    }

    // Settle first, for the same reason applyReplace() does: creating the
    // virtual outputs this arrangement places can make KWin remove and
    // re-add the physical outputs (panels in standby), and a command that
    // names an absent output is refused whole.
    if (!waitForPhysical(m_physical, SettleGoal::Present)) {
        qWarning() << "Physical outputs not all present within" << SettleTimeoutMs << "ms (the compositor is still re-adding them); arrangement not applied";
        return false;
    }

    // kscreen-doctor exits 0 whether or not it applied anything; the
    // read-back decides. A refused combined change (priorities colliding
    // with outputs disabled in the same config) gets the two-step form.
    if (!run(arrangementArgs(entries))) {
        qInfo() << "Combined arrangement did not run; applying in two steps";
        if (!run(arrangementDisableArgs(entries)) || !run(arrangementEnableArgs(entries))) {
            return false;
        }
    }

    // Applied means every named output reads back present and as wanted,
    // and stays so for the stability window: enabling an output that was
    // off makes KWin remove and re-add it a few seconds later (finding A),
    // and the re-add can replay a stored arrangement. Present-but-wrong
    // after the first poll gets the two-step form re-issued, once.
    QElapsedTimer timer;
    timer.start();
    qint64 stableSince = -1;
    bool reissued = false;
    for (;;) {
        const auto now = current();
        const bool good = !now.isEmpty() && arrangementMatches(entries, now);
        if (good) {
            if (stableSince < 0) {
                stableSince = timer.elapsed();
            }
            if (timer.elapsed() - stableSince >= SettleStableMs) {
                qInfo() << "Arrangement applied:" << entries.size() << "outputs, after" << timer.elapsed() << "ms";
                return true;
            }
        } else {
            stableSince = -1;
            if (timer.elapsed() >= SettleTimeoutMs) {
                qWarning() << "Arrangement not verified within" << SettleTimeoutMs << "ms; the outputs may be in any state:" << now;
                return false;
            }
            if (!reissued && timer.elapsed() >= SettlePollMs && !now.isEmpty() && arrangementPresent(entries, now)) {
                reissued = true;
                qInfo() << "Outputs present but not as arranged; re-applying in two steps";
                if (!run(arrangementDisableArgs(entries)) || !run(arrangementEnableArgs(entries))) {
                    return false;
                }
            }
        }
        QThread::msleep(SettlePollMs);
    }
}

bool PhysicalOutputGuard::applyReplaceInTwoSteps(const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName)
{
    // kscreen may refuse the combined change (priorities colliding with
    // outputs that are being disabled in the same config). The virtual
    // outputs already exist and are enabled, so disabling the physical ones
    // first never leaves KWin without an enabled output, and the virtual
    // outputs are placed and prioritised only once nothing is left for them
    // to overlap.
    return run(replaceArgs(m_physical, {}, QString())) && run(positionArgs(virtualOutputs, primaryVirtualName, 1));
}

bool PhysicalOutputGuard::settleReplace(const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName)
{
    // Applied means: every physical output of the snapshot is present in
    // the read-back AND disabled. One that is absent may be a churn that
    // started after the pre-settle; give it ReplaceSettleMs to come back,
    // then require it disabled. One that is present but enabled - the
    // combined change was refused, or the re-add brought it back enabled
    // (KWin replays the stored arrangement, and the virtual output's place
    // with it) - gets the two-step form re-issued, once.
    QElapsedTimer timer;
    timer.start();
    bool reissued = false;
    for (;;) {
        const auto now = current();
        if (!now.isEmpty() && allPresentAndDisabled(m_physical, now)) {
            return true;
        }
        if (!now.isEmpty() && allPresent(m_physical, now)) {
            if (reissued) {
                qWarning() << "replace policy applied but a physical output is still enabled:" << now;
                return false;
            }
            reissued = true;
            qInfo() << "Physical outputs present but not all disabled after the replace; applying in two steps";
            if (!applyReplaceInTwoSteps(virtualOutputs, primaryVirtualName)) {
                return false;
            }
            // The two-step form's own read-back decides; no pause needed.
            continue;
        }
        if (timer.elapsed() >= ReplaceSettleMs) {
            qWarning() << "replace policy applied but a physical output has not come back within" << ReplaceSettleMs
                       << "ms to be verified disabled (compositor churn); the physical outputs may be in any state:" << now;
            return false;
        }
        QThread::msleep(SettlePollMs);
    }
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

bool PhysicalOutputGuard::parkIfPhysicalEnabled()
{
    if (!hasParkPlacements()) {
        return false;
    }
    // Only worth trying while a physical output is enabled: with none, KWin
    // pins the lone enabled output to (0,0) and the move would not stick.
    const auto now = current();
    const bool anyPhysicalEnabled = std::any_of(now.cbegin(), now.cend(), [](const Output &o) {
        return !isVirtual(o.name) && o.enabled;
    });
    if (!anyPhysicalEnabled) {
        return false;
    }
    if (!parkVirtualOutputs()) {
        qWarning() << "Could not park the virtual output beside the physical desktop after an unverified restore; KWin's placement stands";
        return false;
    }
    qInfo() << "Virtual output parked beside the physical desktop after an unverified restore, so KWin does not record the overlap";
    return true;
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

bool PhysicalOutputGuard::waitForPhysical(const QVector<Output> &physical, SettleGoal goal)
{
    // Blocking on purpose: the callers (the replace, teardown, takeover, the
    // retry, the start-up restore) must not run the event loop here, or
    // cursor samples, input and a connection's destruction would re-enter
    // them mid-change.
    QElapsedTimer timer;
    timer.start();
    qint64 stableSince = -1;
    bool reapplied = false;
    bool churned = false;
    for (;;) {
        const auto now = current();
        const bool good = !now.isEmpty() && (goal == SettleGoal::Present ? allPresent(physical, now) : matches(physical, now));
        if (good) {
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
            // Only when restoring: before a replace, present is all that
            // is asked for.
            if (goal == SettleGoal::Matching && !reapplied && timer.elapsed() >= SettlePollMs && !now.isEmpty() && allPresent(physical, now)) {
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

PhysicalOutputGuard::RestoreOutcome PhysicalOutputGuard::restoreSnapshot(const QVector<Output> &physical)
{
    RestoreOutcome outcome;
    if (physical.isEmpty()) {
        outcome.presentVerified = true;
        outcome.verified = true;
        return outcome;
    }
    // kscreen-doctor refuses the whole command when any named output is
    // absent ("Output ... not found", nothing applied), so a snapshotted
    // monitor that dropped HPD mid-session (power button, KVM, cable) would
    // keep the present one dark too. Restore what is there; report the rest
    // and stay held for it. A failed read-back falls back to the full
    // command: there is nothing to intersect with.
    const auto now = current();
    outcome.present = now.isEmpty() ? physical : presentSubset(physical, now);
    outcome.missing = now.isEmpty() ? QVector<Output>() : missingSubset(physical, now);
    if (outcome.present.isEmpty()) {
        qWarning() << "None of the snapshotted physical outputs is connected:" << names(outcome.missing) << "- nothing to restore right now";
        return outcome;
    }
    if (!outcome.missing.isEmpty()) {
        qWarning() << "Snapshotted physical output(s) not connected:" << names(outcome.missing) << "- restoring the rest:" << names(outcome.present);
    }
    if (!run(restoreArgs(outcome.present))) {
        return outcome;
    }
    // The enable command is accepted at once, but a panel that was in
    // standby makes KWin remove the output and re-add it a few seconds
    // later, replaying whatever arrangement it has stored for the set that
    // reappears. Verifying on the first read-back races that (hw batch v6,
    // findings A/B); wait until the snapshot is back and stays back.
    outcome.presentVerified = waitForPhysical(outcome.present, SettleGoal::Matching);
    outcome.verified = outcome.presentVerified && outcome.missing.isEmpty();
    return outcome;
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
    const auto outcome = restoreSnapshot(m_physical);
    if (outcome.verified) {
        m_held = false;
        m_layoutControl = false;
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
    // snapshotted within the settle budget (or the command failed), or a
    // snapshotted output is not connected. The manual command names only
    // what is connected - kscreen-doctor would refuse the rest - and the
    // missing outputs get their own command for when they are back.
    const QString recovery = u"kscreen-doctor "_s + restoreArgs(outcome.present.isEmpty() ? m_physical : outcome.present).join(u' ');
    QString why = u"not back within %1 ms"_s.arg(SettleTimeoutMs);
    if (!outcome.missing.isEmpty()) {
        const QString presentNames = names(outcome.present).join(u", ");
        const QString missingNames = names(outcome.missing).join(u", ");
        if (outcome.present.isEmpty()) {
            why = u"none of %1 is connected"_s.arg(missingNames);
        } else if (outcome.presentVerified) {
            why = u"%1 restored, but %2 not connected"_s.arg(presentNames, missingNames);
        } else {
            why = u"%1 not back within %2 ms and %3 not connected"_s.arg(presentNames).arg(SettleTimeoutMs).arg(missingNames);
        }
        why += u"; once reconnected run: kscreen-doctor "_s + restoreArgs(outcome.missing).join(u' ');
    }
    if (m_retrying) {
        qCritical().noquote() << u"Physical outputs still not restored after the retry (%1). Run:"_s.arg(why) << recovery << "(or krdpserver --restore-outputs)";
        Q_EMIT restored(false);
    } else if (!m_retryTimer.isActive()) {
        qCritical().noquote() << u"Physical outputs NOT restored (%1); retrying in"_s.arg(why) << RestoreRetryMs << "ms. Manual recovery:" << recovery;
        m_retryTimer.start();
    } else {
        qCritical().noquote() << u"Physical outputs NOT restored (%1); a retry is already pending. Manual recovery:"_s.arg(why) << recovery;
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
    const auto outcome = restoreSnapshot(physical);
    if (outcome.verified) {
        file.remove();
        qInfo() << "Physical outputs restored from the state file";
        return true;
    }
    if (outcome.missing.isEmpty()) {
        // Everything is connected but did not settle: worth another try at
        // the next start.
        qCritical().noquote() << "Restore from" << file.fileName() << "failed. Run: kscreen-doctor" << restoreArgs(physical).join(u' ');
        return false;
    }
    // Some or all of the snapshotted outputs are not connected (unplugged,
    // a KVM, a different monitor set): the rest cannot be restored until
    // they are back, and what is connected may not even verify without
    // them (a lone enabled output is renumbered and pinned to 0,0 by KWin,
    // so it can never match its snapshot entry). Set the file aside
    // whatever the connected subset did, rather than pay a blocking restore
    // for outputs that are not there at every later start; the log says
    // how to finish by hand.
    const QString original = file.fileName();
    const QString stale = original + u".stale"_s;
    QFile::remove(stale);
    const bool kept = file.rename(stale);
    const QString presentNames = names(outcome.present).join(u", ");
    const QString missingNames = names(outcome.missing).join(u", ");
    const QString what = outcome.present.isEmpty() ? u"none of its outputs is connected"_s
        : outcome.presentVerified                  ? u"%1 restored, not connected: %2"_s.arg(presentNames, missingNames)
                                                   : u"%1 enabled but not verified as snapshotted, not connected: %2"_s.arg(presentNames, missingNames);
    qCritical().noquote() << "State file" << original << (kept ? u"set aside as %1"_s.arg(stale) : u"could not be renamed"_s) << ":" << what << "- when" << missingNames
                          << "is back, run: kscreen-doctor" << restoreArgs(physical).join(u' ')
                          << "(or rename the file back and run krdpserver --restore-outputs; last resort: rm" << original << "once the monitors are right)";
    return false;
}
