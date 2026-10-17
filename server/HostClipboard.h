// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>
#include <optional>

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include "ClipboardHtml.h"
#include "ClipboardImage.h"
#include "ClipboardLimits.h"
#include "ClipboardText.h"
#include "ConsoleWorkerWire.h"

namespace KRdp
{
/**
 * OPT-067 / OPT-072: the worker's side of the text and image clipboard bridge. The worker owns the host clipboard; this class
 * is the only code that touches it, through a small Backend so the logic is tested without a compositor.
 *
 *  - A host copy is coalesced (SettleMs) into one ClipboardOffer{seq}: no contents, none when the policy
 *    does not allow host to client or when the host has no text.
 *  - The text is read only when the client pastes (a ClipboardRequest), normalised to LF, without NULs,
 *    and refused when it exceeds the cap.
 *  - A ClipboardSet is written only when mayApplyClipboardSet() holds. Writes made on the login screen or
 *    a locked session carry the password-manager hint so Klipper keeps them out of its history.
 *  - Its own write is never offered back (a flag across the write, and the written text for EchoWindowMs).
 *  - OPT-072: an image is offered the same way (flag bit 1 of the offer, no pixels), read only when the client
 *    pastes, turned into PNG (a PNG is checked and passed through, any other format decoded after its size
 *    is checked) and refused over 16 MiB or 8192x8192; a client's PNG is checked again before it is written.
 *    Text wins no precedence here: a copy with both is offered as both and the broker/client choose.
 *  - OPT-077: formatted text the same way. HTML is a third offer flag; it is read only on a paste (bounded at 8 MiB, any
 *    UTF-16 turned into UTF-8), and a client's text and HTML are written together in one offer so the receiving application
 *    picks the richest format it understands. Which of text / HTML / picture are offered follows ClipboardHtml::announce().
 * Contents are never logged here or anywhere else on the bridge; a refusal logs a size and a reason only.
 */
class HostClipboard
{
public:
    /// A bounded read of the host's image: \a data is empty for none; \a tooLarge when the source sent more than the cap (\a bytes is
    /// then what had arrived when the read was abandoned, a lower bound).
    struct ImageRead {
        QByteArray data;
        bool tooLarge = false;
        qint64 bytes = 0;
    };

    struct Backend {
        std::function<bool()> hasText; ///< the host clipboard offers a text format (no data is read)
        std::function<QString()> readText; ///< its text as it is now (read on paste, and to spot an echo)
        std::function<bool(const QString &text, bool secret)> writeText; ///< empty text clears; false = failed
        // OPT-072. Optional: a backend without them offers no images.
        std::function<bool()> hasImage; ///< the host clipboard offers an image format (no data is read)
        std::function<QByteArray()> readImage; ///< the image as the host holds it (any format; PNG preferred); empty = none
        std::function<bool(const QByteArray &png, bool secret)> writeImage; ///< false = failed
        /// OPT-072 (Wayland): the image read with a byte cap, so a huge source is abandoned rather than read whole. Preferred over
        /// readImage when set.
        std::function<ImageRead()> readImageBounded;
        // OPT-077. Optional: a backend without them offers no HTML.
        std::function<bool()> hasHtml; ///< the host clipboard offers text/html (no data is read)
        std::function<ImageRead()> readHtml; ///< its bytes with a byte cap (any encoding; normalised here); tooLarge when the source sent more
        std::function<bool(const QString &text, const QByteArray &html, bool secret)> writeRich; ///< text/plain and text/html in one offer; false = failed
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
        if (request.kind == ConsoleWorkerWire::ClipboardKind::Image) {
            answerImage(request, answer);
            m_send(ConsoleWorkerWire::frame(answer));
            return;
        }
        if (request.kind == ConsoleWorkerWire::ClipboardKind::Html) {
            answerHtml(request, answer);
            m_send(ConsoleWorkerWire::frame(answer));
            return;
        }
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
        if (ConsoleWorkerWire::mayApplyClipboardSet(set, m_control, m_policy, m_lastSetId, m_desktop)
            && set.kind == ConsoleWorkerWire::ClipboardKind::Image) {
            m_lastSetId = set.setId;
            result.status = writeImage(set);
        } else if (ConsoleWorkerWire::mayApplyClipboardSet(set, m_control, m_policy, m_lastSetId, m_desktop)
                   && set.kind == ConsoleWorkerWire::ClipboardKind::Html) {
            m_lastSetId = set.setId;
            result.status = writeRich(set);
        } else if (ConsoleWorkerWire::mayApplyClipboardSet(set, m_control, m_policy, m_lastSetId, m_desktop)) {
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
                m_lastWrittenImage.reset();
                m_lastWrittenHtml.reset();
                m_written.start();
            }
        }
        m_send(ConsoleWorkerWire::frame(result));
    }

