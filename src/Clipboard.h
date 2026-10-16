// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QMimeData>
#include <QObject>

#include <freerdp/freerdp.h>
#include <freerdp/server/cliprdr.h>

#include <mutex>

#include "krdp_export.h"

namespace KRdp
{

class RdpConnection;

class KRDP_EXPORT Clipboard : public QObject
{
    Q_OBJECT

public:
    explicit Clipboard(RdpConnection *session);
    ~Clipboard() override;

    bool initialize();
    void close();

    bool enabled();

    void setServerData(const QMimeData *data);

    // ---- OPT-067: the Console and Virtual bridge (delayed rendering) ----
    // The host's clipboard lives in a worker process, not here. The host side announces that text
    // exists (announceHostText), the client's paste raises hostTextRequested, and the text is
    // handed back with provideHostText; nothing of the host's clipboard is held in between. All
    // of this runs on the main thread. The in-process route above (setServerData) is unchanged.

    /** Which directions the planner allows now. Going from allowed to denied resets that side. */
    void setDirections(bool hostToClient, bool clientToHost);
    bool hostToClientAllowed() const;
    bool clientToHostAllowed() const;
    /** The host copied text: tell the client (coalesced, at most one format list per HostAnnounceIntervalMs). */
    void announceHostText();
    /**
     * OPT-072: the host copied \a text and/or an \a image (a copy can have both). The format list names
     * CF_UNICODETEXT for text and the registered "PNG" plus CF_DIB for an image; nothing of either is held here.
     */
    void announceHost(bool text, bool image);
    /**
     * The answer to hostImageRequested(\a requestId): the host's image as \a png (PNG, validated by the caller's
     * worker, at most ClipboardLimits::MaxImageBytes) when \a ok, a refusal otherwise. Sent as "PNG" or, for a
     * client that asked for CF_DIB, converted to a 24-bit DIB (refused over ClipboardLimits::MaxImageWireBytes).
     */
    void provideHostImage(quint64 requestId, const QByteArray &png, bool ok);
    /** The answer to hostTextRequested(\a requestId): \a text (host LF) when \a ok, a refusal otherwise. */
    void provideHostText(quint64 requestId, const QString &text, bool ok);
    /** Forget the host side: withdraw an announcement, fail a pending request. Handover, control change, denial. */
    void resetHostSide();
    /** Forget a half-received client copy and any request on the wire. */
    void resetClientSide();
    /** The client pasted: the host's text is wanted for \a requestId (answer with provideHostText()). */
    Q_SIGNAL void hostTextRequested(quint64 requestId);
    /** OPT-072: the client pasted an image: the host's image is wanted for \a requestId (answer with provideHostImage()). */
    Q_SIGNAL void hostImageRequested(quint64 requestId);

    struct Counters {
        quint64 announces = 0; ///< format lists sent for a host copy
        quint64 served = 0; ///< host texts delivered to the client
        quint64 refusedPolicy = 0;
        quint64 refusedOversize = 0;
        quint64 refusedRate = 0;
        quint64 refusedTimeout = 0;
        quint64 refusedBusy = 0;
        quint64 clientCopies = 0; ///< client texts passed on
        quint64 hostBytes = 0; ///< UTF-8 bytes of host text served
        quint64 clientBytes = 0; ///< UTF-8 bytes of client text passed on (OPT-072: encoded bytes of an image)
        quint64 hostImages = 0; ///< OPT-072: host images delivered to the client
        quint64 clientImages = 0; ///< OPT-072: client images passed on
        quint64 refusedMalformed = 0; ///< OPT-072: a client picture that was not one
    };
    Counters counters() const;

    /**
     * Test seam: send through \a context (not owned, not freed) instead of a FreeRDP channel.
     */
    void attachContextForTest(CliprdrServerContext *context);

    Q_SIGNAL void clientDataChanged();
    std::unique_ptr<QMimeData> getClipboard() const;

    /**
     * Point \a context's client-PDU callbacks at \a clipboard. The callbacks
     * run on FreeRDP's cliprdr thread; they copy the PDU and hand it to the
     * main thread without waiting (AUD-P5). Public so a test can drive them
     * without a peer.
     */
    static void installClientCallbacks(CliprdrServerContext *context, Clipboard *clipboard);

    /**
     * AUD-FIX5: the client has a picture (VideoStream::graphicsDelivered()). Until then the
     * server sends the client no clipboard request and no clipboard announcement, only the
     * protocol's format-list responses: a stock client (wlfreerdp3 3.22 on an unlocked KDE
     * desktop) that blocks its connection on the local clipboard read a data request starts
     * must not be kept from its graphics setup by it. DeliveredFallback after the first client
     * format list the clipboard goes ahead anyway (a client that never acknowledges a frame).
     */
    void setGraphicsDelivered();

    /// How long the client's format lists are collected before one data request goes out.
    static constexpr int FormatListSettleMs = 250;
    /// A data request not answered for this long no longer holds back the next one.
    static constexpr int ResponseTimeoutMs = 5000;
    /// See setGraphicsDelivered().
    static constexpr int DeliveredFallbackMs = 15000;
    /**
     * AUD-FIX6 F1: a client format list this soon after the server announced or served its
     * own text is taken for that text coming back, and not requested.
     */
    static constexpr int EchoWindowMs = 2000;
    /// OPT-067: at most one format list for a host copy per this long.
    static constexpr int HostAnnounceIntervalMs = 250;
    /// OPT-067: how long the worker has to answer a paste.
    static constexpr int HostTextTimeoutMs = 3000;

private:
    void sendServerData();

    class Private;
    const std::unique_ptr<Private> d;
};
}
