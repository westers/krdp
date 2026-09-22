// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionSupervisor.h"
#include <QDir>
#include <algorithm>

namespace KRdp
{
VirtualSessionSupervisor::VirtualSessionSupervisor(LaunchFactory factory, int readyTimeoutMs,
                                                 int stopTimeoutMs, QObject *parent)
    : QObject(parent), m_factory(std::move(factory)), m_readyTimeoutMs(std::max(1, readyTimeoutMs)),
      m_stopTimeoutMs(std::max(1, stopTimeoutMs))
{
}

VirtualSessionSupervisor::~VirtualSessionSupervisor()
{
    // Explicit supervisor destruction, never the disconnect path. A production
    // launch must use a namespace leader so grandchildren cannot leak out.
    for (auto &[id, value] : m_runtimes) {
        value->deadline.stop();
        value->poll.stop();
        if (value->guardian) continue; // detach supervision, never stop retained applications
        value->process.disconnect(this);
        if (value->process.state() != QProcess::NotRunning) {
            value->process.kill();
            value->process.waitForFinished(1000);
        }
    }
}

std::optional<VirtualSessionSupervisor::Handle> VirtualSessionSupervisor::adopt(const VirtualSessionGuardianClient::Identity &identity)
{
    const auto handle = m_registry.reserveRetained(identity.uid, identity.session);
    if (!handle) return {};
    auto value = std::make_unique<Runtime>();
    value->handle = *handle;
    value->identity = identity;
    value->guardian = std::make_unique<VirtualSessionGuardianClient>();
    auto *r = value.get();
    m_runtimes.emplace(handle->id, std::move(value));
    r->deadline.setSingleShot(true);
    r->poll.setSingleShot(true);
    connect(&r->deadline, &QTimer::timeout, this, [this, r] { guardianUnavailable(*r); });
    connect(&r->poll, &QTimer::timeout, this, [this, r] { queryGuardian(*r); });
    // QLocalSocket may emit failure from inside connectToServer. A user's
    // teardown callback must not delete it while Qt Network is on that stack.
    // Resolve the full handle at delivery so queued replies cannot use an erased
    // runtime or affect a successor after forget/recovery.
    connect(r->guardian.get(), &VirtualSessionGuardianClient::failed, this, [this, handle = *handle](const auto &) {
        if (auto *current = runtime(handle)) guardianUnavailable(*current);
    }, Qt::QueuedConnection);
    connect(r->guardian.get(), &VirtualSessionGuardianClient::received, this, [this, handle = *handle](const auto &phase, bool running) {
        if (auto *current = runtime(handle)) guardianReply(*current, phase, running);
    }, Qt::QueuedConnection);
    r->deadline.start(m_readyTimeoutMs);
    queryGuardian(*r);
    return handle;
}

void VirtualSessionSupervisor::guardianUnavailable(Runtime &r)
{
    if (r.failed || r.terminalConfirmed) return;
    r.failed = true;
    r.observedRunning = false;
    r.deadline.stop();
    r.poll.stop();
    m_registry.unavailable(r.handle);
    notifyUnavailable(r.handle);
    // No Stop, kill, replacement or profile deletion on supervision failure.
}

void VirtualSessionSupervisor::notifyUnavailable(const Handle &handle)
{
    // Both callback storage and argument must survive callback-driven teardown.
    const auto callback = m_unavailable;
    const auto snapshot = handle;
    if (callback) callback(snapshot);
}

void VirtualSessionSupervisor::queryGuardian(Runtime &r)
{
    if (r.failed || r.terminalConfirmed || r.guardian->busy()) return;
    const bool stop = r.stopping && !r.stopSent;
    if (stop) r.stopSent = true;
    // connectToServer may synchronously fail and invoke a callback that destroys
    // this runtime/supervisor. No runtime access after an accepted request.
    if (!r.guardian->request(r.identity, stop ? VirtualSessionGuardianClient::Operation::Stop : VirtualSessionGuardianClient::Operation::Status)) {
        guardianUnavailable(r);
        return;
    }
}

void VirtualSessionSupervisor::guardianReply(Runtime &r, const QString &phase, bool running)
{
    if (r.failed || r.terminalConfirmed) return;
    if (!running && (phase == QStringLiteral("exited") || phase == QStringLiteral("failed"))) {
        r.terminalConfirmed = true;
        r.observedRunning = false;
        r.poll.stop();
        r.deadline.stop();
        m_registry.exited(r.handle);
        notifyUnavailable(r.handle);
        return;
    }
    const bool notifyStopping = phase == QStringLiteral("stopping") && !r.stopping;
    if (notifyStopping) {
        r.stopping = true;
        r.stopSent = true;
        m_registry.stop(r.identity.uid, r.handle.id);
        r.deadline.start(m_stopTimeoutMs + 1000);
    }
    r.observedRunning = running && phase == QStringLiteral("running");
    if (!r.stopping && r.observedRunning && r.captureObserved) {
        m_registry.ready(r.handle);
        r.deadline.stop();
    }
    if (r.stopping && !r.stopSent) queryGuardian(r);
    else r.poll.start(r.stopping ? 100 : 1000);
    // External callbacks may destroy this supervisor. Do not touch r afterward.
    if (notifyStopping) notifyUnavailable(r.handle);
}

std::optional<VirtualSessionSupervisor::Handle> VirtualSessionSupervisor::create(quint32 uid)
{
    auto handle = m_registry.create(uid);
    if (handle) {
        launch(uid, *handle);
    }
    return handle;
}

std::optional<VirtualSessionSupervisor::Handle> VirtualSessionSupervisor::recreate(quint32 uid, const QString &id)
{
    const auto existing = m_runtimes.find(id);
    // A lost guardian may still own live apps. Never replace it via the old
    // process factory, even if the registry is reporting Failed.
    if (existing != m_runtimes.end() && existing->second->guardian) return {};
    auto handle = m_registry.recreate(uid, id);
    if (handle) {
        launch(uid, *handle);
    }
    return handle;
}

void VirtualSessionSupervisor::launch(quint32 uid, const Handle &handle)
{
    const auto spec = m_factory ? m_factory(uid, handle) : std::nullopt;
    if (!spec || !QDir::isAbsolutePath(spec->program) || spec->environment.inheritsFromParent()) {
        m_registry.exited(handle);
        return;
    }
    auto value = std::make_unique<Runtime>();
    value->handle = handle;
    auto *r = value.get();
    m_runtimes.insert_or_assign(handle.id, std::move(value));
    r->process.setProgram(spec->program);
    r->process.setArguments(spec->arguments);
    r->process.setProcessEnvironment(spec->environment);
    if (spec->childSetup) {
        r->process.setChildProcessModifier(spec->childSetup);
    }
    // Drain unlimited child logging without accumulating it in broker memory.
    // Production launcher owns per-session diagnostic files inside its runtime.
    r->process.setStandardOutputFile(QProcess::nullDevice());
    r->process.setStandardErrorFile(QProcess::nullDevice());
    r->process.setStandardInputFile(QProcess::nullDevice());
    r->deadline.setSingleShot(true);
    connect(&r->deadline, &QTimer::timeout, this, [this, r] {
        r->failed = true;
        terminate(*r);
    });
    connect(&r->process, &QProcess::started, this, [r] {
        if (r->stopping) {
            r->process.terminate();
        }
    });
    connect(&r->process, &QProcess::errorOccurred, this, [this, r](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            r->deadline.stop();
            m_registry.exited(r->handle);
        }
    });
    connect(&r->process, &QProcess::finished, this, [this, r](int, QProcess::ExitStatus) {
        r->deadline.stop();
        m_registry.exited(r->handle);
    });
    r->deadline.start(m_readyTimeoutMs);
    r->process.start();
}

