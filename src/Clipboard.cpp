// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Clipboard.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/server/cliprdr.h>

#include <atomic>
#include <mutex>

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
    };
    struct FormatDataRequest {
        uint32_t requestedFormatId = 0;
    };
    struct FormatDataResponse {
        uint16_t msgFlags = 0;
        QByteArray data;
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
    std::unique_ptr<QMimeData> clientData;
    // The format id of the one data request sent for the client's last format
    // list, so the response can be decoded as what was asked for: UTF-16 for
    // CF_UNICODETEXT, 8-bit for CF_TEXT/CF_OEMTEXT.
    uint32_t requestedClientFormat = 0;

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
        if (formatDataResponse) {
            packet.msgFlags = formatDataResponse->common.msgFlags;
            if (formatDataResponse->requestedFormatData && formatDataResponse->common.dataLen > 0) {
                packet.data = QByteArray(reinterpret_cast<const char *>(formatDataResponse->requestedFormatData), qsizetype(formatDataResponse->common.dataLen));
            }
        }
        return postToMainThread<FormatDataResponse, &Private::onClientFormatDataResponse>(static_cast<Clipboard *>(context->custom), std::move(packet));
    }
};

Clipboard::Clipboard(RdpConnection *session)
    : QObject(nullptr)
    , d(std::make_unique<Private>(this))
{
    d->session = session;
}

Clipboard::~Clipboard()
{
    // setServerData() took ownership of the last announcement; every earlier
    // one was freed by its successor.
    delete d->serverData;
    if (auto *context = d->clipContext.exchange(nullptr)) {
        cliprdr_server_context_free(context);
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

void Clipboard::sendServerData()
{
    if (!d->serverData) {
        return;
    }

    CLIPRDR_FORMAT format = {};
    format.formatId = CF_UNICODETEXT;
    format.formatName = nullptr;

    CLIPRDR_FORMAT_LIST formatList = {};
    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.common.msgFlags = 0;
    formatList.numFormats = 1;
    formatList.formats = &format;
    std::lock_guard lock(d->sendMutex);
    if (auto *context = d->clipContext.load(); context && d->enabled) {
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

    if (wanted == 0) {
        qCDebug(KRDP) << "Client announced" << formatList.formatIds.size() << "clipboard formats, none of them text; not requesting data";
        return;
    }

    qCDebug(KRDP) << "Client announced" << formatList.formatIds.size() << "clipboard formats; requesting text as format" << wanted;
    requestedClientFormat = wanted;
    CLIPRDR_FORMAT_DATA_REQUEST formatDataRequest{.common = CLIPRDR_HEADER({.msgType = CB_FORMAT_DATA_REQUEST, .msgFlags = 0, .dataLen = 4}),
                                                  .requestedFormatId = wanted};
    if (canSend) {
        context->ServerFormatDataRequest(context, &formatDataRequest);
    }
}

void Clipboard::Private::onClientFormatDataRequest(const FormatDataRequest &formatDataRequest)
{
    std::lock_guard lock(sendMutex);
    auto *context = clipContext.load();
    if (!context || !enabled) {
        return;
    }
    if (!serverData || formatDataRequest.requestedFormatId != CF_UNICODETEXT) {
        CLIPRDR_FORMAT_DATA_RESPONSE response = {};
        response.common.msgType = CB_FORMAT_DATA_RESPONSE;
        response.common.msgFlags = CB_RESPONSE_FAIL;
        response.common.dataLen = 0;
        response.requestedFormatData = nullptr;
        context->ServerFormatDataResponse(context, &response);
        return;
    }

    // CF_UNICODETEXT is CRLF on the wire and requires null-terminated UTF-16LE.
    const auto text = ClipboardText::toWire(serverData->text());
    QByteArray utf16Data(reinterpret_cast<const char *>(text.utf16()), (text.length() + 1) * 2);

    CLIPRDR_FORMAT_DATA_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    response.common.dataLen = utf16Data.size();
    response.requestedFormatData = reinterpret_cast<const BYTE *>(utf16Data.constData());

    qCDebug(KRDP) << "Serving host clipboard text to the client:" << text.length() << "characters";
    context->ServerFormatDataResponse(context, &response);
}

void Clipboard::Private::onClientFormatDataResponse(const FormatDataResponse &formatDataResponse)
{
    if (!(formatDataResponse.msgFlags & CB_RESPONSE_OK)) {
        qCDebug(KRDP) << "Client refused the clipboard data request for format" << requestedClientFormat;
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

    if (text.isEmpty()) {
        qCDebug(KRDP) << "Client clipboard text is empty";
        clientData.reset();
        Q_EMIT q->clientDataChanged();
        return;
    }

    // Stored LF on the host, whatever the wire carried.
    clientData.reset(new QMimeData());
    clientData->setText(ClipboardText::toHost(text));
    qCDebug(KRDP) << "Client clipboard text received:" << clientData->text().length() << "characters";
    Q_EMIT q->clientDataChanged();
}
}
