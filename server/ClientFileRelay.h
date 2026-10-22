// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
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
 * OPT-078 phase 3: the glue between a connection's Clipboard (which asks the client for file bytes) and the worker's endpoint (which stages
 * them), shared by the Console and the Virtual broker. One relay per endpoint: it hands each Clipboard a ClientFileBackend whose calls reach
 * the endpoint only while \a allowed says this connection controls the worker and the planner allows client-to-host files, remembers which
 * Clipboard owns which transfer (the endpoint numbers them endpoint-wide), and routes the worker's replies, acknowledgements and states back
 * to the owner. A connection ending cancels only ITS transfers: another connection's (the controller's) are never touched by a viewer's.
 */
class ClientFileRelay
{
    // The backend lambdas outlive the relay when a connection is destroyed after the broker that owns the relay (a Clipboard cancels its
    // transfers from its destructor): they hold the state, and a relay that is gone makes them no-ops BEFORE they touch anything else.
    struct State {
        bool alive = true;
        QHash<quint64, QPointer<Clipboard>> owners;
    };

public:
    ClientFileRelay()
        : m_state(std::make_shared<State>())
    {
    }
    ~ClientFileRelay() { m_state->alive = false; }
    ClientFileRelay(const ClientFileRelay &) = delete;
    ClientFileRelay &operator=(const ClientFileRelay &) = delete;

    using Allowed = std::function<bool()>;
    using Endpoint = std::function<ConsoleWorkerEndpoint *()>;
    using Reserve = std::function<quint64()>;

    Clipboard::ClientFileBackend backendFor(QPointer<Clipboard> clipboard, Endpoint endpoint, Allowed allowed, Reserve reserve)
    {
        Clipboard::ClientFileBackend backend;
        const auto state = m_state;
        backend.offer = [state, clipboard, endpoint, allowed, reserve](const QByteArray &descriptor, bool huge) -> quint64 {
            if (!state->alive || !clipboard) return 0;
            auto *worker = endpoint();
            if (!worker || !allowed()) return 0;
            const quint64 seq = worker->offerPushFiles(descriptor, huge, reserve ? reserve() : 0);
            if (seq) state->owners.insert(seq, clipboard);
            return seq;
        };
        backend.data = [state, clipboard, endpoint, allowed](quint64 seq, quint32 index, quint64 offset, const QByteArray &data) {
            if (!state->alive || !clipboard || state->owners.value(seq) != clipboard) return false;
            auto *worker = endpoint();
            if (!worker || !allowed()) return false;
            return worker->pushFileData(seq, index, offset, data);
        };
        backend.cancel = [state, clipboard, endpoint](quint64 seq, int reason) {
            if (!state->alive) return; // the broker is gone with its worker: nothing to tell
            auto &m_owners = state->owners;
            const auto why = ConsoleWorkerWire::FilePushCancelReason(std::clamp(reason, 0, int(ConsoleWorkerWire::LastFilePushCancelReason)));
            auto *worker = endpoint();
            // Releasing is always allowed, but only for what this connection owns: "every transfer" is every transfer of THIS clipboard.
            QList<quint64> seqs;
            if (seq == 0) {
                for (auto it = m_owners.cbegin(); it != m_owners.cend(); ++it) {
                    if (it.value() == clipboard) seqs.append(it.key());
                }
            } else if (m_owners.value(seq) == clipboard) {
                seqs.append(seq);
            }
            for (const quint64 owned : std::as_const(seqs)) {
                m_owners.remove(owned);
                if (worker) worker->cancelPushFiles(owned, why);
            }
        };
        return backend;
    }

    /** The worker answered an offer. */
    void replied(const ConsoleWorkerWire::FilePushReply &reply)
    {
        auto &m_owners = m_state->owners;
        const auto owner = m_owners.value(reply.seq);
        if (!owner) {
            m_owners.remove(reply.seq);
            return;
        }
        Clipboard::ClientFilesReply out;
        out.seq = reply.seq;
        out.accepted = reply.accepted;
        out.reason = QString::fromLatin1(ConsoleWorkerWire::filePushReasonName(reply.reason));
        out.files = reply.files;
        out.directories = reply.directories;
        out.skipped = reply.skipped;
        out.totalBytes = reply.totalBytes;
        out.freeBytes = reply.freeBytes;
        out.neededBytes = reply.neededBytes;
        if (!reply.accepted) m_owners.remove(reply.seq);
        owner->clientFilesReplied(out);
    }

    /** The worker wrote (or refused) a range. */
    void acked(const ConsoleWorkerWire::FilePushAck &ack)
    {
        if (const auto owner = m_state->owners.value(ack.seq)) owner->clientFilesAcked(ack.seq, ack.index, ack.offset, ack.length, int(ack.status));
    }

    /** Progress, the offer to the host clipboard, the end of a transfer. A finished copy stays owned until its owner cancels it. */
    void stateChanged(const ConsoleWorkerWire::FilePushState &state)
    {
        auto &m_owners = m_state->owners;
        const auto owner = m_owners.value(state.seq);
        if (!owner) return;
        using Phase = ConsoleWorkerWire::FilePushPhase;
        const auto phase = state.phase == Phase::Staging ? Clipboard::ClientFilesPhase::Staging
            : state.phase == Phase::Offered              ? Clipboard::ClientFilesPhase::Offered
            : state.phase == Phase::Done                 ? Clipboard::ClientFilesPhase::Done
                                                         : Clipboard::ClientFilesPhase::Failed;
        if (state.phase == Phase::Failed) m_owners.remove(state.seq); // the worker removed its staging
        owner->clientFilesState(state.seq, phase, QString::fromLatin1(ConsoleWorkerWire::filePushReasonName(state.reason)), state.doneBytes, state.totalBytes);
    }

    /** The worker is gone or control moved: nobody will be answered. */
    void clear() { m_state->owners.clear(); }
    int owned() const { return int(m_state->owners.size()); }

private:
    std::shared_ptr<State> m_state;
};
}