VirtualSessionSupervisor::Runtime *VirtualSessionSupervisor::runtime(const Handle &handle)
{
    auto it = m_runtimes.find(handle.id);
    if (it == m_runtimes.end()) {
        return nullptr;
    }
    auto *r = it->second.get();
    return r->handle.manager == handle.manager && r->handle.generation == handle.generation ? r : nullptr;
}

bool VirtualSessionSupervisor::captureReady(const Handle &handle)
{
    auto *r = runtime(handle);
    if (r && r->guardian) {
        if (r->stopping || r->failed || r->terminalConfirmed) return false;
        r->captureObserved = true;
        if (!r->observedRunning || !m_registry.ready(handle)) return false;
        r->deadline.stop();
        return true;
    }
    if (!r || r->stopping || r->failed || r->process.state() != QProcess::Running || !m_registry.ready(handle)) {
        return false;
    }
    r->deadline.stop();
    return true;
}

void VirtualSessionSupervisor::captureUnavailable(const Handle &handle)
{
    auto *r = runtime(handle);
    if (r && r->guardian && !r->stopping) guardianUnavailable(*r);
}

std::optional<VirtualSessionSupervisor::Handle> VirtualSessionSupervisor::attach(quint32 uid, const QString &id, quint64 client)
{
    const auto it = m_runtimes.find(id);
    if (it == m_runtimes.end() || it->second->stopping || it->second->failed
        || (it->second->guardian ? !it->second->observedRunning : it->second->process.state() != QProcess::Running)) {
        return {};
    }
    return m_registry.attach(uid, id, client);
}

void VirtualSessionSupervisor::terminate(Runtime &r)
{
    if (r.stopping) {
        return;
    }
    r.stopping = true;
    r.deadline.stop();
    if (r.guardian) {
        r.observedRunning = false;
        r.poll.stop();
        r.deadline.start(m_stopTimeoutMs + 1000);
        queryGuardian(r);
        return;
    }
    r.process.terminate();
    // Context is the exact QProcess object, not a numeric PID or session name.
    // Replacing/deleting it cancels this callback before a successor can exist.
    QTimer::singleShot(m_stopTimeoutMs, &r.process, [&r] {
        if (r.process.state() != QProcess::NotRunning) {
            r.process.kill();
        }
    });
}

bool VirtualSessionSupervisor::stop(quint32 uid, const QString &id)
{
    auto handle = m_registry.stop(uid, id);
    const auto existing = m_runtimes.find(id);
    if (!handle && existing != m_runtimes.end() && existing->second->guardian
        && !existing->second->terminalConfirmed) {
        handle = m_registry.stopUnavailable(uid, id);
        if (handle) {
            existing->second->failed = false;
            existing->second->stopping = false;
            existing->second->stopSent = false;
        }
    }
    if (!handle) {
        return false;
    }
    if (auto *r = runtime(*handle)) {
        terminate(*r);
    }
    return true;
}

bool VirtualSessionSupervisor::forget(quint32 uid, const QString &id)
{
    const auto existing = m_runtimes.find(id);
    if (existing != m_runtimes.end() && existing->second->guardian && !existing->second->terminalConfirmed) return false;
    if (!m_registry.forget(uid, id)) {
        return false;
    }
    m_runtimes.erase(id);
    return true;
}
}
