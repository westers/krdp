// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-067: the broker's cliprdr side of the clipboard bridge (delayed rendering, directions, caps, resets).
// A fake CliprdrServerContext stands in for the FreeRDP channel: no peer, no daemon, no display.

#include <QSignalSpy>
#include <QLoggingCategory>
#include <QTest>

#include "Clipboard.h"
#include "ClipboardLimits.h"

using namespace KRdp;

namespace
{
const QString Canary = QStringLiteral("CANARY-91c2d4-never-in-a-log");

struct Sent {
    QList<QList<uint32_t>> formatLists;
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
    for (uint32_t i = 0; i < list->numFormats; ++i) ids.append(list->formats[i].formatId);
    g_sent->formatLists.append(ids);
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

    void rateLimitCountsSetsAndBytesOverTime()
    {
        ClipboardLimits::RateLimit limit;
        for (int i = 0; i < ClipboardLimits::MaxSets; ++i) QVERIFY(limit.accept(i * 100, 10));
        QVERIFY(!limit.accept(2000, 10)); // the 21st in 10 s
        QVERIFY(!limit.accept(ClipboardLimits::SetWindowMs - 1, 10));
        QVERIFY(limit.accept(ClipboardLimits::SetWindowMs + 1000, 10)); // the first ones have aged out
        // Bytes: 8 MiB a minute.
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
