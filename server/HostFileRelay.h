// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>

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
 */
class HostFileRelay
{
public:
    using Allowed = std::function<bool()>;
    using Endpoint = std::function<ConsoleWorkerEndpoint *()>;

    /** The backend for \a clipboard. \a endpoint is looked up on every call (a Virtual transport's worker comes and goes). */
    Clipboard::HostFileBackend backendFor(QPointer<Clipboard> clipboard, Endpoint endpoint, Allowed allowed)
    {
        Clipboard::HostFileBackend backend;
        backend.read = [this, clipboard, endpoint, allowed](quint64 seq, quint32 index, quint64 offset, quint32 length, quint32 streamId) {
            auto *worker = endpoint();
            if (!clipboard || !worker || !allowed()) return false;
            if (m_pending.size() >= 4 * ConsoleWorkerEndpoint::MaxFileReads) m_pending.clear(); // answers to abandoned reads are dropped
            const quint64 readId = worker->readFile(seq, index, offset, length);
            if (!readId) return false;
            m_pending.insert(readId, {clipboard, streamId, seq});
            return true;
        };
        backend.lock = [endpoint, allowed](quint64 seq, quint32 clipDataId) {
            if (auto *worker = endpoint(); worker && allowed()) worker->lockFiles(seq, clipDataId);
        };
        backend.unlock = [endpoint](quint32 clipDataId) {
            if (auto *worker = endpoint()) worker->unlockFiles(clipDataId); // releasing is always allowed
        };
        backend.cancel = [this, clipboard, endpoint, allowed](quint64 seq) {
            for (auto it = m_pending.begin(); it != m_pending.end();) {
                it = it->clipboard == clipboard && (seq == 0 || it->seq == seq) ? m_pending.erase(it) : std::next(it);
            }
            // Only the connection that is transferring may end the worker's reads; a viewer resetting its own state must not.
            if (auto *worker = endpoint(); worker && allowed()) worker->cancelFiles(seq);
        };
        return backend;
    }

    /** The worker answered a read. */
    void finished(const ConsoleWorkerWire::FileReadResult &result)
    {
        const auto pending = m_pending.take(result.readId);
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
    void clear() { m_pending.clear(); }
    int pending() const { return int(m_pending.size()); }

private:
    struct Pending {
        QPointer<Clipboard> clipboard;
        quint32 streamId = 0;
        quint64 seq = 0;
    };
    QHash<quint64, Pending> m_pending;
};
}
