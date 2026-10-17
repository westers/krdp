// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QMimeData>
#include <QObject>

#include <freerdp/freerdp.h>
#include <freerdp/server/cliprdr.h>

#include <functional>
#include <mutex>
#include <optional>

#include "ClipboardFilePacing.h"
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
    void announceHost(bool text, bool image, bool html = false, bool files = false);
    /**
     * OPT-077: the answer to hostHtmlRequested(\a requestId): the host's HTML document as \a html (UTF-8, at most
     * ClipboardLimits::MaxHtmlBytes) when \a ok, a refusal otherwise. Sent as the registered "HTML Format" (a header with exact
     * byte offsets around the document, NUL terminated). The plain text beside it is a separate CF_UNICODETEXT request.
     */
    void provideHostHtml(quint64 requestId, const QByteArray &html, bool ok);
    /**
     * The answer to hostImageRequested(\a requestId): the host's image as \a png (PNG, validated by the caller's
     * worker, at most ClipboardLimits::MaxImageBytes) when \a ok, a refusal otherwise. Sent as "PNG" or, for a
     * client that asked for CF_DIB, converted to a 24-bit DIB (refused over ClipboardLimits::MaxImageWireBytes).
     */
    void provideHostImage(quint64 requestId, const QByteArray &png, bool ok);
    /**
     * OPT-078 (host-to-client file copy). The host copied files: announceHost(.., files = true) tells the client (alone, as
     * "FileGroupDescriptorW"); the client's paste raises hostFilesRequested(), answered with provideHostFiles() (the worker's
     * FileGroupDescriptorW); the client then asks for sizes and ranges (FILECONTENTS), which the HostFileBackend serves from the
     * worker, paced. Nothing of the files is held here. File traffic needs setFilesEnabled(true) BEFORE the channel starts (the
     * capability flags are exchanged once), the planner's setFileDirections(), and the client to have negotiated streaming.
     */
    void setFilesEnabled(bool enabled);
    bool filesEnabled() const;
    /** The planner's decision for file copy (new transfers). Going from allowed to denied ends the transfer and withdraws the offer. */
    void setFileDirections(bool hostToClient, bool clientToHost);
    bool hostFilesAllowed() const;
    /** A host-to-client file transfer is running (requests are pending): the planner treats it as started. */
    bool hostFileTransferActive() const;
    /** The client pasted files: the host's FileGroupDescriptorW is wanted for \a requestId (answer with provideHostFiles()). */
    Q_SIGNAL void hostFilesRequested(quint64 requestId, bool hugeFiles);
    /**
     * The answer to hostFilesRequested(): the worker's \a descriptor (FileGroupDescriptorW) of offer \a seq with \a skipped entries left
     * out, when \a ok; a refusal otherwise. The descriptor is decoded and checked again before the client sees it.
     */
    void provideHostFiles(quint64 requestId, quint64 seq, const QByteArray &descriptor, quint32 skipped, bool ok);
    enum class ReadResult { Ok, Changed, Gone, Denied, IoError, NoSuchOffer };
    /** The worker's answer to one range read the backend issued for \a streamId. */
    void provideHostFileRead(quint32 streamId, ReadResult result, const QByteArray &data);
    /** What reaches the worker for host-to-client files; set by the host controller / transport that owns the worker. */
    struct HostFileBackend {
        std::function<bool(quint64 seq, quint32 index, quint64 offset, quint32 length, quint32 streamId)> read; ///< false: not sent
        std::function<void(quint64 seq, quint32 clipDataId)> lock;
        std::function<void(quint32 clipDataId)> unlock;
        std::function<void(quint64 seq)> cancel; ///< 0 = every offer
    };
    void setHostFileBackend(HostFileBackend backend);
    /** The client's `clipboard-transfer` request (KRDPCTL) and the host owner's ceiling; the result is the effective policy. */
    struct FileRate {
        ClipboardFilePacing::RequestedMode mode = ClipboardFilePacing::RequestedMode::Auto;
        std::optional<quint64> capBytesPerSecond;
    };
    void setFileTransferRequest(const FileRate &request);
    void setFileCeiling(const ClipboardFilePacing::Ceiling &ceiling);
    ClipboardFilePacing::Policy effectiveFilePolicy() const;
    /** The host's file service ended a transfer before the client was done (a changed or vanished file, a violation, a reset). */
    Q_SIGNAL void hostFileTransferEnded(quint64 seq, int reason, int cause);
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
    /** OPT-077: the client pasted formatted text: the host's HTML is wanted for \a requestId (answer with provideHostHtml()). */
    Q_SIGNAL void hostHtmlRequested(quint64 requestId);

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
        quint64 refusedMalformed = 0; ///< OPT-072: a client picture that was not one (OPT-077: or HTML that was not valid)
        quint64 hostHtml = 0; ///< OPT-077: host HTML documents delivered to the client
        quint64 clientHtml = 0; ///< OPT-077: client HTML documents passed on
        quint64 hostFileLists = 0; ///< OPT-078: file lists delivered to the client
        quint64 hostFileRanges = 0; ///< OPT-078: ranges delivered
        quint64 hostFileBytes = 0; ///< OPT-078: file bytes delivered
        quint64 hostFileFailed = 0; ///< OPT-078: failed FILECONTENTS responses (hostile requests, changed files, resets)
        quint64 hostFileEnded = 0; ///< OPT-078: transfers ended early
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
     * OPT-078: the capability fields the server offers its client. File transfer (CB_STREAM_FILECLIP_ENABLED, CB_CAN_LOCK_CLIPDATA,
     * CB_HUGE_FILE_SUPPORT_ENABLED, CB_FILECLIP_NO_FILE_PATHS) only when \a filesEnabled (the host owner's ClipboardFiles is not off),
     * otherwise all four stay off. Public so a test can check what a channel would advertise.
     */
    static void applyFileCapabilities(CliprdrServerContext *context, bool filesEnabled);

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
    /// OPT-078: how long the worker has to walk a selection and answer a file paste (its own walk budget is 5 s).
    static constexpr int HostFilesTimeoutMs = 12000;
    /// OPT-078: the pump that issues file reads and paces the responses.
    static constexpr int FilePumpMs = 10;

private:
    void sendServerData();

    class Private;
    const std::unique_ptr<Private> d;
};
}
