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
        value->process.disconnect(this);
        if (value->process.state() != QProcess::NotRunning) {
            value->process.kill();
            value->process.waitForFinished(1000);
        }
    }
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
    if (!r || r->stopping || r->failed || r->process.state() != QProcess::Running || !m_registry.ready(handle)) {
        return false;
    }
    r->deadline.stop();
    return true;
}

std::optional<VirtualSessionSupervisor::Handle> VirtualSessionSupervisor::attach(quint32 uid, const QString &id, quint64 client)
{
    const auto it = m_runtimes.find(id);
    if (it == m_runtimes.end() || it->second->stopping || it->second->failed
        || it->second->process.state() != QProcess::Running) {
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
    if (!m_registry.forget(uid, id)) {
        return false;
    }
    m_runtimes.erase(id);
    return true;
}
}
