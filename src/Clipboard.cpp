// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Clipboard.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/server/cliprdr.h>

#include <QElapsedTimer>
#include <QMetaMethod>
#include <QScopeGuard>
#include <QTimer>

#include <atomic>
#include <mutex>
#include <optional>

#include "ClipboardHtml.h"
#include "ClipboardMime.h"
#include "ClipboardImage.h"
#include "ClipboardLimits.h"
#include "ClipboardText.h"
#include "PeerContext_p.h"
#include "RdpConnection.h"

#include "krdp_logging.h"

using namespace Qt::StringLiterals;

namespace KRdp
{

class KRDP_NO_EXPORT Clipboard::Private
{
public:
    Private(Clipboard *qq)
        : q(qq)
    {
    }

    Clipboard *q;

    // The client's PDUs, copied out of FreeRDP's buffers on the cliprdr
    // thread so they can be handled later on the main thread.
    struct FormatList {
        QList<uint32_t> formatIds;
        QStringList formatNames; // parallel to formatIds; empty for a standard format (OPT-072: "PNG" is a name)
    };
    struct FormatDataRequest {
        uint32_t requestedFormatId = 0;
    };
    struct FormatDataResponse {
        uint16_t msgFlags = 0;
        QByteArray data;
        // OPT-067: the client declared more than ClipboardLimits::MaxWireBytes; the bytes were not copied.
        bool oversize = false;
        uint32_t declaredLength = 0;
    };

    void onClientFormatList(const FormatList &formatList);
    void onClientFormatDataRequest(const FormatDataRequest &formatDataRequest);
    void onClientFormatDataResponse(const FormatDataResponse &formatDataResponse);

    RdpConnection *session;

    // AUD-P7: created on the session thread (initialize()), stopped there
    // (close()), and written through on the main thread; sendMutex orders
    // those writes against Stop(). Freed in the destructor only.
    std::atomic<CliprdrServerContext *> clipContext = nullptr;
    std::mutex sendMutex;
    std::atomic<bool> enabled = false;
    // Set by close(): handlers posted before it and run after it are dropped.
    std::atomic<bool> closing = false;

    const QMimeData *serverData = nullptr;

    // OPT-067 (Console and Virtual bridge). Main-thread state.
    bool ownsContext = true;
    bool hostToClientAllowed = true;
    bool clientToHostAllowed = true;
    bool hostAnnounced = false; // delayed rendering: the host has text and/or an image; none of it is held here
    bool hostText = false; // OPT-072: what the announcement covers
    bool hostImage = false;
    bool hostHtml = false; // OPT-077
    uint32_t pendingHostFormat = 0; // the format the client asked for in the request waiting for the worker
    bool announcedToClient = false; // a format list for it is out on the wire
    quint64 nextHostRequestId = 0;
    quint64 pendingHostRequest = 0; // the client's data request waiting for the worker
    bool discardClientResponse = false; // a request was reset away; its answer is dropped
    QTimer hostRequestTimer; // HostTextTimeoutMs
    QTimer announceTimer; // HostAnnounceIntervalMs coalescing
    QElapsedTimer lastAnnounce;
    QElapsedTimer clock;
    ClipboardLimits::RateLimit rate;
    bool rateEpisodeLogged = false;
    Counters counters;
    void sendFailResponse();
    void respondWithText(const QString &hostText);
    void refused(const char *reason, const char *direction, quint64 bytes);
    std::unique_ptr<QMimeData> clientData;
    // The format id of the one data request sent for the client's last format
    // list, so the response can be decoded as what was asked for: UTF-16 for
    // CF_UNICODETEXT, 8-bit for CF_TEXT/CF_OEMTEXT.
    uint32_t requestedClientFormat = 0;
    // OPT-072: the id the client gave "PNG" in its latest format list (0 = it has none), and the most the response
    // to the request on the wire may declare (the cliprdr thread reads it, the main thread sets it before the request).
    uint32_t clientPngFormatId = 0;
    bool requestedClientImage = false; // what the request on the wire (or the last one) asked for
    bool requestedClientPng = false;
    // OPT-077: the id the client gave "HTML Format" in its latest list (0 = none). A rich copy (HTML, usually with text) takes two
    // requests, text first, then HTML; the text waits in stagedClientText and both are written to the host together. If the HTML
    // fails or is refused the text alone is written, and a failed text request drops the HTML request.
    uint32_t clientHtmlFormatId = 0;
    bool requestedClientHtml = false;
    uint32_t followupHtmlFormat = 0; // HTML request to send once the text answer is in (0 = none)
    std::optional<QString> stagedClientText;
    void clearRichState()
    {
        followupHtmlFormat = 0;
        stagedClientText.reset();
    }
    void onClientHtml(const QByteArray &wire);
    void deliverClientData(const QString &text, const QByteArray &html);
    void finishRichWithoutHtml();
    std::atomic<qint64> responseLimit = ClipboardLimits::MaxWireBytes;
    bool isImageFormat(uint32_t formatId) const
    {
        return formatId == ClipboardImage::CfDib || formatId == ClipboardImage::CfDibV5 || (clientPngFormatId && formatId == clientPngFormatId);
    }
    void onClientImage(bool png, const QByteArray &data);

    // AUD-FIX5: requests to the client wait for its first picture, and there is
    // at most one on the wire. A stock wlfreerdp3 announces its clipboard once per
    // Wayland MIME type (Sol journal: three or four format lists within 6 ms at
    // connect, each drawing a request) and reads the local clipboard for every
    // request synchronously, which on an unlocked KDE desktop stopped it before
    // it ever sent its RDPGFX caps. All main-thread state.
    bool graphicsDelivered = false;
    uint32_t pendingFormat = 0; // text format of the latest list, not requested yet (0 = none)
    uint32_t outstandingFormat = 0; // the request on the wire (0 = none)
    bool pendingServerAnnounce = false;
    QTimer settleTimer; // Clipboard::FormatListSettleMs
    QTimer responseTimer; // Clipboard::ResponseTimeoutMs
    QTimer deliveredFallback; // Clipboard::DeliveredFallbackMs
    void requestPending();