    quint64 offerSeq() const { return m_offerSeq; }

private:
    void offer()
    {
        if (!ConsoleWorkerWire::mayOfferClipboard(m_policy, m_desktop)) {
            return;
        }
        const bool hasText = m_backend.hasText();
        const bool hasImage = m_backend.hasImage && m_backend.hasImage();
        const bool hasHtml = m_backend.hasHtml && m_backend.readHtml && m_backend.hasHtml();
        if (!hasText && !hasImage && !hasHtml) {
            return;
        }
        // OPT-077: formats from which formats exist (never their contents): rich HTML goes with its text and drops a picture;
        // HTML beside a picture and no text is the picture ("Copy image" in a browser).
        const auto choice = ClipboardHtml::announce(hasText, hasHtml, hasImage);
        quint8 flags = 0;
        if (choice.text) flags |= ConsoleWorkerWire::ClipboardOfferText;
        if (choice.image) flags |= ConsoleWorkerWire::ClipboardOfferImage;
        if (choice.html) flags |= ConsoleWorkerWire::ClipboardOfferHtml;
        if (!flags) {
            return;
        }
        // Only just after a write of ours is the content read, to tell an asynchronous echo from a new copy.
        const bool recent = m_written.isValid() && m_written.elapsed() < EchoWindowMs;
        if (recent && m_lastWrittenHtml && (flags & ConsoleWorkerWire::ClipboardOfferHtml)
            && ClipboardHtml::normaliseHostHtml(m_backend.readHtml().data) == *m_lastWrittenHtml) {
            // Our own text+html write coming back: nothing of it is a host copy (the text beside the HTML was written with it).
            flags &= ~(ConsoleWorkerWire::ClipboardOfferHtml | ConsoleWorkerWire::ClipboardOfferText);
        }
        if (recent && m_lastWritten && (flags & ConsoleWorkerWire::ClipboardOfferText)
            && ClipboardText::toHost(m_backend.readText()) == *m_lastWritten) {
            flags &= ~ConsoleWorkerWire::ClipboardOfferText;
        }
        if (recent && m_lastWrittenImage && (flags & ConsoleWorkerWire::ClipboardOfferImage) && (m_backend.readImage || m_backend.readImageBounded)
            && isEchoOfLastWrite(readHostImageForEcho())) {
            flags &= ~ConsoleWorkerWire::ClipboardOfferImage;
        }
        if (!flags) {
            return;
        }
        m_send(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardOffer{++m_offerSeq, flags}));
    }

    /// The host copy is our own write coming back when it holds the same pixels. The bytes can differ: KSystemClipboard
    /// answers other processes with its own PNG encoding of the picture (see PngMimeData), so a byte compare would miss.
    bool isEchoOfLastWrite(const QByteArray &hostImage) const
    {
        if (hostImage.isEmpty() || !m_lastWrittenImage) return false;
        if (hostImage == *m_lastWrittenImage) return true;
        const auto converted = ClipboardImage::toPng(hostImage); // validates the structure and the dimensions before any decode
        return converted.ok() && ClipboardImage::samePixels(converted.data, *m_lastWrittenImage);
    }

    QByteArray readHostImageForEcho() const
    {
        // Bounded when the backend can: the echo check must not read an unlimited foreign image either.
        return m_backend.readImageBounded ? m_backend.readImageBounded().data : m_backend.readImage();
    }

    static void logRefusal(const char *direction, const ClipboardImage::Result &result, qint64 bytes)
    {
        // A reason and sizes, never contents (OPT-072).
        QDebug out = qInfo().nospace();
        out << "Clipboard: refused (" << ClipboardImage::errorName(result.error) << ") direction=" << direction << " kind=image bytes=" << bytes;
        if (result.size.isValid()) out << " size=" << result.size.width() << "x" << result.size.height();
    }

    void answerImage(const ConsoleWorkerWire::ClipboardRequest &request, ConsoleWorkerWire::ClipboardData &answer)
    {
        answer.kind = ConsoleWorkerWire::ClipboardKind::Image;
        if (!m_backend.hasImage || !m_backend.readImage || !m_backend.hasImage()) {
            answer.status = ConsoleWorkerWire::ClipboardStatus::Gone;
            return;
        }
        QByteArray raw;
        if (m_backend.readImageBounded) {
            const ImageRead read = m_backend.readImageBounded();
            if (read.tooLarge) {
                logRefusal("host-to-client", ClipboardImage::Result{{}, ClipboardImage::Error::TooLarge, {}}, read.bytes);
                answer.status = ConsoleWorkerWire::ClipboardStatus::TooLarge;
                return;
            }
            raw = read.data;
        } else {
            raw = m_backend.readImage();
        }
        const auto converted = ClipboardImage::toPng(raw);
        const quint64 limit = std::min<quint64>(request.maxBytes, ConsoleWorkerWire::MaxClipboardImageBytes);
        if (converted.error == ClipboardImage::Error::Empty) {
            answer.status = ConsoleWorkerWire::ClipboardStatus::Gone;
        } else if (converted.error == ClipboardImage::Error::TooLarge || converted.error == ClipboardImage::Error::TooManyPixels) {
            logRefusal("host-to-client", converted, raw.size());
            answer.status = ConsoleWorkerWire::ClipboardStatus::TooLarge;
        } else if (!converted.ok()) {
            logRefusal("host-to-client", converted, raw.size());
            answer.status = ConsoleWorkerWire::ClipboardStatus::Gone;
        } else if (quint64(converted.data.size()) > limit) {
            logRefusal("host-to-client", ClipboardImage::Result{{}, ClipboardImage::Error::TooLarge, converted.size}, converted.data.size());
            answer.status = ConsoleWorkerWire::ClipboardStatus::TooLarge;
        } else {
            answer.status = ConsoleWorkerWire::ClipboardStatus::Ok;
            answer.image = converted.data;
        }
    }

    void answerHtml(const ConsoleWorkerWire::ClipboardRequest &request, ConsoleWorkerWire::ClipboardData &answer)
    {
        answer.kind = ConsoleWorkerWire::ClipboardKind::Html;
        if (!m_backend.hasHtml || !m_backend.readHtml || !m_backend.hasHtml()) {
            answer.status = ConsoleWorkerWire::ClipboardStatus::Gone;
            return;
        }
        const ImageRead read = m_backend.readHtml();
        const quint64 limit = std::min<quint64>(request.maxBytes, ConsoleWorkerWire::MaxClipboardHtmlBytes);
        if (read.tooLarge || quint64(read.data.size()) > ClipboardLimits::MaxHtmlBytes * 2) {
            logHtmlRefusal("oversize", read.tooLarge ? read.bytes : read.data.size());
            answer.status = ConsoleWorkerWire::ClipboardStatus::TooLarge;
            return;
        }
        const QByteArray html = ClipboardHtml::normaliseHostHtml(read.data);
        if (html.trimmed().isEmpty()) {
            answer.status = ConsoleWorkerWire::ClipboardStatus::Gone;
        } else if (quint64(html.size()) > limit) {
            logHtmlRefusal("oversize", html.size());
            answer.status = ConsoleWorkerWire::ClipboardStatus::TooLarge;
        } else {
            answer.status = ConsoleWorkerWire::ClipboardStatus::Ok;
            answer.html = html;
        }
    }

    static void logHtmlRefusal(const char *reason, qint64 bytes)
    {
        // A reason and a size, never contents (OPT-077).
        qInfo().nospace() << "Clipboard: refused (" << reason << ") direction=host-to-client kind=html bytes=" << bytes;
    }

    ConsoleWorkerWire::ClipboardSetStatus writeRich(const ConsoleWorkerWire::ClipboardSet &set)
    {
        // The broker has unwrapped and checked this HTML; the worker bounds it once more before a host application sees it.
        QString text = set.text;
        text.remove(QChar(0));
        const QByteArray html = ClipboardHtml::normaliseHostHtml(set.html);
        if (html.trimmed().isEmpty() || html.size() > ClipboardLimits::MaxHtmlBytes || !m_backend.writeRich) {
            if (m_backend.writeRich) logHtmlRefusal(html.size() > ClipboardLimits::MaxHtmlBytes ? "oversize" : "empty", html.size());
            return ConsoleWorkerWire::ClipboardSetStatus::Failed;
        }
        m_writing = true;
        const bool ok = m_backend.writeRich(ClipboardText::toHost(text), html, m_policy.locked || m_policy.greeter);
        m_writing = false;
        if (ok) {
            m_settle.stop();
            m_lastWrittenHtml = html;
            m_lastWritten.reset();
            m_lastWrittenImage.reset();
            m_written.start();
        }
        return ok ? ConsoleWorkerWire::ClipboardSetStatus::Written : ConsoleWorkerWire::ClipboardSetStatus::Failed;
    }

    ConsoleWorkerWire::ClipboardSetStatus writeImage(const ConsoleWorkerWire::ClipboardSet &set)
    {
        // The broker has checked this PNG; the worker is the last gate before a host application sees it.
        const auto checked = ClipboardImage::validatePng(set.image);
        if (!checked.ok() || !m_backend.writeImage) {
            if (!checked.ok()) logRefusal("client-to-host", checked, set.image.size());
            return ConsoleWorkerWire::ClipboardSetStatus::Failed;
        }
        m_writing = true;
        const bool ok = m_backend.writeImage(set.image, m_policy.locked || m_policy.greeter);
        m_writing = false;
        if (ok) {
            m_settle.stop();
            m_lastWrittenImage = set.image;
            m_lastWritten.reset();
            m_lastWrittenHtml.reset();
            m_written.start();
        }
        return ok ? ConsoleWorkerWire::ClipboardSetStatus::Written : ConsoleWorkerWire::ClipboardSetStatus::Failed;
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
    std::optional<QByteArray> m_lastWrittenImage;
    std::optional<QByteArray> m_lastWrittenHtml; // OPT-077: normalised UTF-8 of our last text+html write
    QElapsedTimer m_written;
};
}
