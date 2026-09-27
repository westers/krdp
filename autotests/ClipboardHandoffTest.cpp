// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QSignalSpy>
#include <QTest>

#include <chrono>
#include <future>
#include <thread>

#include "Clipboard.h"

using namespace KRdp;
using namespace std::chrono_literals;

namespace
{
// What FreeRDP's cliprdr thread does for a client copy: announce the formats,
// then answer the data request with UTF-16 "hi".
UINT deliverClientCopy(CliprdrServerContext *context)
{
    CLIPRDR_FORMAT format{};
    format.formatId = CF_UNICODETEXT;
    CLIPRDR_FORMAT_LIST list{};
    list.common.msgType = CB_FORMAT_LIST;
    list.numFormats = 1;
    list.formats = &format;
    UINT rc = context->ClientFormatList(context, &list);

    static const char16_t text[] = u"hi";
    CLIPRDR_FORMAT_DATA_RESPONSE response{};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    response.common.dataLen = sizeof(text);
    response.requestedFormatData = reinterpret_cast<const BYTE *>(text);
    rc |= context->ClientFormatDataResponse(context, &response);

    CLIPRDR_FORMAT_DATA_REQUEST request{};
    request.requestedFormatId = CF_UNICODETEXT;
    rc |= context->ClientFormatDataRequest(context, &request);
    return rc;
}
}

// AUD-P5: the cliprdr callbacks must not wait for the main thread. They used
// a BlockingQueuedConnection, which deadlocked when the main thread was itself
// waiting for the session (and cliprdr) thread during connection teardown.
class ClipboardHandoffTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void callbacksReturnWhileTheMainThreadIsBlocked()
    {
        Clipboard clipboard(nullptr);
        CliprdrServerContext context{};
        Clipboard::installClientCallbacks(&context, &clipboard);
        QSignalSpy changed(&clipboard, &Clipboard::clientDataChanged);

        // The main thread blocks on the worker, as a teardown join would. With
        // a blocking hand-off the worker could never return.
        auto result = std::async(std::launch::async, [&context]() {
            return deliverClientCopy(&context);
        });
        QCOMPARE(result.wait_for(5s), std::future_status::ready);
        QCOMPARE(result.get(), UINT(CHANNEL_RC_OK));
        QCOMPARE(changed.size(), 0); // nothing ran yet: it was handed off

        // The main thread handles the copies once it gets back to its loop.
        QTRY_COMPARE(changed.size(), 1);
        const auto data = clipboard.getClipboard();
        QVERIFY(data);
        QCOMPARE(data->text(), QStringLiteral("hi"));
    }

    void nothingIsHandledAfterClose()
    {
        Clipboard clipboard(nullptr);
        CliprdrServerContext context{};
        Clipboard::installClientCallbacks(&context, &clipboard);
        QSignalSpy changed(&clipboard, &Clipboard::clientDataChanged);

        // One copy handed off before close, one after: neither is handled.
        std::thread([&context]() {
            deliverClientCopy(&context);
        }).join();
        clipboard.close();
        std::thread([&context]() {
            deliverClientCopy(&context);
        }).join();

        QTest::qWait(50);
        QCOMPARE(changed.size(), 0);
        QVERIFY(!clipboard.enabled());
    }
};

QTEST_GUILESS_MAIN(ClipboardHandoffTest)

#include "ClipboardHandoffTest.moc"
