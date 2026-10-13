// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>
#include <optional>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include "ClipboardText.h"
#include "ConsoleWorkerWire.h"

namespace KRdp
{
/**
 * OPT-067: the worker's side of the text clipboard bridge. The worker owns the host clipboard; this class
 * is the only code that touches it, through a small Backend so the logic is tested without a compositor.
 *
 *  - A host copy is coalesced (SettleMs) into one ClipboardOffer{seq}: no contents, none when the policy
 *    does not allow host to client or when the host has no text.
 *  - The text is read only when the client pastes (a ClipboardRequest), normalised to LF, without NULs,
 *    and refused when it exceeds the cap.
 *  - A ClipboardSet is written only when mayApplyClipboardSet() holds. Writes made on the login screen or
 *    a locked session carry the password-manager hint so Klipper keeps them out of its history.
 *  - Its own write is never offered back (a flag across the write, and the written text for EchoWindowMs).
 * Contents are never logged here or anywhere else on the bridge.
 */
class HostClipboard
{
public:
    struct Backend {
        std::function<bool()> hasText; ///< the host clipboard offers a text format (no data is read)
        std::function<QString()> readText; ///< its text as it is now (read on paste, and to spot an echo)
        std::function<bool(const QString &text, bool secret)> writeText; ///< empty text clears; false = failed
    };
    using Sender = std::function<void(const QByteArray &record)>;

    static constexpr int SettleMs = 100;
    static constexpr int EchoWindowMs = 2000;

    HostClipboard(Backend backend, Sender send, bool desktop)
        : m_backend(std::move(backend))
        , m_send(std::move(send))
        , m_desktop(desktop)
    {
        m_settle.setSingleShot(true);
        m_settle.setInterval(SettleMs);
        QObject::connect(&m_settle, &QTimer::timeout, &m_settle, [this] { offer(); });
    }

    /** The host's clipboard changed (KSystemClipboard::changed for the Clipboard mode). */
    void hostChanged()
    {
        if (m_writing || !ConsoleWorkerWire::mayOfferClipboard(m_policy, m_desktop)) {
            return;
        }
        m_settle.start(); // restarts: only the newest change is offered
    }

    void setPolicy(const ConsoleWorkerWire::ClipboardPolicy &policy)
    {
        if (policy.revision <= m_policy.revision) {
            return; // stale
        }
        m_policy = policy;
        if (!ConsoleWorkerWire::mayOfferClipboard(m_policy, m_desktop)) {
            m_settle.stop();
        }
    }

    void setControl(const ConsoleWorkerWire::ControlState &control)
    {
        m_control = control;
    }

    void request(const ConsoleWorkerWire::ClipboardRequest &request)
    {
        ConsoleWorkerWire::ClipboardData answer;
        answer.requestId = request.requestId;
        answer.seq = m_offerSeq;
        if (!ConsoleWorkerWire::mayAnswerClipboardRequest(request, m_control, m_policy, m_lastRequestId, m_desktop)) {
            answer.status = ConsoleWorkerWire::ClipboardStatus::Denied;
            m_send(ConsoleWorkerWire::frame(answer));
            return;
        }
        m_lastRequestId = request.requestId;
        if (!m_backend.hasText()) {
            answer.status = ConsoleWorkerWire::ClipboardStatus::Gone;
        } else {
            QString text = ClipboardText::toHost(m_backend.readText());
            text.remove(QChar(0));
            const quint64 limit = std::min<quint64>(request.maxBytes, ConsoleWorkerWire::MaxClipboardBytes);
            if (quint64(text.toUtf8().size()) > limit) {
                answer.status = ConsoleWorkerWire::ClipboardStatus::TooLarge;
            } else if (text.isEmpty()) {
                answer.status = ConsoleWorkerWire::ClipboardStatus::Empty;
            } else {
                answer.status = ConsoleWorkerWire::ClipboardStatus::Ok;
                answer.text = text;
            }
        }
        m_send(ConsoleWorkerWire::frame(answer));
    }

    void set(const ConsoleWorkerWire::ClipboardSet &set)
    {
        ConsoleWorkerWire::ClipboardSetResult result{set.setId, ConsoleWorkerWire::ClipboardSetStatus::Denied};
        if (ConsoleWorkerWire::mayApplyClipboardSet(set, m_control, m_policy, m_lastSetId, m_desktop)) {
            m_lastSetId = set.setId;
            QString text = set.text;
            text.remove(QChar(0));
            m_writing = true;
            const bool ok = m_backend.writeText(text, m_policy.locked || m_policy.greeter);
            m_writing = false;
            result.status = ok ? ConsoleWorkerWire::ClipboardSetStatus::Written : ConsoleWorkerWire::ClipboardSetStatus::Failed;
            if (ok) {
                m_settle.stop(); // an offer about the previous host text is stale
                m_lastWritten = text;
                m_written.start();
            }
        }
        m_send(ConsoleWorkerWire::frame(result));
    }

    quint64 offerSeq() const { return m_offerSeq; }

private:
    void offer()
    {
        if (!ConsoleWorkerWire::mayOfferClipboard(m_policy, m_desktop) || !m_backend.hasText()) {
            return;
        }
        // Only just after a write of ours is the text read, to tell an asynchronous echo from a new copy.
        if (m_lastWritten && m_written.isValid() && m_written.elapsed() < EchoWindowMs
            && ClipboardText::toHost(m_backend.readText()) == *m_lastWritten) {
            return;
        }
        m_send(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardOffer{++m_offerSeq, 1}));
    }

    Backend m_backend;
    Sender m_send;
    bool m_desktop;
    ConsoleWorkerWire::ClipboardPolicy m_policy;
    ConsoleWorkerWire::ControlState m_control;
    QTimer m_settle;
    quint64 m_offerSeq = 0;
    quint64 m_lastRequestId = 0;
    quint64 m_lastSetId = 0;
    bool m_writing = false;
    std::optional<QString> m_lastWritten;
    QElapsedTimer m_written;
};
}
