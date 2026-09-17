// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PhysicalOutputGuard.h"

#include <algorithm>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

using namespace KRdp::OutputSnapshot;
using namespace Qt::StringLiterals;

namespace
{
constexpr int KscreenTimeoutMs = 5000;
constexpr int RestoreRetryMs = 2000;
const QString KscreenDoctor = u"kscreen-doctor"_s;
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

    // No state file, no snapshot: a replace without crash recovery could
    // leave the monitors off for good, so the caller falls back to extend.
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::StateLocation));
    QFile file(stateFilePath());
    const QByteArray json = toJson(m_physical);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(json) != json.size() || !file.flush()) {
        qWarning() << "Cannot write" << file.fileName() << ":" << file.errorString() << "- refusing to snapshot without crash recovery";
        m_physical.clear();
        return false;
    }
    qInfo() << "Physical outputs snapshot:" << m_physical.size() << "outputs, enabled union" << enabledUnion(m_physical);
    return true;
}

bool PhysicalOutputGuard::hasSnapshot() const
{
    return !m_physical.isEmpty();
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

    // From here on the physical outputs may have been touched, so the
    // destructor restores even if this call ends up reporting failure.
    m_held = true;

    // kscreen-doctor exits 0 whether or not it applied anything, so the
    // read-back is what tells a refused change from an applied one.
    QVector<Output> after;
    if (run(replaceArgs(m_physical, virtualOutputs, primaryVirtualName))) {
        after = current();
    }
    if (after.isEmpty() || anyPhysicalEnabled(after)) {
        // kscreen may refuse the combined change (priorities colliding with
        // outputs that are being disabled in the same config); apply it as
        // two steps instead, virtual outputs first so an enabled output always exists.
        qInfo() << "Combined replace refused; applying in two steps";
        if (!run(positionArgs(virtualOutputs, primaryVirtualName, 1)) || !run(replaceArgs(m_physical, {}, QString()))) {
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

bool PhysicalOutputGuard::positionOutputs(const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName)
{
    if (virtualOutputs.isEmpty()) {
        return true;
    }
    // Virtual outputs sit behind the physical ones in priority (extend keeps
    // Steve's primary as the primary).
    const int firstPriority = int(m_physical.size()) + 1;
    if (!run(positionArgs(virtualOutputs, primaryVirtualName, firstPriority))) {
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
    return run(restoreArgs(m_physical)) && matches(m_physical, current());
}

bool PhysicalOutputGuard::restoreSnapshot(const QVector<Output> &physical)
{
    if (physical.isEmpty()) {
        return true;
    }
    if (!run(restoreArgs(physical))) {
        return false;
    }
    return matches(physical, current());
}

bool PhysicalOutputGuard::restore()
{
    if (!hasSnapshot()) {
        return true;
    }
    const bool verified = restoreSnapshot(m_physical);
    if (verified) {
        m_held = false;
        // A retry still pending from an earlier failure has nothing left to
        // do and must not fire restored() a second time.
        m_retryTimer.stop();
        QFile::remove(stateFilePath());
        qInfo() << "Physical outputs restored";
        Q_EMIT restored(true);
        return true;
    }

    const QString recovery = u"kscreen-doctor "_s + restoreArgs(m_physical).join(u' ');
    if (m_retrying) {
        qCritical().noquote() << "Physical outputs still not restored after the retry. Run:" << recovery << "(or krdpserver --restore-outputs)";
        Q_EMIT restored(false);
    } else if (!m_retryTimer.isActive()) {
        qCritical().noquote() << "Physical outputs NOT restored; retrying in" << RestoreRetryMs << "ms. Manual recovery:" << recovery;
        m_retryTimer.start();
    } else {
        qCritical().noquote() << "Physical outputs NOT restored; a retry is already pending. Manual recovery:" << recovery;
    }
    return false;
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
    const auto physical = fromJson(file.readAll());
    if (physical.isEmpty()) {
        // The file only exists because a session had replaced the outputs and
        // did not get to restore them, so its being unreadable is not a
        // reason to forget that: keep it under another name and say so.
        const QString original = file.fileName();
        const QString corrupt = original + u".corrupt"_s;
        QFile::remove(corrupt);
        const bool kept = file.rename(corrupt);
        qCritical().noquote() << "State file" << original << "is unreadable" << (kept ? u"(kept as %1)"_s.arg(corrupt) : u"(and could not be renamed)"_s)
                              << "- the physical outputs may still be off. Check: kscreen-doctor -o ; if so run: kscreen-doctor"
                              << "output.DP-1.enable output.HDMI-A-1.enable output.DP-1.position.0,0 output.HDMI-A-1.position.2560,0"
                              << "output.DP-1.priority.1 output.HDMI-A-1.priority.2 (Steve's layout)";
        return false;
    }
    qWarning() << "A previous krdpserver left the physical outputs replaced (state file present); restoring" << physical.size() << "outputs";
    if (!restoreSnapshot(physical)) {
        qCritical().noquote() << "Restore from" << file.fileName() << "failed. Run: kscreen-doctor" << restoreArgs(physical).join(u' ');
        return false;
    }
    file.remove();
    qInfo() << "Physical outputs restored from the state file";
    return true;
}
