// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>
#include <memory>

#include <QHash>
#include <QPointer>

#include "Clipboard.h"
#include "ConsoleWorkerEndpoint.h"
#include "ConsoleWorkerWire.h"

namespace KRdp
{
/**
 * OPT-078: the glue between a connection's Clipboard (which serves FILECONTENTS and paces it) and the worker's endpoint (which
 * reads the files), shared by the Console and the Virtual broker. One relay per endpoint: it hands the Clipboard a HostFileBackend
 * whose calls reach the endpoint only while \a allowed says this connection may transfer files now (it controls the worker and the
 * planner allows host-to-client files), and routes each read's answer back to the Clipboard that asked.
 *
 * The backend lambdas outlive the relay when a connection is destroyed after the broker that owns the relay (a Clipboard cancels its
 * transfers from its destructor): they hold the state, not the relay, and a relay that is gone makes them no-ops before they touch anything.
 */
class HostFileRelay
{
    struct Pending {
        QPointer<Clipboard> clipboard;
        quint32 streamId = 0;
        quint64 seq = 0;
    };
    struct State {
        bool alive = true;
        QHash<quint64, Pending> pending;
    };

public:
    HostFileRelay()
        : m_state(std::make_shared<State>())
    {
    }
    ~HostFileRelay() { m_state->alive = false; }
    HostFileRelay(const HostFileRelay &) = delete;
    HostFileRelay &operator=(const HostFileRelay &) = delete;

    using Allowed = std::function<bool()>;
    using Endpoint = std::function<ConsoleWorkerEndpoint *()>;

    /** The backend for \a clipboard. \a endpoint is looked up on every call (a Virtual transport's worker comes and goes). */
    Clipboard::HostFileBackend backendFor(QPointer<Clipboard> clipboard, Endpoint endpoint, Allowed allowed)
    {
        Clipboard::HostFileBackend backend;
        const auto state = m_state;
        backend.read = [state, clipboard, endpoint, allowed](quint64 seq, quint32 index, quint64 offset, quint32 length, quint32 streamId) {
            if (!state->alive) return false;
            auto *worker = endpoint();
            if (!clipboard || !worker || !allowed()) return false;
            auto &pending = state->pending;
            if (pending.size() >= 4 * ConsoleWorkerEndpoint::MaxFileReads) pending.clear(); // answers to abandoned reads are dropped
            const quint64 readId = worker->readFile(seq, index, offset, length);
            if (!readId) return false;
            pending.insert(readId, {clipboard, streamId, seq});
            return true;
        };
        backend.lock = [state, endpoint, allowed](quint64 seq, quint32 clipDataId) {
            if (!state->alive) return;
            if (auto *worker = endpoint(); worker && allowed()) worker->lockFiles(seq, clipDataId);
        };
        backend.unlock = [state, endpoint](quint32 clipDataId) {
            if (!state->alive) return;
            if (auto *worker = endpoint()) worker->unlockFiles(clipDataId); // releasing is always allowed
        };
        backend.cancel = [state, clipboard, endpoint, allowed](quint64 seq) {
            if (!state->alive) return; // the broker is gone with its worker: nothing to tell
            auto &pending = state->pending;
            for (auto it = pending.begin(); it != pending.end();) {
                it = it->clipboard == clipboard && (seq == 0 || it->seq == seq) ? pending.erase(it) : std::next(it);
            }
            // Only the connection that is transferring may end the worker's reads; a viewer resetting its own state must not.
            if (auto *worker = endpoint(); worker && allowed()) worker->cancelFiles(seq);
        };
        return backend;
    }

    /** The worker answered a read. */
    void finished(const ConsoleWorkerWire::FileReadResult &result)
    {
        const auto pending = m_state->pending.take(result.readId);
        if (!pending.clipboard) return;
        using Status = ConsoleWorkerWire::FileReadStatus;
        Clipboard::ReadResult status = Clipboard::ReadResult::IoError;
        switch (result.status) {
        case Status::Ok: status = Clipboard::ReadResult::Ok; break;
        case Status::Changed: status = Clipboard::ReadResult::Changed; break;
        case Status::Gone: status = Clipboard::ReadResult::Gone; break;
        case Status::Denied: status = Clipboard::ReadResult::Denied; break;
        case Status::IoError: status = Clipboard::ReadResult::IoError; break;
        case Status::NoSuchOffer: status = Clipboard::ReadResult::NoSuchOffer; break;
        }
        pending.clipboard->provideHostFileRead(pending.streamId, status, result.data);
    }

    /** The worker is gone or control moved: nothing in flight will be answered. */
    void clear() { m_state->pending.clear(); }
    int pending() const { return int(m_state->pending.size()); }

private:
    std::shared_ptr<State> m_state;
};
}
