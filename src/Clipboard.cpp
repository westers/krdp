// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Clipboard.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/server/cliprdr.h>

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
    using CliprdrServerContextPtr = std::unique_ptr<CliprdrServerContext, decltype(&cliprdr_server_context_free)>;

    Private(Clipboard *qq)
        : q(qq)
    {
    }

    Clipboard *q;

    uint32_t onClientFormatList(const CLIPRDR_FORMAT_LIST *formatList);
    uint32_t onClientFormatListResponse(const CLIPRDR_FORMAT_LIST_RESPONSE *formatListResponse);
    uint32_t onClientFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST *formatDataRequest);
    uint32_t onClientFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE *formatDataResponse);

    RdpConnection *session;

    CliprdrServerContextPtr clipContext = CliprdrServerContextPtr(nullptr, cliprdr_server_context_free);

    bool enabled = false;
    const QMimeData *serverData = nullptr;
    std::unique_ptr<QMimeData> clientData;
    // The format id of the one data request sent for the client's last format
    // list, so the response can be decoded as what was asked for: UTF-16 for
    // CF_UNICODETEXT, 8-bit for CF_TEXT/CF_OEMTEXT.
    uint32_t requestedClientFormat = 0;

    template<typename>
    struct function_arg_trait;
    template<typename Argument>
    struct function_arg_trait<uint32_t (Private::*)(Argument)> {
        typedef Argument argument_t;
    };

    template<auto func>
    inline static UINT processInMainThread(Clipboard *clipboard, function_arg_trait<decltype(func)>::argument_t packet)
    {
        uint32_t channelState;
        QMetaObject::invokeMethod(
            clipboard,
            [clipboard](function_arg_trait<decltype(func)>::argument_t packet) {
                return ((*clipboard->d).*func)(packet);
            },
            Qt::BlockingQueuedConnection,
            qReturnArg(channelState),
            packet);
        return channelState;
    }

    static UINT clientFormatList(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST *formatList)
    {
        return processInMainThread<&Private::onClientFormatList>(reinterpret_cast<Clipboard *>(context->custom), formatList);
    }

    static UINT clientFormatListResponse(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST_RESPONSE *formatListResponse)
    {
        return processInMainThread<&Private::onClientFormatListResponse>(reinterpret_cast<Clipboard *>(context->custom), formatListResponse);
    }

    static UINT clientFormatDataRequest(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_REQUEST *formatDataRequest)
    {
        return processInMainThread<&Private::onClientFormatDataRequest>(reinterpret_cast<Clipboard *>(context->custom), formatDataRequest);
    }

    static UINT clientFormatDataResponse(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_RESPONSE *formatDataResponse)
    {
        return processInMainThread<&Private::onClientFormatDataResponse>(reinterpret_cast<Clipboard *>(context->custom), formatDataResponse);
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
}

