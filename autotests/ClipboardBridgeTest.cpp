// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-067: the broker's cliprdr side of the clipboard bridge (delayed rendering, directions, caps, resets).
// A fake CliprdrServerContext stands in for the FreeRDP channel: no peer, no daemon, no display.

#include <QBuffer>
#include <QImage>
#include <QSignalSpy>
#include <QLoggingCategory>
#include <QTest>

#include "Clipboard.h"
#include "ClipboardHtml.h"
#include "ClipboardImage.h"
#include "ClipboardLimits.h"

using namespace KRdp;

namespace
{
const QString Canary = QStringLiteral("CANARY-91c2d4-never-in-a-log");

struct Sent {
    QList<QList<uint32_t>> formatLists;
    QList<QStringList> formatNames; // OPT-072: parallel to formatLists
    int formatListResponses = 0;
    QList<uint32_t> dataRequests;
    struct Response {
        uint16_t flags;
        QByteArray data;
    };
    QList<Response> dataResponses;
};
Sent *g_sent = nullptr;

UINT fakeFormatList(CliprdrServerContext *, const CLIPRDR_FORMAT_LIST *list)
{
    QList<uint32_t> ids;
    QStringList names;
    for (uint32_t i = 0; i < list->numFormats; ++i) {
        ids.append(list->formats[i].formatId);
        names.append(list->formats[i].formatName ? QString::fromUtf8(list->formats[i].formatName) : QString());
    }
    g_sent->formatLists.append(ids);
    g_sent->formatNames.append(names);
    return CHANNEL_RC_OK;
}
UINT fakeFormatListResponse(CliprdrServerContext *, const CLIPRDR_FORMAT_LIST_RESPONSE *)
{
    ++g_sent->formatListResponses;
    return CHANNEL_RC_OK;
}
UINT fakeDataRequest(CliprdrServerContext *, const CLIPRDR_FORMAT_DATA_REQUEST *request)
{
    g_sent->dataRequests.append(request->requestedFormatId);
    return CHANNEL_RC_OK;
}
UINT fakeDataResponse(CliprdrServerContext *, const CLIPRDR_FORMAT_DATA_RESPONSE *response)
{
    g_sent->dataResponses.append({response->common.msgFlags,
                                  response->requestedFormatData ? QByteArray(reinterpret_cast<const char *>(response->requestedFormatData), response->common.dataLen)
                                                                : QByteArray()});
    return CHANNEL_RC_OK;
}

QByteArray utf16z(const QString &text)
{
    return QByteArray(reinterpret_cast<const char *>(text.utf16()), (text.size() + 1) * 2);
}

// CF_TEXT: 8-bit with a terminator. A response nobody asked for is decoded this way (no format was requested).
QByteArray ansi(const QString &text)
{
    return text.toLatin1() + '\0';
}

QString fromUtf16z(const QByteArray &data)
{
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(data.constData()), data.size() / 2 - 1);
}

QByteArray encodeImage(const QImage &image, const char *format = "PNG")
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return out;
}
QImage picture()
{
    QImage image(6, 4, QImage::Format_RGB32);
    image.fill(qRgb(200, 30, 90));
    return image;
}
QByteArray le32(quint32 v)
{
    QByteArray out;
    for (int i = 0; i < 4; ++i) out.append(char((v >> (8 * i)) & 0xff));
    return out;
}
constexpr uint32_t ClientHtmlId = 0xC0F1; // whatever number a client gives its registered "HTML Format"
constexpr uint32_t ClientPngId = 0xC0DE; // whatever number a client gives its registered "PNG"

QStringList g_messages;
void capture(QtMsgType, const QMessageLogContext &, const QString &message)
{
    g_messages.append(message);
}

struct Rig {
    Sent sent;
    CliprdrServerContext context{};
    Clipboard clipboard{nullptr};
    Rig()
    {
        g_sent = &sent;
        context.ServerFormatList = fakeFormatList;
        context.ServerFormatListResponse = fakeFormatListResponse;
        context.ServerFormatDataRequest = fakeDataRequest;
        context.ServerFormatDataResponse = fakeDataResponse;
        Clipboard::installClientCallbacks(&context, &clipboard);
        clipboard.attachContextForTest(&context);
    }
    ~Rig() { g_sent = nullptr; }

    UINT clientFormatList(QList<uint32_t> ids = {CF_UNICODETEXT})
    {
        QList<CLIPRDR_FORMAT> formats;
        for (const auto id : ids) {
            CLIPRDR_FORMAT format{};
            format.formatId = id;
            formats.append(format);
        }
        CLIPRDR_FORMAT_LIST list{};
        list.common.msgType = CB_FORMAT_LIST;
        list.numFormats = uint32_t(formats.size());
        list.formats = formats.data();
        return context.ClientFormatList(&context, &list);
    }
    UINT clientFormatListNamed(const QList<QPair<uint32_t, QByteArray>> &entries)
    {
        QList<CLIPRDR_FORMAT> formats;
        for (const auto &entry : entries) {
            CLIPRDR_FORMAT format{};
            format.formatId = entry.first;
            format.formatName = entry.second.isEmpty() ? nullptr : const_cast<char *>(entry.second.constData());
            formats.append(format);
        }
        CLIPRDR_FORMAT_LIST list{};
        list.common.msgType = CB_FORMAT_LIST;
        list.numFormats = uint32_t(formats.size());
        list.formats = formats.data();
        return context.ClientFormatList(&context, &list);
    }
    UINT clientDataResponse(const QByteArray &bytes, uint16_t flags = CB_RESPONSE_OK, std::optional<uint32_t> declared = std::nullopt)
    {
        CLIPRDR_FORMAT_DATA_RESPONSE response{};
        response.common.msgType = CB_FORMAT_DATA_RESPONSE;
        response.common.msgFlags = flags;
        response.common.dataLen = declared.value_or(uint32_t(bytes.size()));
        response.requestedFormatData = reinterpret_cast<const BYTE *>(bytes.constData());
        return context.ClientFormatDataResponse(&context, &response);
    }
    UINT clientDataRequest(uint32_t format = CF_UNICODETEXT)
    {
        CLIPRDR_FORMAT_DATA_REQUEST request{};
        request.requestedFormatId = format;
        return context.ClientFormatDataRequest(&context, &request);
    }
};
}

class ClipboardBridgeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ---- host to client ----
    void hostTextIsFetchedAtPasteTimeNotHeld()
    {
        Rig rig;
        QSignalSpy requested(&rig.clipboard, &Clipboard::hostTextRequested);
        rig.clipboard.announceHostText();
        QTest::qWait(30);
        QVERIFY(rig.sent.formatLists.isEmpty()); // the client has no picture yet (AUD-FIX5)
        rig.clipboard.setGraphicsDelivered();
        QCOMPARE(rig.sent.formatLists.size(), 1);
        QCOMPARE(rig.sent.formatLists.first(), QList<uint32_t>{CF_UNICODETEXT});
        QCOMPARE(requested.size(), 0); // announcing asks the worker for nothing

        rig.clientDataRequest();
        QTRY_COMPARE(requested.size(), 1);
        const auto id = requested.first().first().toULongLong();
        QVERIFY(id);
        QVERIFY(rig.sent.dataResponses.isEmpty()); // the worker has not answered yet
        rig.clipboard.provideHostText(id, QStringLiteral("line one\nline two"), true);
        QCOMPARE(rig.sent.dataResponses.size(), 1);
        QCOMPARE(rig.sent.dataResponses.first().flags, uint16_t(CB_RESPONSE_OK));
        QCOMPARE(fromUtf16z(rig.sent.dataResponses.first().data), QStringLiteral("line one\r\nline two")); // CRLF on the wire
        QCOMPARE(rig.clipboard.counters().served, quint64(1));
        // A second answer for the same id is ignored.
        rig.clipboard.provideHostText(id, QStringLiteral("again"), true);
        QCOMPARE(rig.sent.dataResponses.size(), 1);
    }

    void hostTextWhoseCrlfFormExceedsTheWireCapIsRefused()
    {
        Rig rig;
        QSignalSpy requested(&rig.clipboard, &Clipboard::hostTextRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHostText();
        rig.clientDataRequest();
        QTRY_COMPARE(requested.size(), 1);
        // 700000 newlines are 700000 bytes of UTF-8 (under the 1 MiB cap) but 1.4M UTF-16 characters once CRLF: 2.8 MB.
        rig.clipboard.provideHostText(requested.first().first().toULongLong(), QString(700000, QLatin1Char('\n')), true);
        QCOMPARE(rig.sent.dataResponses.size(), 1);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(rig.clipboard.counters().refusedOversize, quint64(1));
        // A text that fits is still served.
        rig.clientDataRequest();
        QTRY_COMPARE(requested.size(), 2);
        rig.clipboard.provideHostText(requested.last().first().toULongLong(), QString(500000, QLatin1Char('\n')), true);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_OK));
    }

    void aRefusedOrUnsupportedRequestFails()
    {
        Rig rig;
        QSignalSpy requested(&rig.clipboard, &Clipboard::hostTextRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clientDataRequest(); // nothing announced
        QTRY_COMPARE(rig.sent.dataResponses.size(), 1);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        rig.clipboard.announceHostText();
        rig.clientDataRequest(CF_TEXT); // only Unicode text is served
        QTRY_COMPARE(rig.sent.dataResponses.size(), 2);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(requested.size(), 0);
        rig.clientDataRequest();
        QTRY_COMPARE(requested.size(), 1);
        rig.clipboard.provideHostText(requested.first().first().toULongLong(), {}, false); // the worker said no
        QCOMPARE(rig.sent.dataResponses.size(), 3);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QVERIFY(rig.sent.dataResponses.last().data.isEmpty());
    }

    void aSecondPasteWhileOneIsPendingIsRefused()
    {
        Rig rig;
        QSignalSpy requested(&rig.clipboard, &Clipboard::hostTextRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHostText();
        rig.clientDataRequest();
        rig.clientDataRequest();
        QTRY_COMPARE(rig.sent.dataResponses.size(), 1);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(requested.size(), 1);
        QCOMPARE(rig.clipboard.counters().refusedBusy, quint64(1));
    }

    void anUnansweredPasteFailsAfterTheTimeout()
    {
        Rig rig;
        QSignalSpy requested(&rig.clipboard, &Clipboard::hostTextRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHostText();
        rig.clientDataRequest();
        QTRY_COMPARE(requested.size(), 1);
        QTest::qWait(Clipboard::HostTextTimeoutMs / 2);
        QVERIFY(rig.sent.dataResponses.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataResponses.size(), 1, Clipboard::HostTextTimeoutMs);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(rig.clipboard.counters().refusedTimeout, quint64(1));
        // The late answer is dropped.
        rig.clipboard.provideHostText(requested.first().first().toULongLong(), QStringLiteral("late"), true);
        QCOMPARE(rig.sent.dataResponses.size(), 1);
    }

    void announcesAreCoalescedToOneFormatListPerInterval()
    {
        Rig rig;
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHostText();
        QCOMPARE(rig.sent.formatLists.size(), 1);
        for (int i = 0; i < 10; ++i) rig.clipboard.announceHostText(); // ten more host copies at once
        QCOMPARE(rig.sent.formatLists.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.formatLists.size(), 2, Clipboard::HostAnnounceIntervalMs * 3);
        QTest::qWait(Clipboard::HostAnnounceIntervalMs * 2);
        QCOMPARE(rig.sent.formatLists.size(), 2); // the burst became exactly one more
        QCOMPARE(rig.clipboard.counters().announces, quint64(2));
    }

    void denyingHostToClientWithdrawsAndFailsWhatIsPending()
    {
        Rig rig;
        QSignalSpy requested(&rig.clipboard, &Clipboard::hostTextRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHostText();
        rig.clientDataRequest();
        QTRY_COMPARE(requested.size(), 1);
        rig.clipboard.setDirections(false, true); // e.g. the lock screen of another account appeared
        QCOMPARE(rig.sent.dataResponses.size(), 1); // the pending paste fails now
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(rig.sent.formatLists.size(), 2);
        QVERIFY(rig.sent.formatLists.last().isEmpty()); // the announcement is withdrawn
        // Nothing more is announced, and a paste of the old announcement gets nothing.
        rig.clipboard.announceHostText();
        QCOMPARE(rig.sent.formatLists.size(), 2);
        rig.clientDataRequest();
        QTRY_COMPARE(rig.sent.dataResponses.size(), 2);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(requested.size(), 1);
        // The answer that was still on its way is dropped.
        rig.clipboard.provideHostText(requested.first().first().toULongLong(), Canary, true);
        QCOMPARE(rig.sent.dataResponses.size(), 2);
    }

    void resetHostSideWithdrawsOnlyWhatWasAnnounced()
    {
        Rig rig;
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.resetHostSide();
        QVERIFY(rig.sent.formatLists.isEmpty()); // nothing to withdraw
        rig.clipboard.announceHostText();
        rig.clipboard.resetHostSide(); // the greeter handed over to the session
        QCOMPARE(rig.sent.formatLists.size(), 2);
        QVERIFY(rig.sent.formatLists.last().isEmpty());
        rig.clipboard.resetHostSide();
        QCOMPARE(rig.sent.formatLists.size(), 2);
    }

    void announceWaitsForGraphicsAfterAReset()
    {
        Rig rig;
        rig.clipboard.announceHostText();
        rig.clipboard.resetHostSide(); // dropped before it was ever sent
        rig.clipboard.setGraphicsDelivered();
        QTest::qWait(30);
        QVERIFY(rig.sent.formatLists.isEmpty());
    }

    // ---- client to host ----
    void aClientCopyIsRequestedAndPassedOn()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        rig.clientFormatList({CF_UNICODETEXT, CF_TEXT});
        QTRY_COMPARE(rig.sent.formatListResponses, 1); // acknowledged first
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), uint32_t(CF_UNICODETEXT));
        rig.clientDataResponse(utf16z(QStringLiteral("typed\r\non the client")));
        QTRY_COMPARE(changed.size(), 1);
        const auto data = rig.clipboard.getClipboard();
        QVERIFY(data);
        QCOMPARE(data->text(), QStringLiteral("typed\non the client")); // LF on the host
        QCOMPARE(rig.clipboard.counters().clientCopies, quint64(1));
    }

    void deniedClientToHostIsAcknowledgedButNeverRequested()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.setDirections(true, false);
        rig.clientFormatList();
        QTRY_COMPARE(rig.sent.formatListResponses, 1);
        QTest::qWait(Clipboard::FormatListSettleMs * 2);
        QVERIFY(rig.sent.dataRequests.isEmpty());
        rig.clientDataResponse(ansi(Canary)); // even an unsolicited answer is not passed on
        QTest::qWait(30);
        QCOMPARE(changed.size(), 0);
    }

    void denyingClientToHostDropsAHalfReceivedCopy()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        rig.clientFormatList();
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        rig.clipboard.resetClientSide(); // control moved
        rig.clientDataResponse(utf16z(QStringLiteral("late")));
        QTest::qWait(30);
        QCOMPARE(changed.size(), 0);
        // The next copy works.
        rig.clientFormatList();
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 2, 2000);
        rig.clientDataResponse(utf16z(QStringLiteral("next")));
        QTRY_COMPARE(changed.size(), 1);
    }

    void oversizeClientTextIsRefusedBeforeItIsCopied()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        // The client declares more than the wire cap: the bytes are not even copied (the buffer is a few bytes).
        rig.clientDataResponse(QByteArray(4, 'x'), CB_RESPONSE_OK, uint32_t(ClipboardLimits::MaxWireBytes + 1));
        QTest::qWait(30);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(rig.clipboard.counters().refusedOversize, quint64(1));
        // Within the wire cap but more than 1 MiB of UTF-8 once decoded.
        rig.clientDataResponse(ansi(QString(600 * 1024 + 1, QChar(0x00e9)))); // 2 bytes per character in UTF-8, 1.2 MB
        QTest::qWait(30);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(rig.clipboard.counters().refusedOversize, quint64(2));
        // Exactly 1 MiB of ASCII is fine.
        rig.clientDataResponse(ansi(QString(ClipboardLimits::MaxTextBytes, QLatin1Char('a'))));
        QTRY_COMPARE(changed.size(), 1);
        QCOMPARE(rig.clipboard.getClipboard()->text().size(), int(ClipboardLimits::MaxTextBytes));
    }

    void clientCopiesAreRateLimited()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        for (int i = 0; i < 30; ++i) {
            rig.clientDataResponse(ansi(QStringLiteral("copy %1").arg(i)));
        }
        QTRY_COMPARE(changed.size(), ClipboardLimits::MaxSets);
        QTest::qWait(50);
        QCOMPARE(changed.size(), ClipboardLimits::MaxSets);
        QCOMPARE(rig.clipboard.counters().refusedRate, quint64(10));
    }

    // ---- OPT-072: images ----
    void anImageIsAnnouncedAsPngAndDibAndFetchedAtPasteTime()
    {
        Rig rig;
        QSignalSpy textRequested(&rig.clipboard, &Clipboard::hostTextRequested);
        QSignalSpy imageRequested(&rig.clipboard, &Clipboard::hostImageRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHost(false, true);
        QCOMPARE(rig.sent.formatLists.size(), 1);
        QCOMPARE(rig.sent.formatLists.first(), (QList<uint32_t>{ClipboardImage::PngFormatId, ClipboardImage::CfDib}));
        QCOMPARE(rig.sent.formatNames.first(), (QStringList{QStringLiteral("PNG"), QString()}));
        QCOMPARE(imageRequested.size(), 0); // announcing asks the worker for nothing

        const QByteArray png = encodeImage(picture());
        rig.clientDataRequest(ClipboardImage::PngFormatId);
        QTRY_COMPARE(imageRequested.size(), 1);
        QCOMPARE(textRequested.size(), 0);
        rig.clipboard.provideHostImage(imageRequested.first().first().toULongLong(), png, true);
        QCOMPARE(rig.sent.dataResponses.size(), 1);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_OK));
        QCOMPARE(rig.sent.dataResponses.last().data, png); // PNG goes through untouched
        QCOMPARE(rig.clipboard.counters().hostImages, quint64(1));

        // A client that asks for the DIB gets a 24-bit DIB of the same picture.
        rig.clientDataRequest(ClipboardImage::CfDib);
        QTRY_COMPARE(imageRequested.size(), 2);
        rig.clipboard.provideHostImage(imageRequested.last().first().toULongLong(), png, true);
        QCOMPARE(rig.sent.dataResponses.size(), 2);
        const auto dib = rig.sent.dataResponses.last().data;
        QCOMPARE(dib.size(), 40 + 20 * 4); // 6*3 = 18 -> 20 bytes a row, 4 rows
        const auto back = ClipboardImage::dibToPng(dib);
        QVERIFY(back.ok());
        QImage again;
        QVERIFY(again.loadFromData(back.data, "PNG"));
        QCOMPARE(again.pixel(2, 2) & 0xffffff, 0xc81e5au);
    }

    void aCopyWithTextAndImageAnnouncesBoth()
    {
        Rig rig;
        QSignalSpy textRequested(&rig.clipboard, &Clipboard::hostTextRequested);
        QSignalSpy imageRequested(&rig.clipboard, &Clipboard::hostImageRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHost(true, true);
        QCOMPARE(rig.sent.formatLists.first(), (QList<uint32_t>{CF_UNICODETEXT, ClipboardImage::PngFormatId, ClipboardImage::CfDib}));
        rig.clientDataRequest(CF_UNICODETEXT);
        QTRY_COMPARE(textRequested.size(), 1);
        QCOMPARE(imageRequested.size(), 0);
        rig.clipboard.provideHostText(textRequested.first().first().toULongLong(), QStringLiteral("hello"), true);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_OK));
    }

    void anImageIsNotServedUnlessTheAnnouncementCoveredIt()
    {
        Rig rig;
        QSignalSpy imageRequested(&rig.clipboard, &Clipboard::hostImageRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHostText(); // text only
        rig.clientDataRequest(ClipboardImage::PngFormatId);
        QTRY_COMPARE(rig.sent.dataResponses.size(), 1);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        rig.clientDataRequest(ClipboardImage::CfDib);
        QTRY_COMPARE(rig.sent.dataResponses.size(), 2);
        QCOMPARE(imageRequested.size(), 0);
        rig.clipboard.announceHost(false, true); // image only: text is no longer served
        QTest::qWait(Clipboard::HostAnnounceIntervalMs * 2);
        rig.clientDataRequest(CF_UNICODETEXT);
        QTRY_COMPARE(rig.sent.dataResponses.size(), 3);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        rig.clientDataRequest(ClipboardImage::CfDibV5); // never announced
        QTRY_COMPARE(rig.sent.dataResponses.size(), 4);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
    }

    void aHostImageIsRefusedWhenItIsNotAcceptable()
    {
        Rig rig;
        QSignalSpy imageRequested(&rig.clipboard, &Clipboard::hostImageRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHost(false, true);
        int asked = 0;
        uint16_t lastFlags = 0;
        auto ask = [&](const QByteArray &png, bool ok, uint32_t format = ClipboardImage::PngFormatId) {
            rig.clientDataRequest(format);
            ++asked;
            QTRY_COMPARE(imageRequested.size(), asked);
            rig.clipboard.provideHostImage(imageRequested.last().first().toULongLong(), png, ok);
            lastFlags = rig.sent.dataResponses.last().flags;
        };
        ask({}, false);
        QCOMPARE(lastFlags, uint16_t(CB_RESPONSE_FAIL)); // the worker said no
        ask(QByteArray("not a png"), true);
        QCOMPARE(lastFlags, uint16_t(CB_RESPONSE_FAIL)); // what comes from the worker is checked again
        // A bomb: a 70 byte PNG declaring 60000 x 60000.
        QByteArray bomb = encodeImage(picture());
        for (int i = 0; i < 4; ++i) {
            bomb[16 + i] = char((60000 >> (8 * (3 - i))) & 0xff);
            bomb[20 + i] = char((60000 >> (8 * (3 - i))) & 0xff);
        }
        ask(bomb, true);
        QCOMPARE(lastFlags, uint16_t(CB_RESPONSE_FAIL));
        ask(bomb, true, ClipboardImage::CfDib);
        QCOMPARE(lastFlags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(rig.clipboard.counters().hostImages, quint64(0));
        // A picture whose DIB would pass 64 MiB (8000 x 8000 x 3) is refused on the DIB path, served as PNG.
        QByteArray big = encodeImage(picture());
        for (int i = 0; i < 4; ++i) {
            big[16 + i] = char((8000 >> (8 * (3 - i))) & 0xff);
            big[20 + i] = char((8000 >> (8 * (3 - i))) & 0xff);
        }
        ask(big, true, ClipboardImage::CfDib);
        QCOMPARE(lastFlags, uint16_t(CB_RESPONSE_FAIL));
        ask(big, true);
        QCOMPARE(lastFlags, uint16_t(CB_RESPONSE_OK));
    }

    void denyingHostToClientFailsAPendingImagePaste()
    {
        Rig rig;
        QSignalSpy imageRequested(&rig.clipboard, &Clipboard::hostImageRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHost(false, true);
        rig.clientDataRequest(ClipboardImage::PngFormatId);
        QTRY_COMPARE(imageRequested.size(), 1);
        rig.clipboard.setDirections(false, true);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QVERIFY(rig.sent.formatLists.last().isEmpty()); // withdrawn
        rig.clipboard.provideHostImage(imageRequested.first().first().toULongLong(), encodeImage(picture()), true); // late
        QCOMPARE(rig.sent.dataResponses.size(), 1);
    }

    void aClientPngIsRequestedByItsRegisteredNameAndPassedOn()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        // CF_DIB and CF_DIBV5 alongside: the registered PNG wins whatever id the client gave it.
        rig.clientFormatListNamed({{ClipboardImage::CfDib, {}}, {ClientPngId, "PNG"}, {ClipboardImage::CfDibV5, {}}});
        QTRY_COMPARE(rig.sent.formatListResponses, 1);
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), ClientPngId);
        const auto png = encodeImage(picture());
        rig.clientDataResponse(png);
        QTRY_COMPARE(changed.size(), 1);
        const auto data = rig.clipboard.getClipboard();
        QVERIFY(data);
        QCOMPARE(data->data(QStringLiteral("image/png")), png);
        QVERIFY(!data->hasText());
        QCOMPARE(rig.clipboard.counters().clientImages, quint64(1));
    }

    void aClientDibIsConvertedAndPreferenceIsPngThenV5ThenDib()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        // The 24-bit DIB of a 2x1 picture: blue pixel, then green pixel, padded to 8 bytes.
        QByteArray dib = le32(40) + le32(2) + le32(1) + QByteArray("\x01\x00\x18\x00", 4) + le32(0) + le32(8) + le32(2835) + le32(2835) + le32(0) + le32(0)
            + QByteArray("\xff\x00\x00\x00\xff\x00\x00\x00", 8);
        rig.clientFormatList({ClipboardImage::CfDib, ClipboardImage::CfDibV5});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), uint32_t(ClipboardImage::CfDibV5));
        rig.clientDataResponse(dib);
        QTRY_COMPARE(changed.size(), 1);
        const auto data = rig.clipboard.getClipboard();
        QVERIFY(data);
        QImage image;
        QVERIFY(image.loadFromData(data->data(QStringLiteral("image/png")), "PNG"));
        QCOMPARE(image.size(), QSize(2, 1));
        QCOMPARE(image.pixel(0, 0) & 0xffffff, 0x0000ffu);
        QCOMPARE(image.pixel(1, 0) & 0xffffff, 0x00ff00u);
        rig.clientFormatList({ClipboardImage::CfDib});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 2, 2000);
        QCOMPARE(rig.sent.dataRequests.last(), uint32_t(ClipboardImage::CfDib));
    }

    void textWinsOverAnImageInTheSameFormatList()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged); // something takes the client's clipboard
        rig.clipboard.setGraphicsDelivered();
        rig.clientFormatListNamed({{ClientPngId, "PNG"}, {CF_UNICODETEXT, {}}, {ClipboardImage::CfDib, {}}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), uint32_t(CF_UNICODETEXT));
    }

    void formatNamesDecideWhatIsPng()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged); // something takes the client's clipboard
        rig.clipboard.setGraphicsDelivered();
        // A registered format with another name is not an image; nothing is requested.
        rig.clientFormatListNamed({{0xC010, "Rich Text Format"}, {0xC011, "image/x-unknown"}});
        QTest::qWait(Clipboard::FormatListSettleMs * 2);
        QVERIFY(rig.sent.dataRequests.isEmpty());
        // Some clients call it by its MIME type.
        rig.clientFormatListNamed({{0xC012, "image/png"}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), uint32_t(0xC012));
    }

    void aDeniedClientToHostNeverRequestsAnImage()
    {
        Rig rig;
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.setDirections(true, false);
        rig.clientFormatListNamed({{ClientPngId, "PNG"}});
        QTRY_COMPARE(rig.sent.formatListResponses, 1);
        QTest::qWait(Clipboard::FormatListSettleMs * 2);
        QVERIFY(rig.sent.dataRequests.isEmpty());
    }

    void clientImagesAreRefusedWhenOversizeBombMalformedOrTooFast()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        const auto request = [&](int expectedRequests) {
            rig.clientFormatListNamed({{ClientPngId, "PNG"}});
            QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), expectedRequests, 2000);
        };
        // Declared over the image wire cap: not even copied.
        request(1);
        rig.clientDataResponse(QByteArray(4, 'x'), CB_RESPONSE_OK, uint32_t(ClipboardLimits::MaxImageWireBytes + 1));
        QTRY_COMPARE(rig.clipboard.counters().refusedOversize, quint64(1));
        // Within the wire cap but over the 16 MiB encoded cap.
        request(2);
        QByteArray huge = encodeImage(picture()) + QByteArray(ClipboardLimits::MaxImageBytes, 'x');
        rig.clientDataResponse(huge);
        QTRY_COMPARE(rig.clipboard.counters().refusedOversize, quint64(2));
        // A decompression bomb: tiny, declares 60000 x 60000.
        request(3);
        QByteArray bomb = encodeImage(picture());
        for (int i = 0; i < 4; ++i) {
            bomb[16 + i] = char((60000 >> (8 * (3 - i))) & 0xff);
            bomb[20 + i] = char((60000 >> (8 * (3 - i))) & 0xff);
        }
        rig.clientDataResponse(bomb);
        QTRY_COMPARE(rig.clipboard.counters().refusedOversize, quint64(3));
        // Garbage that is not a PNG, and a truncated one.
        request(4);
        rig.clientDataResponse(QByteArray(200, 'g'));
        QTRY_COMPARE(rig.clipboard.counters().refusedMalformed, quint64(1));
        request(5);
        rig.clientDataResponse(encodeImage(picture()).left(40));
        QTRY_COMPARE(rig.clipboard.counters().refusedMalformed, quint64(2));
        QCOMPARE(changed.size(), 0);
        // And a fine one still goes through afterwards; the text cap is back for the next text.
        request(6);
        rig.clientDataResponse(encodeImage(picture()));
        QTRY_COMPARE(changed.size(), 1);
        QCOMPARE(rig.clipboard.counters().clientImages, quint64(1));
    }

    void aLargeImageResponseIsAcceptedUpToTheImageCapOnlyWhenAnImageWasAsked()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        // Asked for text: a 3 MiB response is over the text wire cap and is refused unread.
        rig.clientFormatList();
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        rig.clientDataResponse(QByteArray(4, 'x'), CB_RESPONSE_OK, uint32_t(3 * 1024 * 1024));
        QTRY_COMPARE(rig.clipboard.counters().refusedOversize, quint64(1));
        QCOMPARE(changed.size(), 0);
    }

    void imageCopiesShareTheRateLimitWithText()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        const auto png = encodeImage(picture());
        for (int i = 0; i < ClipboardLimits::MaxSets - 1; ++i) {
            rig.clientDataResponse(ansi(QStringLiteral("copy %1").arg(i)));
        }
        QTRY_COMPARE(changed.size(), ClipboardLimits::MaxSets - 1);
        rig.clientFormatListNamed({{ClientPngId, "PNG"}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        rig.clientDataResponse(png); // the 20th set
        QTRY_COMPARE(changed.size(), ClipboardLimits::MaxSets);
        rig.clientFormatListNamed({{ClientPngId, "PNG"}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 2, 2000);
        rig.clientDataResponse(png); // the 21st in the window
        QTRY_COMPARE(rig.clipboard.counters().refusedRate, quint64(1));
        QCOMPARE(changed.size(), ClipboardLimits::MaxSets);
    }

    void imageContentsNeverReachALog()
    {
        g_messages.clear();
        const auto previous = qInstallMessageHandler(capture);
        QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true"));
        {
            Rig rig;
            QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
            QSignalSpy imageRequested(&rig.clipboard, &Clipboard::hostImageRequested);
            rig.clipboard.setGraphicsDelivered();
            rig.clipboard.announceHost(false, true);
            rig.clientDataRequest(ClipboardImage::PngFormatId);
            QTRY_COMPARE(imageRequested.size(), 1);
            rig.clipboard.provideHostImage(imageRequested.first().first().toULongLong(), encodeImage(picture()) + Canary.toUtf8(), true); // malformed: data after IEND
            QTest::qWait(Clipboard::EchoWindowMs + 100); // the client's list this soon would be taken for an echo
            rig.clientFormatListNamed({{ClientPngId, "PNG"}});
            QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
            rig.clientDataResponse(encodeImage(picture()) + Canary.toUtf8());
            QTRY_COMPARE(rig.clipboard.counters().refusedMalformed, quint64(1));
        }
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(previous);
        bool logged = false;
        for (const auto &message : std::as_const(g_messages)) {
            QVERIFY2(!message.contains(QStringLiteral("CANARY")), qPrintable(message));
            logged = logged || message.contains(QStringLiteral("refused (malformed)"));
        }
        QVERIFY(logged);
    }

    // ---- OPT-077: formatted text ----
    void htmlIsAnnouncedBesideTextAndFetchedAtPasteTime()
    {
        Rig rig;
        QSignalSpy textRequested(&rig.clipboard, &Clipboard::hostTextRequested);
        QSignalSpy htmlRequested(&rig.clipboard, &Clipboard::hostHtmlRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHost(true, false, true);
        QCOMPARE(rig.sent.formatLists.size(), 1);
        QCOMPARE(rig.sent.formatLists.first(), (QList<uint32_t>{ClipboardHtml::HtmlFormatId, CF_UNICODETEXT}));
        QCOMPARE(rig.sent.formatNames.first(), (QStringList{QStringLiteral("HTML Format"), QString()}));
        QCOMPARE(htmlRequested.size(), 0); // announcing asks the worker for nothing

        rig.clientDataRequest(ClipboardHtml::HtmlFormatId);
        QTRY_COMPARE(htmlRequested.size(), 1);
        QCOMPARE(textRequested.size(), 0);
        const QByteArray html("<p>caf\xC3\xA9 <b>bold</b></p>");
        rig.clipboard.provideHostHtml(htmlRequested.first().first().toULongLong(), html, true);
        QCOMPARE(rig.sent.dataResponses.size(), 1);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_OK));
        const QByteArray wire = rig.sent.dataResponses.last().data;
        QVERIFY(wire.startsWith("Version:0.9\r\nStartHTML:0000000105\r\n"));
        QVERIFY(wire.endsWith('\0'));
        QCOMPARE(rig.clipboard.counters().hostHtml, quint64(1));
        // What a client reads back is the document, valid UTF-8, with the text intact.
        const auto back = ClipboardHtml::unwrapMsHtml(wire);
        QVERIFY(back.ok());
        QVERIFY(back.data.contains("<b>bold</b>"));
        QVERIFY(back.data.contains("caf\xC3\xA9"));
        // The text of the same copy is its own request, answered separately.
        rig.clientDataRequest(CF_UNICODETEXT);
        QTRY_COMPARE(textRequested.size(), 1);
        rig.clipboard.provideHostText(textRequested.first().first().toULongLong(), QStringLiteral("café bold"), true);
        QCOMPARE(rig.sent.dataResponses.size(), 2);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_OK));
    }

    void theAnnouncementFollowsTheCombinationRule()
    {
        const auto announced = [](bool text, bool image, bool html) {
            Rig rig;
            rig.clipboard.setGraphicsDelivered();
            rig.clipboard.announceHost(text, image, html);
            return rig.sent.formatLists.isEmpty() ? QList<uint32_t>() : rig.sent.formatLists.first();
        };
        QCOMPARE(announced(false, false, true), (QList<uint32_t>{ClipboardHtml::HtmlFormatId}));
        QCOMPARE(announced(true, false, true), (QList<uint32_t>{ClipboardHtml::HtmlFormatId, CF_UNICODETEXT}));
        // A browser's "Copy image": <img> HTML and a picture, no text. The picture wins.
        QCOMPARE(announced(false, true, true), (QList<uint32_t>{ClipboardImage::PngFormatId, ClipboardImage::CfDib}));
        // A spreadsheet range: text, HTML and a picture. Text and HTML; the picture is dropped.
        QCOMPARE(announced(true, true, true), (QList<uint32_t>{ClipboardHtml::HtmlFormatId, CF_UNICODETEXT}));
        // Without HTML, the earlier rule: text and picture both announced.
        QCOMPARE(announced(true, true, false), (QList<uint32_t>{CF_UNICODETEXT, ClipboardImage::PngFormatId, ClipboardImage::CfDib}));
    }

    void anHtmlRequestFailsPromptlyWhenItCannotBeServed()
    {
        Rig rig;
        QSignalSpy htmlRequested(&rig.clipboard, &Clipboard::hostHtmlRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clientDataRequest(ClipboardHtml::HtmlFormatId); // nothing announced
        QTRY_COMPARE(rig.sent.dataResponses.size(), 1);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        rig.clipboard.announceHost(true, false, false);
        rig.clientDataRequest(ClipboardHtml::HtmlFormatId); // announced text only
        QTRY_COMPARE(rig.sent.dataResponses.size(), 2);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(htmlRequested.size(), 0);
        rig.clipboard.announceHost(true, false, true);
        QTest::qWait(Clipboard::HostAnnounceIntervalMs + 50);
        // The worker says no / empty / not valid: each is a failure response now.
        for (const QByteArray &answer : {QByteArray(), QByteArray("   \n "), QByteArray("<p>ok</p>")}) {
            const int before = rig.sent.dataResponses.size();
            rig.clientDataRequest(ClipboardHtml::HtmlFormatId);
            QTRY_COMPARE(htmlRequested.size(), before - 1);
            const bool good = answer == "<p>ok</p>";
            rig.clipboard.provideHostHtml(htmlRequested.last().first().toULongLong(), answer, !answer.isEmpty() && good);
            QCOMPARE(rig.sent.dataResponses.size(), before + 1);
            QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(good ? CB_RESPONSE_OK : CB_RESPONSE_FAIL));
        }
        // An answer the wrapper refuses (whitespace only, claimed ok) fails too.
        rig.clientDataRequest(ClipboardHtml::HtmlFormatId);
        QTRY_COMPARE(htmlRequested.size(), 4);
        rig.clipboard.provideHostHtml(htmlRequested.last().first().toULongLong(), QByteArray("   \n "), true);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(rig.clipboard.counters().refusedMalformed, quint64(1));
        // Over the cap: refused, with the oversize counter.
        rig.clientDataRequest(ClipboardHtml::HtmlFormatId);
        QTRY_COMPARE(htmlRequested.size(), 5);
        rig.clipboard.provideHostHtml(htmlRequested.last().first().toULongLong(), QByteArray(ClipboardLimits::MaxHtmlBytes + 1, 'a'), true);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(rig.clipboard.counters().refusedOversize, quint64(1));
    }

    void anUnansweredHtmlPasteFailsAfterTheTimeoutAndDenialFailsAPendingOne()
    {
        Rig rig;
        QSignalSpy htmlRequested(&rig.clipboard, &Clipboard::hostHtmlRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHost(true, false, true);
        rig.clientDataRequest(ClipboardHtml::HtmlFormatId);
        QTRY_COMPARE(htmlRequested.size(), 1);
        QVERIFY(rig.sent.dataResponses.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataResponses.size(), 1, Clipboard::HostTextTimeoutMs + 2000);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(rig.clipboard.counters().refusedTimeout, quint64(1));
        // A late answer is ignored (no second response).
        rig.clipboard.provideHostHtml(htmlRequested.first().first().toULongLong(), QByteArray("<p>late</p>"), true);
        QCOMPARE(rig.sent.dataResponses.size(), 1);
        // Denying host to client while a paste is pending answers it with a failure at once.
        rig.clientDataRequest(ClipboardHtml::HtmlFormatId);
        QTRY_COMPARE(htmlRequested.size(), 2);
        rig.clipboard.setDirections(false, true);
        QCOMPARE(rig.sent.dataResponses.size(), 2);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
    }

    void aSecondHtmlPasteWhileOneIsPendingIsRefused()
    {
        Rig rig;
        QSignalSpy htmlRequested(&rig.clipboard, &Clipboard::hostHtmlRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHost(true, false, true);
        rig.clientDataRequest(ClipboardHtml::HtmlFormatId);
        rig.clientDataRequest(ClipboardHtml::HtmlFormatId);
        QTRY_COMPARE(rig.sent.dataResponses.size(), 1);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(htmlRequested.size(), 1);
        QCOMPARE(rig.clipboard.counters().refusedBusy, quint64(1));
    }

    void aRichClientCopyIsRequestedTextThenHtmlAndWrittenTogether()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        const QByteArray html("<p>caf\xC3\xA9 <i>italic</i></p>");
        rig.clientFormatListNamed({{ClientHtmlId, "HTML Format"}, {CF_UNICODETEXT, {}}, {CF_TEXT, {}}});
        QTRY_COMPARE(rig.sent.formatListResponses, 1);
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), uint32_t(CF_UNICODETEXT)); // text first
        rig.clientDataResponse(utf16z(QStringLiteral("café italic")));
        QTRY_COMPARE(rig.sent.dataRequests.size(), 2);
        QCOMPARE(rig.sent.dataRequests.last(), ClientHtmlId); // then the HTML, with no wait for a settle timer
        QCOMPARE(changed.size(), 0); // nothing is written until both are in
        rig.clientDataResponse(ClipboardHtml::wrapForWire(html).data);
        QTRY_COMPARE(changed.size(), 1);
        const auto data = rig.clipboard.getClipboard();
        QVERIFY(data);
        QCOMPARE(data->text(), QStringLiteral("café italic"));
        const QByteArray stored = data->data(QStringLiteral("text/html"));
        QVERIFY(QByteArrayView(stored).isValidUtf8());
        QVERIFY(stored.contains("<i>italic</i>"));
        QVERIFY(stored.contains("charset"));
        QCOMPARE(rig.clipboard.counters().clientHtml, quint64(1));
    }

    void anHtmlOnlyClientCopyIsRequestedAndPassedOn()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        rig.clientFormatListNamed({{ClientHtmlId, "html format"}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), ClientHtmlId);
        rig.clientDataResponse(ClipboardHtml::wrapForWire("<b>only</b>").data);
        QTRY_COMPARE(changed.size(), 1);
        const auto data = rig.clipboard.getClipboard();
        QVERIFY(data && !data->hasText());
        QVERIFY(data->data(QStringLiteral("text/html")).contains("<b>only</b>"));
    }

    void aClientCopyImageWithHtmlButNoTextIsThePicture()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        rig.clientFormatListNamed({{ClientHtmlId, "HTML Format"}, {ClientPngId, "PNG"}, {ClipboardImage::CfDib, {}}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), ClientPngId);
        rig.clientDataResponse(encodeImage(picture()));
        QTRY_COMPARE(changed.size(), 1);
        const auto data = rig.clipboard.getClipboard();
        QVERIFY(data && data->hasFormat(QStringLiteral("image/png")));
        QVERIFY(!data->hasFormat(QStringLiteral("text/html")));
        QCOMPARE(rig.sent.dataRequests.size(), 1); // the HTML was never asked for
    }

    void aSpreadsheetStyleClientCopyIsRichNotAPicture()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        rig.clientFormatListNamed({{CF_UNICODETEXT, {}}, {ClientHtmlId, "HTML Format"}, {ClientPngId, "PNG"}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), uint32_t(CF_UNICODETEXT));
        rig.clientDataResponse(utf16z(QStringLiteral("a\tb")));
        QTRY_COMPARE(rig.sent.dataRequests.size(), 2);
        QCOMPARE(rig.sent.dataRequests.last(), ClientHtmlId);
        rig.clientDataResponse(ClipboardHtml::wrapForWire("<table><tr><td>a</td><td>b</td></tr></table>").data);
        QTRY_COMPARE(changed.size(), 1);
        QVERIFY(!rig.clipboard.getClipboard()->hasFormat(QStringLiteral("image/png")));
    }

    void whenTheHtmlIsRefusedTheTextAloneLands()
    {
        // Each way the second request can fail: the client says no, the document is malformed, it declares too much.
        for (int how = 0; how < 3; ++how) {
            Rig rig;
            QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
            rig.clipboard.setGraphicsDelivered();
            rig.clientFormatListNamed({{CF_UNICODETEXT, {}}, {ClientHtmlId, "HTML Format"}});
            QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
            rig.clientDataResponse(utf16z(QStringLiteral("still the text")));
            QTRY_COMPARE(rig.sent.dataRequests.size(), 2);
            if (how == 0) {
                rig.clientDataResponse({}, CB_RESPONSE_FAIL);
            } else if (how == 1) {
                rig.clientDataResponse(QByteArray("Version:0.9\r\nStartHTML:oops\r\n<p>x</p>"));
            } else {
                rig.clientDataResponse({}, CB_RESPONSE_OK, uint32_t(ClipboardLimits::MaxHtmlWireBytes + 1));
            }
            QTRY_COMPARE(changed.size(), 1);
            const auto data = rig.clipboard.getClipboard();
            QCOMPARE(data->text(), QStringLiteral("still the text"));
            QVERIFY(!data->hasFormat(QStringLiteral("text/html")));
            QCOMPARE(rig.clipboard.counters().clientHtml, quint64(0));
            QCOMPARE(rig.clipboard.counters().clientCopies, quint64(1));
            // A refused HTML does not stop later copies being requested.
            rig.clientFormatListNamed({{CF_UNICODETEXT, {}}});
            QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 3, 2000);
        }
    }

    void aRefusedTextRequestDropsTheHtmlRequest()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        rig.clientFormatListNamed({{CF_UNICODETEXT, {}}, {ClientHtmlId, "HTML Format"}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        rig.clientDataResponse({}, CB_RESPONSE_FAIL);
        QTest::qWait(150);
        QCOMPARE(rig.sent.dataRequests.size(), 1); // no HTML request after a refused text request
        QCOMPARE(changed.size(), 0);
    }

    void aNewFormatListForgetsAHalfFetchedRichCopy()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        rig.clientFormatListNamed({{CF_UNICODETEXT, {}}, {ClientHtmlId, "HTML Format"}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        rig.clientDataResponse(utf16z(QStringLiteral("old text")));
        QTRY_COMPARE(rig.sent.dataRequests.size(), 2); // the HTML request is on the wire
        // The client copies again before answering: its new list is for a text-only copy.
        rig.clientFormatListNamed({{CF_UNICODETEXT, {}}});
        QTest::qWait(30);
        rig.clientDataResponse(ClipboardHtml::wrapForWire("<p>old html</p>").data); // the answer to the old request: dropped
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 3, 2000);
        QCOMPARE(rig.sent.dataRequests.last(), uint32_t(CF_UNICODETEXT));
        QCOMPARE(changed.size(), 0);
        rig.clientDataResponse(utf16z(QStringLiteral("new text")));
        QTRY_COMPARE(changed.size(), 1);
        const auto data = rig.clipboard.getClipboard();
        QVERIFY(data);
        QCOMPARE(data->text(), QStringLiteral("new text"));
        QVERIFY(!data->hasFormat(QStringLiteral("text/html"))); // never mixed with the old copy's HTML
    }

    void denyingClientToHostDropsAHalfReceivedRichCopy()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        rig.clientFormatListNamed({{CF_UNICODETEXT, {}}, {ClientHtmlId, "HTML Format"}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        rig.clientDataResponse(utf16z(QStringLiteral("text")));
        QTRY_COMPARE(rig.sent.dataRequests.size(), 2);
        rig.clipboard.setDirections(true, false);
        rig.clientDataResponse(ClipboardHtml::wrapForWire("<p>x</p>").data);
        QTest::qWait(100);
        QCOMPARE(changed.size(), 0);
        QVERIFY(!rig.clipboard.getClipboard());
    }

    void htmlResponsesHaveTheirOwnCapAndTextKeepsItsOwn()
    {
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        // A text request: 3 MiB declared is over the text wire cap.
        rig.clientFormatList({CF_UNICODETEXT});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        rig.clientDataResponse({}, CB_RESPONSE_OK, uint32_t(3 * 1024 * 1024));
        QTRY_COMPARE(rig.clipboard.counters().refusedOversize, quint64(1));
        // An HTML request may carry the document cap.
        rig.clientFormatListNamed({{ClientHtmlId, "HTML Format"}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 2, 2000);
        const QByteArray big = ClipboardHtml::wrapForWire("<p>" + QByteArray(5 * 1024 * 1024, 'a') + "</p>").data;
        rig.clientDataResponse(big);
        QTRY_COMPARE(changed.size(), 1);
        QCOMPARE(rig.clipboard.counters().clientHtml, quint64(1));
    }

    void htmlCopiesShareTheRateLimit()
    {
        const QByteArray wire = ClipboardHtml::wrapForWire("<p>x</p>").data;
        Rig limited;
        QSignalSpy limitedChanged(&limited.clipboard, &Clipboard::clientDataChanged);
        limited.clipboard.setGraphicsDelivered();
        for (int i = 0; i < 25; ++i) {
            limited.clientFormatListNamed({{ClientHtmlId, "HTML Format"}});
            QTRY_COMPARE_WITH_TIMEOUT(limited.sent.dataRequests.size(), i + 1, 2000);
            limited.clientDataResponse(wire);
            QTest::qWait(10);
        }
        QTRY_COMPARE(limitedChanged.size(), ClipboardLimits::MaxSets);
        QVERIFY(limited.clipboard.counters().refusedRate >= 1);
    }

    void htmlContentsNeverReachALog()
    {
        g_messages.clear();
        const auto previous = qInstallMessageHandler(capture);
        QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true"));
        {
            Rig rig;
            QSignalSpy htmlRequested(&rig.clipboard, &Clipboard::hostHtmlRequested);
            QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
            rig.clipboard.setGraphicsDelivered();
            rig.clipboard.announceHost(true, false, true);
            rig.clientDataRequest(ClipboardHtml::HtmlFormatId);
            QTRY_COMPARE(htmlRequested.size(), 1);
            rig.clipboard.provideHostHtml(htmlRequested.first().first().toULongLong(), (Canary + QStringLiteral(" host")).toUtf8(), true);
            QTest::qWait(Clipboard::EchoWindowMs + 100);
            rig.clientFormatListNamed({{CF_UNICODETEXT, {}}, {ClientHtmlId, "HTML Format"}});
            QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
            rig.clientDataResponse(utf16z(Canary + QStringLiteral(" text")));
            QTRY_COMPARE(rig.sent.dataRequests.size(), 2);
            rig.clientDataResponse(QByteArray("Version:0.9\r\nStartHTML:oops\r\n") + Canary.toUtf8()); // malformed: an Info line
            QTRY_COMPARE(changed.size(), 1);
            rig.clientFormatListNamed({{ClientHtmlId, "HTML Format"}});
            QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 3, 2000);
            rig.clientDataResponse(ClipboardHtml::wrapForWire((Canary + QStringLiteral(" client")).toUtf8()).data);
            QTRY_COMPARE(changed.size(), 2);
        }
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(previous);
        bool logged = false;
        for (const auto &message : std::as_const(g_messages)) {
            QVERIFY2(!message.contains(QStringLiteral("CANARY")), qPrintable(message));
            logged = logged || message.contains(QStringLiteral("kind=html"));
        }
        QVERIFY(logged);
    }

    // ---- OPT-077: failure responses (capability / failure review) ----
    void everyRefusalOfAHostPasteIsACompleteFailureResponse()
    {
        Rig rig;
        QSignalSpy textRequested(&rig.clipboard, &Clipboard::hostTextRequested);
        QSignalSpy imageRequested(&rig.clipboard, &Clipboard::hostImageRequested);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHost(true, true, false);
        // Image: worker refuses, then an oversize PNG, then a malformed one: each a prompt CB_RESPONSE_FAIL with no data.
        const auto fails = [&](int count) {
            QCOMPARE(rig.sent.dataResponses.size(), count);
            QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
            QVERIFY(rig.sent.dataResponses.last().data.isEmpty());
        };
        rig.clientDataRequest(ClipboardImage::PngFormatId);
        QTRY_COMPARE(imageRequested.size(), 1);
        rig.clipboard.provideHostImage(imageRequested.last().first().toULongLong(), {}, false);
        fails(1);
        rig.clientDataRequest(ClipboardImage::PngFormatId);
        QTRY_COMPARE(imageRequested.size(), 2);
        rig.clipboard.provideHostImage(imageRequested.last().first().toULongLong(), QByteArray("not a png"), true);
        fails(2);
        rig.clientDataRequest(CF_UNICODETEXT);
        QTRY_COMPARE(textRequested.size(), 1);
        rig.clipboard.provideHostText(textRequested.last().first().toULongLong(), {}, false);
        fails(3);
        // A format that was never announced.
        rig.clientDataRequest(ClipboardHtml::HtmlFormatId);
        QTRY_COMPARE(rig.sent.dataResponses.size(), 4);
        fails(4);
    }

    void rateLimitCountsSetsAndBytesOverTime()
    {
        ClipboardLimits::RateLimit limit;
        for (int i = 0; i < ClipboardLimits::MaxSets; ++i) QVERIFY(limit.accept(i * 100, 10));
        QVERIFY(!limit.accept(2000, 10)); // the 21st in 10 s
        QVERIFY(!limit.accept(ClipboardLimits::SetWindowMs - 1, 10));
        QVERIFY(limit.accept(ClipboardLimits::SetWindowMs + 1000, 10)); // the first ones have aged out
        // Bytes: 64 MiB a minute (text and images share it).
        ClipboardLimits::RateLimit bytes;
        QVERIFY(bytes.accept(0, ClipboardLimits::MaxBytesPerMinute - 1));
        QVERIFY(bytes.accept(1, 1));
        QVERIFY(!bytes.accept(2, 1));
        QVERIFY(!bytes.accept(ClipboardLimits::ByteWindowMs - 1, 1));
        QVERIFY(bytes.accept(ClipboardLimits::ByteWindowMs + 1, 1));
        // A refused copy is not counted against the next one.
        ClipboardLimits::RateLimit refused;
        QVERIFY(!refused.accept(0, ClipboardLimits::MaxBytesPerMinute + 1));
        QVERIFY(refused.accept(1, ClipboardLimits::MaxBytesPerMinute));
    }

    void anEchoOfTheHostTextIsNeitherRequestedNorWrittenBack()
    {
        Rig rig;
        QSignalSpy requested(&rig.clipboard, &Clipboard::hostTextRequested);
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        rig.clipboard.announceHostText();
        rig.clientDataRequest();
        QTRY_COMPARE(requested.size(), 1);
        rig.clipboard.provideHostText(requested.first().first().toULongLong(), QStringLiteral("host text"), true);
        // The client re-offers what it just pasted (Klipper re-owning it): taken for an echo.
        rig.clientFormatList();
        QTest::qWait(Clipboard::FormatListSettleMs * 2);
        QVERIFY(rig.sent.dataRequests.isEmpty());
        // And a response carrying exactly the host's text is not written back.
        rig.clientDataResponse(ansi(QStringLiteral("host text")));
        QTest::qWait(30);
        QCOMPARE(changed.size(), 0);
    }

    void stockClientRulesAreKept()
    {
        // A client that refuses its first request is never asked again (wlfreerdp3, AUD-FIX6 F1).
        Rig rig;
        QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
        rig.clipboard.setGraphicsDelivered();
        rig.clientFormatList();
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        rig.clientDataResponse({}, CB_RESPONSE_FAIL, 0);
        QTest::qWait(30);
        rig.clientFormatList();
        QTest::qWait(Clipboard::FormatListSettleMs * 2);
        QCOMPARE(rig.sent.dataRequests.size(), 1);
        QCOMPARE(changed.size(), 0);
    }

    void contentsNeverReachALog()
    {
        g_messages.clear();
        const auto previous = qInstallMessageHandler(capture);
        QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true"));
        {
            Rig rig;
            QSignalSpy requested(&rig.clipboard, &Clipboard::hostTextRequested);
            QSignalSpy changed(&rig.clipboard, &Clipboard::clientDataChanged);
            rig.clipboard.setGraphicsDelivered();
            rig.clipboard.announceHostText();
            rig.clientDataRequest();
            QTRY_COMPARE(requested.size(), 1);
            rig.clipboard.provideHostText(requested.first().first().toULongLong(), Canary + QStringLiteral(" host"), true);
            rig.clientDataResponse(ansi(Canary + QStringLiteral(" client")));
            QTRY_COMPARE(changed.size(), 1);
            rig.clientDataResponse(ansi(QString(ClipboardLimits::MaxTextBytes + 10, QLatin1Char('x')) + Canary)); // oversize: an Info line
            for (int i = 0; i < 30; ++i) rig.clientDataResponse(ansi(Canary + QString::number(i))); // rate: an Info line
            QTest::qWait(60);
        }
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(previous);
        QVERIFY(!g_messages.isEmpty()); // the lines that carry counts were emitted at all
        bool sawRefusal = false;
        for (const auto &message : std::as_const(g_messages)) {
            QVERIFY2(!message.contains(QStringLiteral("CANARY")), qPrintable(message));
            sawRefusal |= message.startsWith(QStringLiteral("Clipboard: refused"));
        }
        QVERIFY(sawRefusal);
    }
};

QTEST_GUILESS_MAIN(ClipboardBridgeTest)

#include "ClipboardBridgeTest.moc"