    // AUD-FIX6 F1: a stock wlfreerdp3 3.22 refuses the first data request, and after a real
    // copy on its side never answers again and stops drawing (its synchronous Wayland read
    // under the lock its event loop needs). So:
    // - a client format list within EchoWindowMs of the server announcing or serving its own
    //   text is taken for that text coming back (Klipper re-owning what the client just
    //   pasted from us) and not requested; a response carrying exactly that text is not
    //   written back to the host either;
    // - after one refused (a stock client) or unanswered (any client) request the client is
    //   not asked again this session. Host-to-client copies keep working.
    bool clientRequestsDisabled = false;
    QElapsedTimer serverClipboardActivity; // last announce or serve of the host's text
    QString lastServerText; // what that was, as host text (LF)
    QByteArray lastServerHtml; // OPT-077: the HTML document last served (UTF-8), null when none
    bool isEcho() const
    {
        return serverClipboardActivity.isValid() && serverClipboardActivity.elapsed() < Clipboard::EchoWindowMs;
    }
    void disableClientRequests(const char *why);

    // AUD-P5: hand the PDU to the main thread and return at once. This used
    // to be a BlockingQueuedConnection: the cliprdr thread waited for the
    // main thread, and a main thread that was itself waiting for the session
    // (and so the cliprdr) thread to finish - connection teardown - never
    // came, a deadlock. Nothing the client is told depends on the handler's
    // result, so CHANNEL_RC_OK is returned without waiting.
    template<typename Packet, void (Private::*handler)(const Packet &)>
    static UINT postToMainThread(Clipboard *clipboard, Packet packet)
    {
        if (!clipboard || clipboard->d->closing) {
            return CHANNEL_RC_OK;
        }
        QMetaObject::invokeMethod(
            clipboard,
            [clipboard, packet = std::move(packet)]() {
                if (clipboard->d->closing) {
                    return;
                }
                ((*clipboard->d).*handler)(packet);
            },
            Qt::QueuedConnection);
        return CHANNEL_RC_OK;
    }

    static UINT clientFormatList(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST *formatList)
    {
        FormatList packet;
        for (uint32_t i = 0; formatList && i < formatList->numFormats; ++i) {
            packet.formatIds.append(formatList->formats[i].formatId);
            const char *name = formatList->formats[i].formatName;
            packet.formatNames.append(name ? QString::fromUtf8(name, qstrnlen(name, 64)) : QString());
        }
        return postToMainThread<FormatList, &Private::onClientFormatList>(static_cast<Clipboard *>(context->custom), std::move(packet));
    }

    static UINT clientFormatListResponse(CliprdrServerContext *, const CLIPRDR_FORMAT_LIST_RESPONSE *)
    {
        return CHANNEL_RC_OK;
    }

    static UINT clientFormatDataRequest(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_REQUEST *formatDataRequest)
    {
        FormatDataRequest packet{.requestedFormatId = formatDataRequest ? formatDataRequest->requestedFormatId : 0};
        return postToMainThread<FormatDataRequest, &Private::onClientFormatDataRequest>(static_cast<Clipboard *>(context->custom), packet);
    }

