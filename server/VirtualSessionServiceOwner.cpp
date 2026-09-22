// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionServiceOwner.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace KRdp {
namespace {
void childSignals()
{
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_UNBLOCK, &mask, nullptr)) _exit(127);
}
void prepare(QProcess &process)
{
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(QStringLiteral("/"));
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
    process.setChildProcessModifier(childSignals);
    process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
}
}
VirtualSessionServiceOwner::VirtualSessionServiceOwner(Checks checks, std::function<void(int)> finished, QObject *parent)
    : QObject(parent), m_checks(std::move(checks)), m_finished(std::move(finished))
{
    prepare(m_keeper); prepare(m_desktop);
    m_desktop.setProcessChannelMode(QProcess::ForwardedChannels);
    connect(&m_keeper, &QProcess::readyReadStandardOutput, this, &VirtualSessionServiceOwner::readyRead);
    connect(&m_keeper, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (m_phase == Phase::ClosingKeeper) {
            m_failed |= code != 0 || status != QProcess::NormalExit;
            complete();
        } else if (m_phase != Phase::Finished) {
            // Unexpected keeper loss is never a normal desktop logout.
            stopDesktop(true);
        }
    });
    connect(&m_keeper, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { m_failed = true; complete(); }
    });
    connect(&m_desktop, &QProcess::started, this, [this] {
        closeCredential();
        if (m_phase == Phase::StoppingDesktop) m_desktop.terminate();
    });
    connect(&m_desktop, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        stopDesktop(code != 0 || status != QProcess::NormalExit);
    });
    connect(&m_desktop, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { closeCredential(); stopDesktop(true); }
    });
    connect(&m_tick, &QTimer::timeout, this, &VirtualSessionServiceOwner::tick);
    connect(&m_monitor, &QTimer::timeout, this, [this] {
        if (m_phase == Phase::Running && (m_keeper.state() != QProcess::Running
            || !m_checks.keeper(pid_t(m_keeper.processId()), m_login))) stopDesktop(true);
    });
}
VirtualSessionServiceOwner::~VirtualSessionServiceOwner()
{
    // QProcess destruction may emit finished while waiting for a live child.
    // Other members are destroyed first, so callbacks must be disconnected now.
    disconnect(&m_keeper, nullptr, this, nullptr);
    disconnect(&m_desktop, nullptr, this, nullptr);
    m_tick.stop(); m_monitor.stop();
    if (m_phase != Phase::Idle && m_phase != Phase::Finished)
        qWarning("Virtual service owner destroyed before ordered completion; cleanup unproven");
    closeCredential();
    // Normal ownership must reach Finished before destruction. Emergency
    // process death still requires systemd and logind crash reconciliation.
}
void VirtualSessionServiceOwner::closeCredential()
{
    if (m_credential >= 0) { close(m_credential); m_credential = -1; }
}
bool VirtualSessionServiceOwner::start(const VirtualSessionJournal::Record &record, const QString &keeper,
    const VirtualSessionServicePlan &desktop, Timing timing)
{
    if (m_phase != Phase::Idle || !record.valid() || keeper.isEmpty() || desktop.credential != record.token
        || !m_checks.keeper || !m_checks.descendantsGone || !m_checks.signalDescendants || !m_checks.emergency
        || timing.opening <= 0 || timing.graceful <= 0 || timing.closing <= 0 || timing.poll <= 0 || timing.monitor <= 0
        || timing.emergency <= timing.graceful) return false;
    m_record = record; m_plan = desktop; m_timing = timing;
    m_phase = Phase::Opening; m_clock.start(); m_tick.start(timing.poll);
    m_keeper.start(keeper, {QStringLiteral("--session"), record.session, QStringLiteral("--launch"), record.launch,
        QStringLiteral("--parent"), QString::number(getpid())});
    return true;
}
void VirtualSessionServiceOwner::readyRead()
{
    // Never accumulate an unbounded helper stream; only one <=1024-byte line.
    const auto bytes = m_keeper.read(1025);
    if (bytes.isEmpty()) return;
    if (m_phase != Phase::Opening) { stopDesktop(true); return; }
    m_ready += bytes;
    if (m_ready.size() > 1024 || m_keeper.bytesAvailable() > 0) { m_failed = true; closeKeeper(); return; }
    if (!m_ready.contains('\n')) return;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(m_ready, &error);
    const auto object = document.object();
    const auto login = object.value(QStringLiteral("login"));
    const auto leader = object.value(QStringLiteral("leader"));
    const auto uid = object.value(QStringLiteral("uid"));
    if (!m_ready.endsWith('\n') || m_ready.count('\n') != 1 || error.error != QJsonParseError::NoError
        || !document.isObject() || object.size() != 7
        || object.value(QStringLiteral("v")) != QJsonValue(1)
        || object.value(QStringLiteral("type")) != QJsonValue(QStringLiteral("ready"))
        || object.value(QStringLiteral("session")) != QJsonValue(m_record.session)
        || object.value(QStringLiteral("launch")) != QJsonValue(m_record.launch)
        || !uid.isDouble() || uid.toDouble() != double(m_record.uid)
        || !leader.isDouble() || leader.toDouble() != double(m_keeper.processId()) || m_keeper.processId() <= 1
        || !login.isString() || !QRegularExpression(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z")).match(login.toString()).hasMatch()
        || !m_checks.keeper(pid_t(m_keeper.processId()), login.toString())
        || m_checks.descendantsGone() != std::optional<bool>(true)) {
        m_failed = true; closeKeeper(); return;
    }
    m_login = login.toString();
    // The bounded authoritative check can itself consume time. A timer cannot
    // fire during it, so recheck before allowing any desktop process to start.
    if (m_clock.elapsed() >= m_timing.opening) { m_failed = true; closeKeeper(); return; }
    if (m_stopRequested) { closeKeeper(); return; }
    startDesktop();
}
void VirtualSessionServiceOwner::startDesktop()
{
    // Prefill before fork: guardian reads nonblocking immediately on exec.
    // No credential in argv, environment, disk file or QProcess output buffer.
    int fds[2];
    if (pipe2(fds, O_CLOEXEC)) { m_failed = true; closeKeeper(); return; }
    ssize_t count;
    do { count = write(fds[1], m_plan.credential.constData(), 32); } while (count < 0 && errno == EINTR);
    close(fds[1]);
    if (count != 32) { close(fds[0]); m_failed = true; closeKeeper(); return; }
    m_credential = fds[0];
    m_desktop.setStandardInputFile(QProcess::nullDevice());
    m_desktop.setChildProcessModifier([fd = m_credential] {
        childSignals();
        if (dup2(fd, STDIN_FILENO) < 0 || fcntl(STDIN_FILENO, F_SETFD, 0)) _exit(127);
    });
    m_desktopAttempted = true; m_phase = Phase::Running;
    m_monitor.start(m_timing.monitor);
    m_desktop.start(m_plan.program, m_plan.arguments);
}
void VirtualSessionServiceOwner::stop()
{
    m_stopRequested = true;
    // During PAM startup do not race migration with service-group signaling.
    // Await Ready or the bounded opening failure. No desktop exists yet.
    if (m_phase == Phase::Running) stopDesktop(false);
}
void VirtualSessionServiceOwner::stopDesktop(bool failed)
{
    m_failed |= failed;
    if (m_phase == Phase::Finished || m_phase == Phase::ClosingKeeper) return;
    if (!m_desktopAttempted) { closeKeeper(); return; }
    if (m_phase != Phase::StoppingDesktop) {
        m_monitor.stop(); m_phase = Phase::StoppingDesktop; m_clock.restart();
        if (m_desktop.state() == QProcess::Running) m_desktop.terminate();
    }
    tick();
}
void VirtualSessionServiceOwner::emergency()
{
    if (m_emergencyRequested) return;
    m_failed = true; m_emergencyRequested = true;
    m_checks.emergency();
}
void VirtualSessionServiceOwner::closeKeeper()
{
    if (m_phase == Phase::ClosingKeeper || m_phase == Phase::Finished) return;
    m_monitor.stop(); m_phase = Phase::ClosingKeeper; m_clock.restart();
    if (m_keeper.state() == QProcess::NotRunning) { complete(); return; }
    if (m_keeper.write("stop\n", 5) != 5) m_failed = true;
    m_keeper.closeWriteChannel();
}
void VirtualSessionServiceOwner::tick()
{
    if (m_phase == Phase::Opening && m_clock.elapsed() >= m_timing.opening) {
        m_failed = true; closeKeeper();
    } else if (m_phase == Phase::StoppingDesktop) {
        // Guardian exit is not enough: namespace descendants must also be gone.
        if (m_desktop.state() == QProcess::NotRunning && m_checks.descendantsGone() == std::optional<bool>(true)) {
            closeKeeper();
        } else if (m_clock.elapsed() >= m_timing.graceful) {
            m_failed = true;
            m_checks.signalDescendants(SIGKILL);
            // Unknown containment keeps PAM alive until actual emergency
            // service teardown, never reported as ordered/clean completion.
            if (m_clock.elapsed() >= m_timing.emergency) emergency();
        }
    } else if (m_phase == Phase::ClosingKeeper && m_clock.elapsed() >= m_timing.closing) {
        m_failed = true;
        if (m_keeper.state() != QProcess::NotRunning) m_keeper.kill();
        // SIGKILL submission is not proof of exit (e.g. uninterruptible I/O).
        if (m_clock.elapsed() >= qint64(m_timing.closing) + 5000) emergency();
    }
}
void VirtualSessionServiceOwner::complete()
{
    if (m_phase == Phase::Finished) return;
    m_tick.stop(); m_monitor.stop(); m_phase = Phase::Finished;
    closeCredential();
    if (m_finished) m_finished(m_failed ? 1 : 0);
}
}