bool Clipboard::initialize()
{
    if (d->clipContext) {
        return true;
    }

    auto peerContext = reinterpret_cast<PeerContext *>(d->session->rdpPeer()->context);

    d->clipContext = Private::CliprdrServerContextPtr{cliprdr_server_context_new(peerContext->virtualChannelManager), cliprdr_server_context_free};
    if (!d->clipContext) {
        qCWarning(KRDP) << "Failed creating Clipboard context";
        return false;
    }

    d->clipContext->useLongFormatNames = TRUE;
    d->clipContext->streamFileClipEnabled = FALSE;
    d->clipContext->fileClipNoFilePaths = FALSE;
    d->clipContext->canLockClipData = FALSE;
    d->clipContext->hasHugeFileSupport = FALSE;

    d->clipContext->custom = this;
    d->clipContext->rdpcontext = d->session->rdpPeer()->context;

    d->clipContext->ClientFormatList = Private::clientFormatList;
    d->clipContext->ClientFormatListResponse = Private::clientFormatListResponse;
    d->clipContext->ClientFormatDataRequest = Private::clientFormatDataRequest;
    d->clipContext->ClientFormatDataResponse = Private::clientFormatDataResponse;

    // returns 0 on success
    // https://pub.freerdp.com/api/server_2cliprdr__main_8c.html#ab4e8a28c6b4371c2a5f34e8716ab1e9e
    if (d->clipContext->Start(d->clipContext.get())) {
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
    if (!d->clipContext) {
        return;
    }

    if (d->clipContext->Stop(d->clipContext.get())) {
        qCWarning(KRDP) << "Could not stop Clipboard context";
        return;
    };
    d->enabled = false;
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
    if (!d->serverData || !d->enabled) {
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
    d->clipContext->ServerFormatList(d->clipContext.get(), &formatList);
}

uint32_t Clipboard::Private::onClientFormatList(const CLIPRDR_FORMAT_LIST *formatList)
{
    // One data request per format list, whatever text formats it announces:
    // a client copy typically lists CF_UNICODETEXT and CF_TEXT together, and
    // a request for each of them meant two clipboard writes and two announces
    // on the host per copy (the x2 amplifier in the announce storm). Prefer
    // CF_UNICODETEXT, then CF_TEXT, then CF_OEMTEXT.
    uint32_t wanted = 0;
    for (uint32_t i = 0; i < formatList->numFormats; ++i) {
        const auto formatId = formatList->formats[i].formatId;
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
    CLIPRDR_FORMAT_LIST_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    clipContext->ServerFormatListResponse(clipContext.get(), &response);

    if (wanted == 0) {
        qCDebug(KRDP) << "Client announced" << formatList->numFormats << "clipboard formats, none of them text; not requesting data";
        return CHANNEL_RC_OK;
    }

    qCDebug(KRDP) << "Client announced" << formatList->numFormats << "clipboard formats; requesting text as format" << wanted;
    requestedClientFormat = wanted;
    CLIPRDR_FORMAT_DATA_REQUEST formatDataRequest{.common = CLIPRDR_HEADER({.msgType = CB_FORMAT_DATA_REQUEST, .msgFlags = 0, .dataLen = 4}),
                                                  .requestedFormatId = wanted};
    clipContext->ServerFormatDataRequest(clipContext.get(), &formatDataRequest);

    return CHANNEL_RC_OK;
}

uint32_t Clipboard::Private::onClientFormatListResponse(const CLIPRDR_FORMAT_LIST_RESPONSE *)
{
    return CHANNEL_RC_OK;
}

uint32_t Clipboard::Private::onClientFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST *formatDataRequest)
{
    if (!serverData || formatDataRequest->requestedFormatId != CF_UNICODETEXT) {
        CLIPRDR_FORMAT_DATA_RESPONSE response = {};
        response.common.msgType = CB_FORMAT_DATA_RESPONSE;
        response.common.msgFlags = CB_RESPONSE_FAIL;
        response.common.dataLen = 0;
        response.requestedFormatData = nullptr;
        clipContext->ServerFormatDataResponse(clipContext.get(), &response);
        return CHANNEL_RC_OK;
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
    clipContext->ServerFormatDataResponse(clipContext.get(), &response);

    return CHANNEL_RC_OK;
}

uint32_t Clipboard::Private::onClientFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE *formatDataResponse)
{
    if (!(formatDataResponse->common.msgFlags & CB_RESPONSE_OK)) {
        qCDebug(KRDP) << "Client refused the clipboard data request for format" << requestedClientFormat;
        return CHANNEL_RC_OK;
    }

    const auto *bytes = reinterpret_cast<const char *>(formatDataResponse->requestedFormatData);
    const auto length = formatDataResponse->common.dataLen;
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
        return CHANNEL_RC_OK;
    }

    // Stored LF on the host, whatever the wire carried.
    clientData.reset(new QMimeData());
    clientData->setText(ClipboardText::toHost(text));
    qCDebug(KRDP) << "Client clipboard text received:" << clientData->text().length() << "characters";
    Q_EMIT q->clientDataChanged();

    return CHANNEL_RC_OK;
}
}