    static UINT clientFormatDataResponse(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_RESPONSE *formatDataResponse)
    {
        FormatDataResponse packet;
        auto *clipboard = static_cast<Clipboard *>(context->custom);
        if (clipboard && formatDataResponse) {
            packet.msgFlags = formatDataResponse->common.msgFlags;
            packet.declaredLength = formatDataResponse->common.dataLen;
            if (qint64(formatDataResponse->common.dataLen) > clipboard->d->responseLimit.load()) {
                // OPT-067: never copy what the client declared beyond the cap (OPT-072: the cap of what was asked for).
                packet.oversize = true;
            } else if (formatDataResponse->requestedFormatData && formatDataResponse->common.dataLen > 0) {
                packet.data = QByteArray(reinterpret_cast<const char *>(formatDataResponse->requestedFormatData), qsizetype(formatDataResponse->common.dataLen));
            }
        }
        return postToMainThread<FormatDataResponse, &Private::onClientFormatDataResponse>(clipboard, std::move(packet));
    }
};

Clipboard::Clipboard(RdpConnection *session)
    : QObject(nullptr)
    , d(std::make_unique<Private>(this))
{
    d->session = session;
    d->clock.start();

    d->hostRequestTimer.setSingleShot(true);
    d->hostRequestTimer.setInterval(HostTextTimeoutMs);
    connect(&d->hostRequestTimer, &QTimer::timeout, this, [this]() {
        if (!d->pendingHostRequest) {
            return;
        }
        d->pendingHostRequest = 0;
        ++d->counters.refusedTimeout;
        d->refused("timeout", "host-to-client", 0);
        d->sendFailResponse();
    });
    d->announceTimer.setSingleShot(true);
    connect(&d->announceTimer, &QTimer::timeout, this, [this]() {
        sendServerData();
    });

    d->settleTimer.setSingleShot(true);
    d->settleTimer.setInterval(FormatListSettleMs);
    connect(&d->settleTimer, &QTimer::timeout, this, [this]() {
        d->requestPending();
    });
    d->responseTimer.setSingleShot(true);
    d->responseTimer.setInterval(ResponseTimeoutMs);
    connect(&d->responseTimer, &QTimer::timeout, this, [this]() {
        qCDebug(KRDP) << "Client did not answer the clipboard data request for format" << d->outstandingFormat << "within" << ResponseTimeoutMs << "ms";
        d->outstandingFormat = 0;
        // AUD-FIX6 F1: a client that does not answer may be stuck on its own clipboard;
        // asking it again is what froze wlfreerdp3.
        d->disableClientRequests("did not answer");
    });
    d->deliveredFallback.setSingleShot(true);
    d->deliveredFallback.setInterval(DeliveredFallbackMs);
    connect(&d->deliveredFallback, &QTimer::timeout, this, [this]() {
        qCInfo(KRDP) << "The client acknowledged no frame" << DeliveredFallbackMs << "ms after its first clipboard format list; serving the clipboard anyway";
        setGraphicsDelivered();
    });
}

Clipboard::~Clipboard()
{
    const auto &c = d->counters;
    if (c.announces || c.served || c.clientCopies || c.refusedPolicy || c.refusedOversize || c.refusedRate || c.refusedTimeout || c.refusedBusy
        || c.hostImages || c.clientImages || c.refusedMalformed || c.hostHtml || c.clientHtml) {
        // Counts only, never contents (OPT-067).
        qCInfo(KRDP).nospace() << "Clipboard: closed announces=" << c.announces << " served=" << c.served << " clientCopies=" << c.clientCopies
                               << " refused(policy=" << c.refusedPolicy << " oversize=" << c.refusedOversize << " rate=" << c.refusedRate
                               << " timeout=" << c.refusedTimeout << " busy=" << c.refusedBusy << " malformed=" << c.refusedMalformed
                               << ") images(host=" << c.hostImages << " client=" << c.clientImages << ") html(host=" << c.hostHtml << " client=" << c.clientHtml
                               << ") bytes(host=" << c.hostBytes
                               << " client=" << c.clientBytes << ")";
    }
    // setServerData() took ownership of the last announcement; every earlier
    // one was freed by its successor.
    delete d->serverData;
    if (auto *context = d->clipContext.exchange(nullptr)) {
        if (d->ownsContext) {
            cliprdr_server_context_free(context);
        }
    }
}

void Clipboard::installClientCallbacks(CliprdrServerContext *context, Clipboard *clipboard)
{
    context->custom = clipboard;
    context->ClientFormatList = Private::clientFormatList;
    context->ClientFormatListResponse = Private::clientFormatListResponse;
    context->ClientFormatDataRequest = Private::clientFormatDataRequest;
    context->ClientFormatDataResponse = Private::clientFormatDataResponse;
}

bool Clipboard::initialize()
{
    if (d->clipContext) {
        return true;
    }

    auto peerContext = reinterpret_cast<PeerContext *>(d->session->rdpPeer()->context);

    auto *context = cliprdr_server_context_new(peerContext->virtualChannelManager);
    if (!context) {
        qCWarning(KRDP) << "Failed creating Clipboard context";
        return false;
    }

    context->useLongFormatNames = TRUE;
    context->streamFileClipEnabled = FALSE;
    context->fileClipNoFilePaths = FALSE;
    context->canLockClipData = FALSE;
    context->hasHugeFileSupport = FALSE;

    context->rdpcontext = d->session->rdpPeer()->context;
    installClientCallbacks(context, this);
    d->clipContext = context;

    // returns 0 on success
    // https://pub.freerdp.com/api/server_2cliprdr__main_8c.html#ab4e8a28c6b4371c2a5f34e8716ab1e9e
    if (context->Start(context)) {
        qCWarning(KRDP) << "Could not start Clipboard context";
        return false;
    };

    d->enabled = true;

    return true;
}

bool Clipboard::enabled()
{
    return d->enabled;
}

void Clipboard::close()
{
    d->closing = true;
    d->enabled = false;
    auto *context = d->clipContext.load();
    if (!context) {
        return;
    }

    std::lock_guard lock(d->sendMutex);
    if (context->Stop(context)) {
        qCWarning(KRDP) << "Could not stop Clipboard context";
    };
}

void Clipboard::setServerData(const QMimeData *data)
{
    if (d->serverData) {
        delete d->serverData;
    }

    d->serverData = data;
    QMetaObject::invokeMethod(this, &Clipboard::sendServerData, Qt::QueuedConnection);
}

std::unique_ptr<QMimeData> Clipboard::getClipboard() const
{
    return std::move(d->clientData);
}

void Clipboard::setGraphicsDelivered()
{
    if (d->graphicsDelivered || d->closing) {
        return;
    }
    d->graphicsDelivered = true;
    d->deliveredFallback.stop();
    if (d->pendingServerAnnounce) {
        sendServerData();
    }
    d->requestPending();
}

void Clipboard::sendServerData()
{
    if (!d->serverData && !d->hostAnnounced) {
        return;
    }
    if (!d->graphicsDelivered) {
        // AUD-FIX5: announced once the client has its picture (setGraphicsDelivered()).
        d->pendingServerAnnounce = true;
        return;
    }
    if (!d->serverData && d->lastAnnounce.isValid() && d->lastAnnounce.elapsed() < HostAnnounceIntervalMs) {
        // OPT-067: at most one format list per HostAnnounceIntervalMs; the newest copy goes out then.
        if (!d->announceTimer.isActive()) {
            d->announceTimer.start(HostAnnounceIntervalMs - int(d->lastAnnounce.elapsed()));
        }
        return;
    }
    d->pendingServerAnnounce = false;

    // Text (the in-process route and a text copy on the host) and, OPT-072, an image: the registered "PNG"
    // format and CF_DIB, so a client picks the one it can paste.
    // OPT-077: formatted text, "HTML Format" beside the text (the receiving application picks the richest it understands).
    // The same combination rule as the worker applies (ClipboardHtml::announce): HTML goes with its text and drops a picture,
    // HTML next to a picture and no text is the picture.
    static char pngName[] = "PNG";
    static char htmlName[] = "HTML Format";
    CLIPRDR_FORMAT formats[4] = {};
    uint32_t count = 0;
    const bool hasText = d->serverData || d->hostText;
    const auto choice = ClipboardHtml::announce(hasText, d->hostHtml, d->hostImage);
    // Richest first (a format list is in the owner's order of preference): "HTML Format", then the plain text.
    if (choice.html) {
        formats[count].formatId = ClipboardHtml::HtmlFormatId;
        formats[count++].formatName = htmlName;
    }
    if (choice.text) {
        formats[count++].formatId = CF_UNICODETEXT;
    }
    if (choice.image) {
        formats[count].formatId = ClipboardImage::PngFormatId;
        formats[count++].formatName = pngName;
        formats[count++].formatId = ClipboardImage::CfDib;
    }

    CLIPRDR_FORMAT_LIST formatList = {};
    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.common.msgFlags = 0;
    formatList.numFormats = count;
    formatList.formats = formats;
    std::lock_guard lock(d->sendMutex);
    if (auto *context = d->clipContext.load(); context && d->enabled) {
        if (d->serverData) {
            d->lastServerText = ClipboardText::toHost(d->serverData->text());
        }
        d->serverClipboardActivity.start();
        d->lastAnnounce.start();
        d->announcedToClient = true;
        ++d->counters.announces;
        context->ServerFormatList(context, &formatList);
    }
}

void Clipboard::Private::onClientFormatList(const FormatList &formatList)
{
    // One data request per format list, whatever text formats it announces:
    // a client copy typically lists CF_UNICODETEXT and CF_TEXT together, and
    // a request for each of them meant two clipboard writes and two announces
    // on the host per copy (the x2 amplifier in the announce storm). Prefer
    // CF_UNICODETEXT, then CF_TEXT, then CF_OEMTEXT.
    uint32_t wanted = 0;
    // OPT-072: images, only when there is no text: a copy with both is taken as text (a spreadsheet range or a
    // web selection is text first). PNG over DIBV5 over DIB.
    uint32_t pngId = 0;
    bool hasDibV5 = false;
    bool hasDib = false;
    uint32_t htmlId = 0;
    for (qsizetype i = 0; i < formatList.formatIds.size(); ++i) {
        const auto formatId = formatList.formatIds.at(i);
        if (formatId >= 0xC000 && i < formatList.formatNames.size() && ClipboardImage::isPngFormatName(formatList.formatNames.at(i))) {
            pngId = formatId;
        }
        if (formatId >= 0xC000 && i < formatList.formatNames.size() && ClipboardHtml::isHtmlFormatName(formatList.formatNames.at(i))) {
            htmlId = formatId;
        }
        hasDibV5 = hasDibV5 || formatId == ClipboardImage::CfDibV5;
        hasDib = hasDib || formatId == ClipboardImage::CfDib;
    }
    clientPngFormatId = pngId; // per list: a client may number its registered formats afresh
    clientHtmlFormatId = htmlId;
    if (outstandingFormat != 0 && requestedClientHtml) {
        // OPT-077: the HTML of an older copy is still on its way; it is not this list's, so its answer is dropped.
        discardClientResponse = true;
    }
    clearRichState(); // an earlier list's half-fetched copy is stale
    const uint32_t imageWanted = pngId ? pngId : hasDibV5 ? ClipboardImage::CfDibV5 : hasDib ? ClipboardImage::CfDib : 0;
    for (const auto formatId : formatList.formatIds) {
        switch (formatId) {
        case CF_UNICODETEXT:
            wanted = formatId;
            break;
        case CF_TEXT:
            if (wanted != CF_UNICODETEXT) {
                wanted = formatId;
            }
            break;
        case CF_OEMTEXT:
            if (wanted == 0) {
                wanted = formatId;
            }
            break;
        default:
            break;
        }
    }

    // Acknowledge the client's format list first (MS-RDPECLIP 3.1.5.2.3:
    // the Format List Response precedes any Format Data Request).
    std::lock_guard lock(sendMutex);
    auto *context = clipContext.load();
    const bool canSend = context && enabled;
    CLIPRDR_FORMAT_LIST_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    if (canSend) {
        context->ServerFormatListResponse(context, &response);
    }

    if (!clientToHostAllowed) {
        // OPT-067: the planner denies the client's copies in this state (acknowledged, never requested).
        qCDebug(KRDP) << "Client announced" << formatList.formatIds.size() << "clipboard formats; the client to host direction is not allowed now";
        pendingFormat = 0;
        clearRichState();
        return;
    }
    // OPT-077: HTML with its text (or HTML alone); a picture beside HTML and no text is the picture; text beats a picture.
    const auto choice = ClipboardHtml::request(wanted != 0, htmlId != 0, imageWanted != 0);
    if (choice.html) {
        // Text first (what an application without HTML support would paste), then the HTML; both land on the host together.
        followupHtmlFormat = wanted ? htmlId : 0;
        if (!wanted) {
            wanted = htmlId;
        }
    } else if (wanted == 0) {
        wanted = imageWanted;
    }
    if (wanted == 0) {
        qCDebug(KRDP) << "Client announced" << formatList.formatIds.size() << "clipboard formats, none of them text, HTML or an image; not requesting data";
        pendingFormat = 0; // what an earlier list offered is gone
        return;
    }
    if (clientRequestsDisabled) {
        qCDebug(KRDP) << "Client announced" << formatList.formatIds.size() << "clipboard formats; not requesting them (the client failed a request before)";
        pendingFormat = 0;
        return;
    }
    if (isEcho()) {
        // AUD-FIX6 F1: the host's own text coming back, not a copy on the client.
        qCDebug(KRDP) << "Client announced" << formatList.formatIds.size() << "clipboard formats" << serverClipboardActivity.elapsed()
                      << "ms after the host's clipboard went to it; taking it for an echo and not requesting it";
        pendingFormat = 0;
        clearRichState();
        return;
    }

    // AUD-FIX5: the request itself goes out from requestPending(): once the client has
    // its picture, after the lists stop coming (FormatListSettleMs), for the latest list
    // only and never while an earlier request is unanswered.
    requestedClientFormat = wanted;
    pendingFormat = wanted;
    if (!graphicsDelivered) {
        qCDebug(KRDP) << "Client announced" << formatList.formatIds.size() << "clipboard formats; format" << wanted
                      << "will be requested once the client has a picture";
        if (!deliveredFallback.isActive()) {
            deliveredFallback.start();
        }
        return;
    }
    qCDebug(KRDP) << "Client announced" << formatList.formatIds.size() << "clipboard formats; the one wanted is format" << wanted;
    settleTimer.start();
}

void Clipboard::Private::disableClientRequests(const char *why)
{
    pendingFormat = 0;
    clearRichState();
    settleTimer.stop();
    if (clientRequestsDisabled) {
        return;
    }
    clientRequestsDisabled = true;
    qCInfo(KRDP).nospace() << "Clipboard: the client " << why
                           << " a clipboard data request; not asking it for its clipboard again this session (copies from the host to the client still work)";
}

void Clipboard::Private::requestPending()
{
    if (closing || !graphicsDelivered || pendingFormat == 0 || settleTimer.isActive()) {
        return;
    }
    if (clientRequestsDisabled) {
        pendingFormat = 0;
        return;
    }
    if (outstandingFormat != 0) {
        return; // the response (or ResponseTimeoutMs) comes back here
    }
    if (!q->isSignalConnected(QMetaMethod::fromSignal(&Clipboard::clientDataChanged))) {
        // The console host has no session clipboard to write to: asking the client
        // would only make it read its own clipboard for nothing.
        qCDebug(KRDP) << "Nothing here takes the client's clipboard; not requesting it";
        pendingFormat = 0;
        return;
    }

    std::lock_guard lock(sendMutex);
    auto *context = clipContext.load();
    if (!context || !enabled) {
        return;
    }
    qCDebug(KRDP) << "Requesting the client's clipboard as format" << pendingFormat;
    requestedClientFormat = pendingFormat;
    outstandingFormat = pendingFormat;
    pendingFormat = 0;
    requestedClientImage = isImageFormat(requestedClientFormat);
    requestedClientPng = requestedClientImage && clientPngFormatId && requestedClientFormat == clientPngFormatId;
    requestedClientHtml = clientHtmlFormatId && requestedClientFormat == clientHtmlFormatId;
    responseLimit = requestedClientImage ? ClipboardLimits::MaxImageWireBytes : requestedClientHtml ? ClipboardLimits::MaxHtmlWireBytes : ClipboardLimits::MaxWireBytes;
    CLIPRDR_FORMAT_DATA_REQUEST formatDataRequest{.common = CLIPRDR_HEADER({.msgType = CB_FORMAT_DATA_REQUEST, .msgFlags = 0, .dataLen = 4}),
                                                  .requestedFormatId = requestedClientFormat};
    context->ServerFormatDataRequest(context, &formatDataRequest);
    responseTimer.start();
}

void Clipboard::Private::sendFailResponse()
{
    std::lock_guard lock(sendMutex);
    auto *context = clipContext.load();
    if (!context || !enabled) {
        return;
    }
    CLIPRDR_FORMAT_DATA_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_FAIL;
    response.common.dataLen = 0;
    // A failure carries no data; a valid pointer anyway, so a channel implementation that copies dataLen (0) bytes from it
    // never sees a null source (FreeRDP's cliprdr server does exactly that).
    static const BYTE none = 0;
    response.requestedFormatData = &none;
    context->ServerFormatDataResponse(context, &response);
}

void Clipboard::Private::refused(const char *reason, const char *direction, quint64 bytes)
{
    // One Info line per refusal, counts only (OPT-067).
    qCInfo(KRDP).nospace() << "Clipboard: refused (" << reason << ") direction=" << direction << " bytes=" << bytes;
}

// Called with sendMutex NOT held; the context is checked under it.
void Clipboard::Private::respondWithText(const QString &hostText)
{
    std::lock_guard lock(sendMutex);
    auto *context = clipContext.load();
    if (!context || !enabled) {
        return;
    }
    // CF_UNICODETEXT is CRLF on the wire and requires null-terminated UTF-16LE.
    const auto text = ClipboardText::toWire(hostText);
    if (qint64(text.length() + 1) * 2 > ClipboardLimits::MaxWireBytes) {
        // OPT-067: the CRLF form of a text near the cap can pass the wire cap; refuse it, like an oversize text.
        ++counters.refusedOversize;
        refused("oversize", "host-to-client", quint64((text.length() + 1) * 2));
        CLIPRDR_FORMAT_DATA_RESPONSE refusal = {};
        refusal.common.msgType = CB_FORMAT_DATA_RESPONSE;
        refusal.common.msgFlags = CB_RESPONSE_FAIL;
        static const BYTE none = 0;
        refusal.requestedFormatData = &none;
        context->ServerFormatDataResponse(context, &refusal);
        return;
    }
    QByteArray utf16Data(reinterpret_cast<const char *>(text.utf16()), (text.length() + 1) * 2);

    CLIPRDR_FORMAT_DATA_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    response.common.dataLen = utf16Data.size();
    response.requestedFormatData = reinterpret_cast<const BYTE *>(utf16Data.constData());

    qCDebug(KRDP) << "Serving host clipboard text to the client:" << text.length() << "characters";
    lastServerText = ClipboardText::toHost(hostText);
    serverClipboardActivity.start();
    ++counters.served;
    counters.hostBytes += quint64(hostText.toUtf8().size());
    context->ServerFormatDataResponse(context, &response);
}

void Clipboard::Private::onClientFormatDataRequest(const FormatDataRequest &formatDataRequest)
{
    if (hostAnnounced) {
        // OPT-067: delayed rendering. The text is fetched from the worker now, not read from a copy.
        // OPT-072: an image the same way, as "PNG" or CF_DIB, only when the announcement covered one.
        const auto requested = formatDataRequest.requestedFormatId;
        const bool wantText = requested == CF_UNICODETEXT && hostText;
        const bool wantImage = (requested == ClipboardImage::PngFormatId || requested == ClipboardImage::CfDib) && hostImage;
        const bool wantHtml = requested == ClipboardHtml::HtmlFormatId && hostHtml;
        if ((!wantText && !wantImage && !wantHtml) || !hostToClientAllowed) {
            ++counters.refusedPolicy;
            sendFailResponse();
            return;
        }
        if (pendingHostRequest) {
            ++counters.refusedBusy;
            refused("busy", "host-to-client", 0);
            sendFailResponse();
            return;
        }
        pendingHostRequest = ++nextHostRequestId;
        pendingHostFormat = requested;
        hostRequestTimer.start();
        if (wantImage) {
            Q_EMIT q->hostImageRequested(pendingHostRequest);
        } else if (wantHtml) {
            Q_EMIT q->hostHtmlRequested(pendingHostRequest);
        } else {
            Q_EMIT q->hostTextRequested(pendingHostRequest);
        }
        return;
    }
    if (!serverData || formatDataRequest.requestedFormatId != CF_UNICODETEXT) {
        sendFailResponse();
        return;
    }
    respondWithText(serverData->text());
}

void Clipboard::Private::finishRichWithoutHtml()
{
    // The HTML of a rich copy was refused, oversize or not answered: the plain text that came with it still lands on the host.
    const auto text = stagedClientText;
    clearRichState();
    if (text && !text->isEmpty()) {
        deliverClientData(*text, {});
    }
}

void Clipboard::Private::onClientHtml(const QByteArray &wire)
{
    const auto unwrapped = ClipboardHtml::unwrapMsHtml(wire);
    if (!unwrapped.ok()) {
        if (unwrapped.error == ClipboardHtml::Error::TooLarge) {
            ++counters.refusedOversize;
        } else {
            ++counters.refusedMalformed;
        }
        // A reason and a size, never the document (OPT-077).
        qCInfo(KRDP).nospace() << "Clipboard: refused (" << ClipboardHtml::errorName(unwrapped.error) << ") direction=client-to-host kind=html bytes=" << wire.size();
        finishRichWithoutHtml();
        return;
    }
    const QString text = stagedClientText.value_or(QString());
    clearRichState();
    deliverClientData(text, unwrapped.data);
}

void Clipboard::Private::deliverClientData(const QString &text, const QByteArray &html)
{
    const quint64 utf8Bytes = quint64(text.toUtf8().size());
    if (utf8Bytes > quint64(ClipboardLimits::MaxTextBytes)) {
        ++counters.refusedOversize;
        refused("oversize", "client-to-host", utf8Bytes);
        return;
    }
    const quint64 totalBytes = utf8Bytes + quint64(html.size());
    if (!rate.accept(clock.elapsed(), qint64(totalBytes))) {
        ++counters.refusedRate;
        if (!rateEpisodeLogged) {
            rateEpisodeLogged = true;
            refused("rate", "client-to-host", totalBytes);
        }
        return;
    }
    rateEpisodeLogged = false;

    if (!html.isEmpty()) {
        // OPT-077: text and HTML in one offer. The host's own copy coming back (Klipper re-owning what we served) is judged by
        // the format-list echo window; a body identical to what was just served is dropped here as well.
        if (!lastServerHtml.isNull() && html == lastServerHtml && (text.isEmpty() || ClipboardText::toHost(text) == lastServerText)) {
            qCDebug(KRDP) << "Client clipboard HTML is the host's own; not writing it back";
            return;
        }
        clientData.reset(buildClipboardMime(text.isEmpty() ? QString() : ClipboardText::toHost(text), html, false));
        qCDebug(KRDP) << "Client clipboard formatted text received:" << html.size() << "bytes of HTML," << text.length() << "characters of text";
        ++counters.clientHtml;
        counters.clientBytes += totalBytes;
        Q_EMIT q->clientDataChanged();
        return;
    }

    if (text.isEmpty()) {
        qCDebug(KRDP) << "Client clipboard text is empty";
        clientData.reset();
        ++counters.clientCopies;
        Q_EMIT q->clientDataChanged();
        return;
    }

    // AUD-FIX6 F1: the host's own text coming back; writing it would only announce it again.
    if (!lastServerText.isNull() && ClipboardText::toHost(text) == lastServerText) {
        qCDebug(KRDP) << "Client clipboard text is the host's own; not writing it back";
        return;
    }

    // Stored LF on the host, whatever the wire carried.
    clientData.reset(new QMimeData());
    clientData->setText(ClipboardText::toHost(text));
    qCDebug(KRDP) << "Client clipboard text received:" << clientData->text().length() << "characters";
    ++counters.clientCopies;
    counters.clientBytes += utf8Bytes;
    Q_EMIT q->clientDataChanged();
}

void Clipboard::Private::onClientFormatDataResponse(const FormatDataResponse &formatDataResponse)
{
    // The answer to the request on the wire (or a late one, decoded as the last asked-for
    // format); a newer format list may be waiting for its request.
    if (outstandingFormat != 0) {
        requestedClientFormat = outstandingFormat;
    }
    outstandingFormat = 0;
    responseTimer.stop();
    const auto next = qScopeGuard([this]() {
        requestPending();
    });

    if (discardClientResponse) {
        // OPT-067: the request was reset away (denial, handover); its answer is not wanted.
        discardClientResponse = false;
        clearRichState();
        return;
    }
    if (!clientToHostAllowed) {
        ++counters.refusedPolicy;
        clearRichState();
        return;
    }
    if (formatDataResponse.oversize) {
        ++counters.refusedOversize;
        refused("oversize", "client-to-host", formatDataResponse.declaredLength);
        finishRichWithoutHtml();
        return;
    }

    if (!(formatDataResponse.msgFlags & CB_RESPONSE_OK)) {
        qCDebug(KRDP) << "Client refused the clipboard data request for format" << requestedClientFormat;
        // AUD-FIX6 F1: a stock client that refuses is not asked again (wlfreerdp3 3.22
        // refuses the first request and hangs on a later one). Our own client may refuse one
        // (its clipboard changed under it) and is asked again next time.
        if (requestedClientHtml) {
            // The text of this copy is already here: it is still a copy. A refused HTML request does not disable requests.
            finishRichWithoutHtml();
            return;
        }
        clearRichState();
        if (!session || !session->isOwnClient()) {
            disableClientRequests("refused");
        }
        return;
    }

    if (requestedClientHtml) {
        onClientHtml(formatDataResponse.data);
        return;
    }

    if (requestedClientImage) {
        onClientImage(requestedClientPng, formatDataResponse.data);
        return;
    }

    const auto *bytes = formatDataResponse.data.constData();
    const auto length = qsizetype(formatDataResponse.data.size());
    QString text;
    if (requestedClientFormat == CF_UNICODETEXT) {
        // UTF-16LE with a null terminator; anything shorter than the
        // terminator is an empty string.
        if (length >= 2) {
            text = QString::fromUtf16(reinterpret_cast<const char16_t *>(bytes), length / 2 - 1);
        }
    } else {
        // CF_TEXT / CF_OEMTEXT: 8-bit with a null terminator.
        if (length >= 1) {
            text = QString::fromLatin1(bytes, length - 1);
        }
    }

    if (followupHtmlFormat) {
        // OPT-077: the text of a rich copy. Held back; the HTML is requested next and both are written together.
        if (quint64(text.toUtf8().size()) > quint64(ClipboardLimits::MaxTextBytes)) {
            ++counters.refusedOversize;
            refused("oversize", "client-to-host", quint64(text.toUtf8().size()));
            // The HTML alone is still worth having.
        } else if (!lastServerText.isNull() && !text.isEmpty() && ClipboardText::toHost(text) == lastServerText) {
            qCDebug(KRDP) << "Client clipboard text is the host's own; not writing it back";
            clearRichState();
            return;
        } else {
            stagedClientText = text;
        }
        pendingFormat = std::exchange(followupHtmlFormat, 0);
        return; // requestPending() (the scope guard) sends the HTML request
    }
    clearRichState();
    deliverClientData(text, {});
}

void Clipboard::setDirections(bool hostToClient, bool clientToHost)
{
    const bool hostWas = d->hostToClientAllowed;
    const bool clientWas = d->clientToHostAllowed;
    d->hostToClientAllowed = hostToClient;
    d->clientToHostAllowed = clientToHost;
    if (hostWas && !hostToClient) {
        resetHostSide();
    }
    if (clientWas && !clientToHost) {
        resetClientSide();
    }
}

bool Clipboard::hostToClientAllowed() const
{
    return d->hostToClientAllowed;
}

bool Clipboard::clientToHostAllowed() const
{
    return d->clientToHostAllowed;
}

void Clipboard::announceHostText()
{
    announceHost(true, false);
}

void Clipboard::announceHost(bool text, bool image, bool html)
{
    if (d->closing || !d->hostToClientAllowed || (!text && !image && !html)) {
        return;
    }
    d->hostAnnounced = true;
    d->hostText = text;
    d->hostImage = image;
    d->hostHtml = html;
    // The text the client may have of the previous copy is no longer the host's.
    d->lastServerText = QString();
    d->lastServerHtml = QByteArray();
    sendServerData();
}

void Clipboard::provideHostText(quint64 requestId, const QString &text, bool ok)
{
    if (d->closing || !requestId || requestId != d->pendingHostRequest) {
        return; // a late answer for a request that timed out or was reset
    }
    d->pendingHostRequest = 0;
    d->hostRequestTimer.stop();
    if (!ok || !d->hostToClientAllowed) {
        ++d->counters.refusedPolicy;
        d->sendFailResponse();
        return;
    }
    d->respondWithText(text);
}

void Clipboard::provideHostHtml(quint64 requestId, const QByteArray &html, bool ok)
{
    if (d->closing || !requestId || requestId != d->pendingHostRequest) {
        return; // a late answer for a request that timed out or was reset
    }
    d->pendingHostRequest = 0;
    d->hostRequestTimer.stop();
    if (!ok || !d->hostToClientAllowed || html.isEmpty()) {
        ++d->counters.refusedPolicy;
        d->sendFailResponse();
        return;
    }
    // The worker has bounded this document; wrap it with exact offsets, and refuse what cannot be wrapped (an answer from another
    // process is checked again before it reaches a client).
    const auto wrapped = ClipboardHtml::wrapForWire(html);
    if (!wrapped.ok()) {
        if (wrapped.error == ClipboardHtml::Error::TooLarge) {
            ++d->counters.refusedOversize;
        } else {
            ++d->counters.refusedMalformed;
        }
        d->refused(ClipboardHtml::errorName(wrapped.error), "host-to-client", quint64(html.size()));
        d->sendFailResponse();
        return;
    }
    std::lock_guard lock(d->sendMutex);
    auto *context = d->clipContext.load();
    if (!context || !d->enabled) {
        return;
    }
    CLIPRDR_FORMAT_DATA_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    response.common.dataLen = quint32(wrapped.data.size());
    response.requestedFormatData = reinterpret_cast<const BYTE *>(wrapped.data.constData());
    qCDebug(KRDP) << "Serving host clipboard HTML to the client:" << wrapped.data.size() << "bytes";
    d->lastServerHtml = html;
    d->serverClipboardActivity.start();
    ++d->counters.hostHtml;
    d->counters.hostBytes += quint64(wrapped.data.size());
    context->ServerFormatDataResponse(context, &response);
}

void Clipboard::provideHostImage(quint64 requestId, const QByteArray &png, bool ok)
{
    if (d->closing || !requestId || requestId != d->pendingHostRequest) {
        return; // a late answer for a request that timed out or was reset
    }
    d->pendingHostRequest = 0;
    d->hostRequestTimer.stop();
    const auto format = d->pendingHostFormat;
    if (!ok || !d->hostToClientAllowed || png.isEmpty()) {
        ++d->counters.refusedPolicy;
        d->sendFailResponse();
        return;
    }
    // The worker has checked this PNG; check again what arrives from another process before it reaches a client.
    QByteArray payload;
    if (format == ClipboardImage::CfDib) {
        const auto converted = ClipboardImage::pngToDib(png);
        if (!converted.ok()) {
            ++d->counters.refusedOversize;
            d->refused(ClipboardImage::errorName(converted.error), "host-to-client", quint64(png.size()));
            d->sendFailResponse();
            return;
        }
        payload = converted.data;
    } else {
        const auto checked = ClipboardImage::validatePng(png);
        if (!checked.ok()) {
            ++d->counters.refusedOversize;
            d->refused(ClipboardImage::errorName(checked.error), "host-to-client", quint64(png.size()));
            d->sendFailResponse();
            return;
        }
        payload = png;
    }
    std::lock_guard lock(d->sendMutex);
    auto *context = d->clipContext.load();
    if (!context || !d->enabled) {
        return;
    }
    CLIPRDR_FORMAT_DATA_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    response.common.dataLen = quint32(payload.size());
    response.requestedFormatData = reinterpret_cast<const BYTE *>(payload.constData());
    qCDebug(KRDP) << "Serving host clipboard image to the client:" << payload.size() << "bytes as format" << format;
    d->lastServerText = QString();
    d->serverClipboardActivity.start();
    ++d->counters.hostImages;
    d->counters.hostBytes += quint64(payload.size());
    context->ServerFormatDataResponse(context, &response);
}

void Clipboard::Private::onClientImage(bool png, const QByteArray &data)
{
    // OPT-072: the client's picture. PNG is structure-checked and kept as it is; a DIB is parsed, bounded and
    // converted. Nothing is decoded before the declared dimensions have been checked against the caps.
    const auto converted = png ? ClipboardImage::validatePng(data) : ClipboardImage::dibToPng(data);
    if (!converted.ok()) {
        if (converted.error == ClipboardImage::Error::TooLarge || converted.error == ClipboardImage::Error::TooManyPixels) {
            ++counters.refusedOversize;
        } else {
            ++counters.refusedMalformed;
        }
        qCInfo(KRDP).nospace() << "Clipboard: refused (" << ClipboardImage::errorName(converted.error) << ") direction=client-to-host kind=image bytes=" << data.size()
                               << " size=" << converted.size.width() << "x" << converted.size.height();
        return;
    }
    if (!rate.accept(clock.elapsed(), converted.data.size())) {
        ++counters.refusedRate;
        if (!rateEpisodeLogged) {
            rateEpisodeLogged = true;
            refused("rate", "client-to-host", quint64(converted.data.size()));
        }
        return;
    }
    rateEpisodeLogged = false;
    clientData.reset(new QMimeData());
    clientData->setData(u"image/png"_s, converted.data);
    qCDebug(KRDP) << "Client clipboard image received:" << converted.size.width() << "x" << converted.size.height() << "," << converted.data.size() << "bytes";
    ++counters.clientImages;
    counters.clientBytes += quint64(converted.data.size());
    Q_EMIT q->clientDataChanged();
}

void Clipboard::resetHostSide()
{
    d->announceTimer.stop();
    delete d->serverData;
    d->serverData = nullptr;
    const bool announced = d->announcedToClient;
    d->hostAnnounced = false;
    d->hostText = false;
    d->hostImage = false;
    d->hostHtml = false;
    d->lastServerHtml = QByteArray();
    d->pendingHostFormat = 0;
    d->announcedToClient = false;
    d->pendingServerAnnounce = false;
    d->lastServerText = QString();
    d->serverClipboardActivity = QElapsedTimer();
    d->lastAnnounce = QElapsedTimer();
    if (d->pendingHostRequest) {
        d->pendingHostRequest = 0;
        d->hostRequestTimer.stop();
        d->sendFailResponse();
    }
    if (announced && !d->closing) {
        // An empty format list withdraws the offer (MS-RDPECLIP 2.2.3.1).
        CLIPRDR_FORMAT_LIST formatList = {};
        formatList.common.msgType = CB_FORMAT_LIST;
        formatList.common.msgFlags = 0;
        formatList.numFormats = 0;
        formatList.formats = nullptr;
        std::lock_guard lock(d->sendMutex);
        if (auto *context = d->clipContext.load(); context && d->enabled) {
            context->ServerFormatList(context, &formatList);
        }
    }
}

void Clipboard::resetClientSide()
{
    d->clientData.reset();
    d->pendingFormat = 0;
    d->clearRichState();
    d->settleTimer.stop();
    d->responseLimit = ClipboardLimits::MaxWireBytes;
    if (d->outstandingFormat != 0) {
        d->outstandingFormat = 0;
        d->responseTimer.stop();
        d->discardClientResponse = true;
    }
}

Clipboard::Counters Clipboard::counters() const
{
    return d->counters;
}

void Clipboard::attachContextForTest(CliprdrServerContext *context)
{
    d->ownsContext = false;
    d->clipContext = context;
    d->enabled = true;
}
}
