// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 3: the broker's cliprdr side of client-to-host file copy (the client's format list, the lock, the descriptor, hostile
// descriptors, the worker's answer, windowed FILECONTENTS requests, acknowledgements, replace/cancel/reset, hostile answers, timeouts, logs)
// against a fake CliprdrServerContext and a fake worker backend. No peer, no worker, no display.

#include <QJsonObject>
#include <QLoggingCategory>
#include <QSignalSpy>
#include <QTest>

#include "Clipboard.h"
#include "ClipboardFileDescriptor.h"
#include "ClipboardFileFetch.h"
#include "ClipboardFileLimits.h"

using namespace KRdp;
using namespace Qt::StringLiterals;

namespace
{
constexpr quint64 GiB = 1024ULL * 1024 * 1024;
constexpr uint32_t FileGroupId = 0xC0A1;
constexpr uint32_t FileContentsId = 0xC0A2;
const QString Canary = u"CANARY-91ad03-do-not-log"_s;

struct Sent {
    QStringList events; // in order: "lock:N", "unlock:N", "datareq:ID"
    QList<uint32_t> locks;
    QList<uint32_t> unlocks;
    QList<uint32_t> dataRequests;
    struct Contents {
        uint32_t streamId, index, flags, length;
        uint64_t offset;
        bool haveLock;
        uint32_t lockId;
    };
    QList<Contents> requests;
};
Sent *g_sent = nullptr;

UINT fakeFormatList(CliprdrServerContext *, const CLIPRDR_FORMAT_LIST *) { return CHANNEL_RC_OK; }
UINT fakeFormatListResponse(CliprdrServerContext *, const CLIPRDR_FORMAT_LIST_RESPONSE *) { return CHANNEL_RC_OK; }
UINT fakeDataResponse(CliprdrServerContext *, const CLIPRDR_FORMAT_DATA_RESPONSE *) { return CHANNEL_RC_OK; }
UINT fakeDataRequest(CliprdrServerContext *, const CLIPRDR_FORMAT_DATA_REQUEST *request)
{
    if (!g_sent) return CHANNEL_RC_OK;
    g_sent->dataRequests.append(request->requestedFormatId);
    g_sent->events.append(QStringLiteral("datareq:%1").arg(request->requestedFormatId, 0, 16));
    return CHANNEL_RC_OK;
}
UINT fakeLock(CliprdrServerContext *, const CLIPRDR_LOCK_CLIPBOARD_DATA *lock)
{
    if (!g_sent) return CHANNEL_RC_OK;
    g_sent->locks.append(lock->clipDataId);
    g_sent->events.append(QStringLiteral("lock:%1").arg(lock->clipDataId));
    return CHANNEL_RC_OK;
}
UINT fakeUnlock(CliprdrServerContext *, const CLIPRDR_UNLOCK_CLIPBOARD_DATA *unlock)
{
    if (!g_sent) return CHANNEL_RC_OK;
    g_sent->unlocks.append(unlock->clipDataId);
    g_sent->events.append(QStringLiteral("unlock:%1").arg(unlock->clipDataId));
    return CHANNEL_RC_OK;
}
UINT fakeContentsRequest(CliprdrServerContext *, const CLIPRDR_FILE_CONTENTS_REQUEST *request)
{
    if (!g_sent) return CHANNEL_RC_OK;
    g_sent->requests.append({request->streamId, request->listIndex, request->dwFlags, request->cbRequested, (uint64_t(request->nPositionHigh) << 32) | request->nPositionLow,
                             bool(request->haveClipDataId), request->clipDataId});
    return CHANNEL_RC_OK;
}

QStringList g_messages;
void capture(QtMsgType, const QMessageLogContext &, const QString &message)
{
    g_messages.append(message);
}

QByteArray descriptorOf(const QList<ClipboardFileDescriptor::EncodeItem> &items)
{
    return ClipboardFileDescriptor::encode(items).data;
}
// 0: dir, 1: 600 KiB file, 2: empty file, 3: 100 bytes
QList<ClipboardFileDescriptor::EncodeItem> defaultItems()
{
    return {{u"docs"_s, true, 0, std::nullopt}, {u"docs\\a.bin"_s, false, 600 * 1024, std::nullopt}, {u"empty"_s, false, 0, std::nullopt}, {u"b.txt"_s, false, 100, std::nullopt}};
}

struct Rig {
    // (the Clipboard is the LAST member: its destructor reports to the backend lambdas, which use the members above)
    Sent sent;
    struct Offer {
        QByteArray descriptor;
        bool huge;
    };
    QList<Offer> offers;
    struct Data {
        quint64 seq;
        quint32 index;
        quint64 offset;
        QByteArray data;
    };
    QList<Data> data;
    QList<QPair<quint64, int>> cancels;
    quint64 nextSeq = 1;
    bool refuseOffers = false;
    bool refuseData = false;
    QList<QJsonObject> notices;
    CliprdrServerContext context{};
    Clipboard clipboard{nullptr};
    QSignalSpy changed{&clipboard, &Clipboard::clientDataChanged}; // the console host connects to this; requests need a taker

    explicit Rig(bool negotiateStream = true, bool lock = true, bool huge = true)
    {
        g_sent = &sent;
        context.ServerFormatList = fakeFormatList;
        context.ServerFormatListResponse = fakeFormatListResponse;
        context.ServerFormatDataResponse = fakeDataResponse;
        context.ServerFormatDataRequest = fakeDataRequest;
        context.ServerLockClipboardData = fakeLock;
        context.ServerUnlockClipboardData = fakeUnlock;
        context.ServerFileContentsRequest = fakeContentsRequest;
        Clipboard::applyFileCapabilities(&context, true);
        context.streamFileClipEnabled = negotiateStream; // what the capability exchange leaves: the intersection
        context.canLockClipData = lock;
        context.hasHugeFileSupport = huge;
        Clipboard::installClientCallbacks(&context, &clipboard);
        clipboard.attachContextForTest(&context);
        clipboard.setFilesEnabled(true);
        clipboard.setDirections(true, true);
        clipboard.setFileDirections(false, true);
        clipboard.setStockClientForTest(false);
        clipboard.setFileTransferRequest({ClipboardFilePacing::RequestedMode::Unlimited, std::nullopt}); // the pacer has its own tests: here the window is all that limits
        Clipboard::ClientFileBackend backend;
        backend.offer = [this](const QByteArray &descriptor, bool huge) -> quint64 {
            if (refuseOffers) return 0;
            offers.append({descriptor, huge});
            return nextSeq++;
        };
        backend.data = [this](quint64 seq, quint32 index, quint64 offset, const QByteArray &bytes) {
            if (refuseData) return false;
            data.append({seq, index, offset, bytes});
            return true;
        };
        backend.cancel = [this](quint64 seq, int reason) { cancels.append({seq, reason}); };
        clipboard.setClientFileBackend(backend);
        clipboard.setNoticeSinkForTest([this](const QJsonObject &notice) { notices.append(notice); });
        clipboard.setGraphicsDelivered();
    }
    ~Rig() { g_sent = nullptr; }

    /// The client announces files (and optionally other formats).
    UINT formatList(const QList<QPair<uint32_t, QByteArray>> &formats)
    {
        QList<CLIPRDR_FORMAT> list;
        for (const auto &format : formats) {
            CLIPRDR_FORMAT f{};
            f.formatId = format.first;
            f.formatName = format.second.isEmpty() ? nullptr : const_cast<char *>(format.second.constData());
            list.append(f);
        }
        CLIPRDR_FORMAT_LIST formatList{};
        formatList.common.msgType = CB_FORMAT_LIST;
        formatList.numFormats = uint32_t(list.size());
        formatList.formats = list.data();
        return context.ClientFormatList(&context, &formatList);
    }
    UINT announceFiles() { return formatList({{FileGroupId, QByteArrayLiteral("FileGroupDescriptorW")}, {FileContentsId, QByteArrayLiteral("FileContents")}}); }
    UINT descriptorResponse(const QByteArray &bytes, bool ok = true)
    {
        CLIPRDR_FORMAT_DATA_RESPONSE response{};
        response.common.msgType = CB_FORMAT_DATA_RESPONSE;
        response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
        response.common.dataLen = uint32_t(bytes.size());
        response.requestedFormatData = reinterpret_cast<const BYTE *>(bytes.constData());
        return context.ClientFormatDataResponse(&context, &response);
    }
    UINT contentsResponse(uint32_t streamId, const QByteArray &bytes, bool ok = true, std::optional<uint32_t> declared = std::nullopt)
    {
        CLIPRDR_FILE_CONTENTS_RESPONSE response{};
        response.common.msgType = CB_FILECONTENTS_RESPONSE;
        response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
        response.streamId = streamId;
        response.cbRequested = declared.value_or(uint32_t(bytes.size()));
        response.common.dataLen = response.cbRequested + 4;
        response.requestedData = reinterpret_cast<const BYTE *>(bytes.constData());
        return context.ClientFileContentsResponse(&context, &response);
    }
    /// announce, wait for the descriptor request, answer it
    void offerFiles(const QByteArray &descriptor = descriptorOf(defaultItems()))
    {
        const auto before = sent.dataRequests.size();
        announceFiles();
        QTRY_COMPARE_WITH_TIMEOUT(sent.dataRequests.size(), before + 1, 2000);
        descriptorResponse(descriptor);
    }
    Clipboard::ClientFilesReply accepted(quint64 seq, const QByteArray &descriptor = descriptorOf(defaultItems()))
    {
        const auto plan = ClipboardFilePush::makePlan(descriptor, true);
        Clipboard::ClientFilesReply reply;
        reply.seq = seq;
        reply.accepted = true;
        reply.files = quint32(plan.files);
        reply.directories = quint32(plan.directories);
        reply.skipped = quint32(plan.skippedTotal());
        reply.totalBytes = plan.totalBytes;
        return reply;
    }
    /// answers every request that is out with the right bytes, acknowledges what reached the worker; until nothing is left to do
    int drive(int maxRounds = 500)
    {
        int answered = 0;
        int acked = 0;
        for (int round = 0; round < maxRounds; ++round) {
            bool progress = false;
            while (answered < sent.requests.size()) {
                const auto &request = sent.requests.at(answered++);
                contentsResponse(request.streamId, QByteArray(int(request.length), char('a' + (request.index & 7))));
                progress = true;
            }
            QCoreApplication::processEvents();
            while (acked < data.size()) {
                const auto &chunk = data.at(acked++);
                clipboard.clientFilesAcked(chunk.seq, chunk.index, chunk.offset, quint32(chunk.data.size()), int(ClipboardFileFetch::AckStatus::Ok));
                progress = true;
            }
            QCoreApplication::processEvents();
            QTest::qWait(12);
            if (!progress && answered == sent.requests.size() && acked == data.size()) break;
        }
        return answered;
    }
    QStringList noticeEvents() const
    {
        QStringList out;
        for (const auto &notice : notices) out.append(notice.value(u"event"_s).toString() + u":"_s + notice.value(u"reason"_s).toString());
        return out;
    }
};
}

class ClipboardClientFilesBridgeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        g_messages.clear();
    }

    // The lock comes BEFORE the descriptor request so the client snapshots what the host is about to read.
    void theClipboardIsLockedThenTheDescriptorRequested()
    {
        Rig rig;
        rig.announceFiles();
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), FileGroupId); // the file group, never FileContents, never text
        QCOMPARE(rig.sent.events.size(), 2);
        QVERIFY(rig.sent.events.at(0).startsWith(u"lock:"_s));
        QVERIFY(rig.sent.events.at(1).startsWith(u"datareq:"_s));
        QVERIFY(!rig.clipboard.clientFileTransferActive()); // asked for, not started
    }
    void withoutLockNegotiatedNoLockIsSent()
    {
        Rig rig(true, false);
        rig.offerFiles();
        QVERIFY(rig.sent.locks.isEmpty());
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1));
        QTRY_VERIFY(!rig.sent.requests.isEmpty());
        QVERIFY(!rig.sent.requests.first().haveLock);
    }
    void filesNeedStreamingNegotiatedAndTheFlag()
    {
        {
            Rig rig(false); // CB_STREAM_FILECLIP_ENABLED was not negotiated
            rig.announceFiles();
            QTest::qWait(Clipboard::FormatListSettleMs * 3);
            QVERIFY(rig.sent.dataRequests.isEmpty());
            QVERIFY(rig.offers.isEmpty());
        }
        {
            Rig denied; // the planner says no (locked desktop, new transfer)
            denied.clipboard.setFileDirections(false, false);
            denied.announceFiles();
            QTest::qWait(Clipboard::FormatListSettleMs * 3);
            QVERIFY(denied.sent.dataRequests.isEmpty());
            QCOMPARE(denied.noticeEvents(), QStringList({u"refused:policy"_s}));
        }
        {
            Rig off; // the host owner turned files off
            off.clipboard.setFilesEnabled(false);
            off.announceFiles();
            QTest::qWait(Clipboard::FormatListSettleMs * 3);
            QVERIFY(off.sent.dataRequests.isEmpty());
            QVERIFY(off.notices.isEmpty());
        }
    }
    void textStillWorksWhenFilesAreDenied()
    {
        Rig rig;
        rig.clipboard.setFileDirections(false, false);
        rig.formatList({{FileGroupId, QByteArrayLiteral("FileGroupDescriptorW")}, {CF_UNICODETEXT, {}}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), uint32_t(CF_UNICODETEXT));
        QVERIFY(rig.sent.locks.isEmpty());
    }
    void filesWinOverTextAndImagesInOneList()
    {
        Rig rig;
        rig.formatList({{CF_UNICODETEXT, {}}, {FileGroupId, QByteArrayLiteral("FileGroupDescriptorW")}, {FileContentsId, QByteArrayLiteral("FileContents")}});
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        QCOMPARE(rig.sent.dataRequests.first(), FileGroupId);
    }

    // ---- the descriptor is hostile input ----
    void aGoodDescriptorGoesToTheWorkerAndTheClientIsAskedForBytesOnlyAfterTheWorkerAccepted()
    {
        Rig rig;
        const auto descriptor = descriptorOf(defaultItems());
        rig.offerFiles(descriptor);
        QTRY_COMPARE(rig.offers.size(), 1);
        QCOMPARE(rig.offers.first().descriptor, descriptor);
        QVERIFY(rig.offers.first().huge);
        QVERIFY(rig.sent.requests.isEmpty()); // nothing is asked of the client before the worker has said yes
        QVERIFY(rig.clipboard.clientFileTransferActive());
        rig.clipboard.clientFilesReplied(rig.accepted(1, descriptor));
        QTRY_VERIFY(!rig.sent.requests.isEmpty());
        QCOMPARE(rig.sent.requests.first().index, 1u); // the 600 KiB file; the empty file and the folder need no request
        QCOMPARE(rig.sent.requests.first().flags, 2u); // FILECONTENTS_RANGE
        QCOMPARE(rig.sent.requests.first().offset, quint64(0));
        QVERIFY(rig.sent.requests.first().haveLock);
        QCOMPARE(rig.sent.requests.first().lockId, rig.sent.locks.first());
        QCOMPARE(rig.clipboard.counters().clientFileLists, quint64(1));
    }
    void hostileDescriptorsNeverReachTheWorkerAndTheLockIsReleased_data()
    {
        QTest::addColumn<QByteArray>("descriptor");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("torn") << descriptorOf(defaultItems()).left(300);
        QTest::newRow("count lies") << []() {
            QByteArray d = descriptorOf(defaultItems());
            qToLittleEndian<quint32>(0x7fffffff, d.data());
            return d;
        }();
        QTest::newRow("traversal") << descriptorOf({{u"ok"_s, false, 1, std::nullopt}, {u"..\\x"_s, false, 1, std::nullopt}});
        QTest::newRow("absolute") << descriptorOf({{u"\\etc\\passwd"_s, false, 1, std::nullopt}});
        QTest::newRow("drive") << descriptorOf({{u"C:\\x"_s, false, 1, std::nullopt}});
        QTest::newRow("canary traversal") << descriptorOf({{u"a\\..\\..\\"_s + Canary, false, 1, std::nullopt}});
        QTest::newRow("nothing to copy") << descriptorOf({});
    }
    void hostileDescriptorsNeverReachTheWorkerAndTheLockIsReleased()
    {
        QFETCH(QByteArray, descriptor);
        Rig rig;
        rig.offerFiles(descriptor);
        QTest::qWait(60);
        QVERIFY(rig.offers.isEmpty());
        QVERIFY(rig.sent.requests.isEmpty());
        QCOMPARE(rig.sent.unlocks, rig.sent.locks);
        QVERIFY(!rig.clipboard.clientFileTransferActive());
        QCOMPARE(rig.clipboard.counters().clientFileRefused, quint64(1));
        QVERIFY(rig.noticeEvents().first().startsWith(u"refused:"_s));
    }
    void anOversizedDescriptorIsNotCopied()
    {
        Rig rig;
        rig.announceFiles();
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        CLIPRDR_FORMAT_DATA_RESPONSE response{};
        response.common.msgFlags = CB_RESPONSE_OK;
        response.common.dataLen = uint32_t(ClipboardFileLimits::MaxDescriptorWireBytes + 1);
        static const BYTE none = 0;
        response.requestedFormatData = &none; // never read: the bridge does not copy what exceeds the cap
        rig.context.ClientFormatDataResponse(&rig.context, &response);
        QTest::qWait(60);
        QVERIFY(rig.offers.isEmpty());
        QCOMPARE(rig.sent.unlocks, rig.sent.locks);
        QCOMPARE(rig.clipboard.counters().clientFileRefused, quint64(1));
    }
    void aClientThatRefusesTheDescriptorReleasesTheLock()
    {
        Rig rig;
        rig.announceFiles();
        QTRY_COMPARE_WITH_TIMEOUT(rig.sent.dataRequests.size(), 1, 2000);
        rig.descriptorResponse({}, false);
        QTest::qWait(60);
        QCOMPARE(rig.sent.unlocks, rig.sent.locks);
        QVERIFY(rig.offers.isEmpty());
    }
    void filesOverFourGibNeedTheNegotiatedHugeFlag()
    {
        Rig rig(true, true, false);
        rig.offerFiles(descriptorOf({{u"big"_s, false, 5 * GiB, std::nullopt}, {u"small"_s, false, 10, std::nullopt}}));
        QTRY_COMPARE(rig.offers.size(), 1);
        QVERIFY(!rig.offers.first().huge);
        // the worker would skip the big one; the broker's plan agrees: only the small one is fetched
        auto reply = rig.accepted(1, descriptorOf({{u"small"_s, false, 10, std::nullopt}}));
        reply.skipped = 1;
        rig.clipboard.clientFilesReplied(reply);
        QTRY_VERIFY(!rig.sent.requests.isEmpty());
        QCOMPARE(rig.sent.requests.first().index, 1u);
        QCOMPARE(rig.sent.requests.first().length, 10u);
    }

    // ---- the worker's answer ----
    void aRefusalIsToldToTheClientAndTheLockReleased()
    {
        Rig rig;
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        Clipboard::ClientFilesReply reply;
        reply.seq = 1;
        reply.accepted = false;
        reply.reason = u"no-space"_s;
        reply.freeBytes = 3 * GiB;
        reply.neededBytes = 7 * GiB;
        rig.clipboard.clientFilesReplied(reply);
        QVERIFY(rig.sent.requests.isEmpty());
        QCOMPARE(rig.sent.unlocks, rig.sent.locks);
        QVERIFY(!rig.clipboard.clientFileTransferActive());
        QCOMPARE(rig.noticeEvents(), QStringList({u"refused:no-space"_s}));
        QCOMPARE(rig.clipboard.counters().clientFileRefused, quint64(1));
        // a stray reply for another seq is ignored
        rig.clipboard.clientFilesReplied(rig.accepted(99));
        QVERIFY(rig.sent.requests.isEmpty());
    }
    void aWorkerThatDisagreesAboutTheTotalsIsNotTrusted()
    {
        Rig rig;
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        auto reply = rig.accepted(1);
        reply.totalBytes += 1;
        rig.clipboard.clientFilesReplied(reply);
        QVERIFY(rig.sent.requests.isEmpty());
        QVERIFY(!rig.cancels.isEmpty());
        QCOMPARE(rig.sent.unlocks, rig.sent.locks);
        QVERIFY(!rig.clipboard.clientFileTransferActive());
    }
    void aWorkerThatNeverAnswersEndsTheTransfer()
    {
        Rig rig;
        rig.clipboard.setClientFileTimeoutsForTest(150, 150);
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!rig.clipboard.clientFileTransferActive(), 2000);
        QVERIFY(!rig.cancels.isEmpty());
        QCOMPARE(rig.sent.unlocks, rig.sent.locks);
    }
    void aWorkerThatRefusesTheOfferEndsItAtOnce()
    {
        Rig rig;
        rig.refuseOffers = true;
        rig.offerFiles();
        QTest::qWait(60);
        QVERIFY(!rig.clipboard.clientFileTransferActive());
        QCOMPARE(rig.sent.unlocks, rig.sent.locks);
        QVERIFY(rig.noticeEvents().first().startsWith(u"refused:"_s));
    }

    // ---- the transfer ----
    void aWholeTransferFlowsThroughTheWindowAndEndsInDone()
    {
        Rig rig;
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1));
        QCOMPARE(rig.noticeEvents(), QStringList({u"receiving:"_s}));
        QCOMPARE(rig.notices.first().value(u"direction"_s).toString(), u"client-to-host"_s);
        QCOMPARE(rig.notices.first().value(u"count"_s).toInt(), 3);
        rig.drive();
        // everything the plan fetches arrived, in order, exactly once: file 1 (600 KiB) then file 3 (100 B)
        quint64 total = 0;
        quint32 lastIndex = 0;
        quint64 expectedOffset = 0;
        for (const auto &chunk : std::as_const(rig.data)) {
            QCOMPARE(chunk.seq, quint64(1));
            if (chunk.index != lastIndex) {
                lastIndex = chunk.index;
                expectedOffset = 0;
            }
            QCOMPARE(chunk.offset, expectedOffset);
            expectedOffset += quint64(chunk.data.size());
            total += quint64(chunk.data.size());
        }
        QCOMPARE(total, quint64(600 * 1024 + 100));
        QCOMPARE(rig.data.last().index, 3u);
        QVERIFY(rig.clipboard.clientFileTransferActive()); // the worker has not said Done yet
        QCOMPARE(rig.sent.unlocks, rig.sent.locks); // but the client's snapshot is released as soon as every byte is acknowledged
        rig.clipboard.clientFilesState(1, Clipboard::ClientFilesPhase::Done, QString(), total, total);
        QVERIFY(!rig.clipboard.clientFileTransferActive());
        QCOMPARE(rig.sent.unlocks, rig.sent.locks); // the lock is released when the files are all there
        QCOMPARE(rig.noticeEvents().last(), u"done:"_s);
        QCOMPARE(rig.clipboard.counters().clientFileBytes, total);
        QVERIFY(rig.cancels.isEmpty()); // a finished copy is not cancelled: it stays staged for the paste
    }
    void everyRequestCarriesTheLockAndNoneExceedsTheRangeSizeOrTheWindow()
    {
        Rig rig;
        rig.offerFiles(descriptorOf({{u"big"_s, false, 50ULL * 1024 * 1024, std::nullopt}}));
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1, descriptorOf({{u"big"_s, false, 50ULL * 1024 * 1024, std::nullopt}})));
        QTRY_VERIFY(!rig.sent.requests.isEmpty());
        QTest::qWait(60);
        QVERIFY(rig.sent.requests.size() <= ClipboardFileLimits::MaxWindow); // nothing is answered: the window is all that is out
        for (const auto &request : std::as_const(rig.sent.requests)) {
            QVERIFY(request.haveLock);
            QVERIFY(request.length >= 1 && request.length <= ClipboardFileLimits::RangeBytes);
            QCOMPARE(request.flags, 2u);
        }
        QSet<uint32_t> ids;
        for (const auto &request : std::as_const(rig.sent.requests)) ids.insert(request.streamId);
        QCOMPARE(ids.size(), rig.sent.requests.size());
    }
    void aStockClientGetsAWindowOfFour()
    {
        Rig rig(true, false);
        rig.clipboard.setStockClientForTest(true);
        rig.offerFiles(descriptorOf({{u"big"_s, false, 50ULL * 1024 * 1024, std::nullopt}}));
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1, descriptorOf({{u"big"_s, false, 50ULL * 1024 * 1024, std::nullopt}})));
        QTRY_VERIFY(!rig.sent.requests.isEmpty());
        QTest::qWait(100);
        QVERIFY(rig.sent.requests.size() <= ClipboardFileLimits::StockClientWindow);
    }
    void aWorkerThatRefusesARangeStopsTheTransfer()
    {
        Rig rig;
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1));
        QTRY_VERIFY(!rig.sent.requests.isEmpty());
        rig.contentsResponse(rig.sent.requests.first().streamId, QByteArray(int(rig.sent.requests.first().length), 'x'));
        QTRY_VERIFY(!rig.data.isEmpty());
        rig.clipboard.clientFilesAcked(1, rig.data.first().index, rig.data.first().offset, quint32(rig.data.first().data.size()), int(ClipboardFileFetch::AckStatus::NoSpace));
        QVERIFY(!rig.clipboard.clientFileTransferActive());
        QCOMPARE(rig.sent.unlocks, rig.sent.locks);
        QCOMPARE(rig.clipboard.counters().clientFileFailed, quint64(1));
        QVERIFY(rig.noticeEvents().last().startsWith(u"failed:"_s));
    }
    void theWorkersFailureStateEndsTheTransfer()
    {
        Rig rig;
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1));
        rig.clipboard.clientFilesState(1, Clipboard::ClientFilesPhase::Failed, u"no-space"_s, 10, 100);
        QVERIFY(!rig.clipboard.clientFileTransferActive());
        QCOMPARE(rig.sent.unlocks, rig.sent.locks);
        QCOMPARE(rig.noticeEvents().last(), u"failed:no-space"_s);
        QVERIFY(rig.cancels.isEmpty()); // the worker already removed it
    }

    // ---- hostile answers (spec 4.3) ----
    void aShortAnswerEndsTheTransferAndTellsTheWorker()
    {
        Rig rig;
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1));
        QTRY_VERIFY(!rig.sent.requests.isEmpty());
        rig.contentsResponse(rig.sent.requests.first().streamId, QByteArray(10, 'x'));
        QTRY_VERIFY(!rig.clipboard.clientFileTransferActive());
        QVERIFY(rig.data.isEmpty()); // nothing short is passed on
        QVERIFY(!rig.cancels.isEmpty());
        QCOMPARE(rig.sent.unlocks, rig.sent.locks);
        QVERIFY(rig.noticeEvents().last().startsWith(u"failed:"_s));
    }
    void anOversizedAnswerIsNotCopiedAndEndsTheTransfer()
    {
        Rig rig;
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1));
        QTRY_VERIFY(!rig.sent.requests.isEmpty());
        static const char none = 0;
        rig.contentsResponse(rig.sent.requests.first().streamId, QByteArray::fromRawData(&none, 1), true, ClipboardFileLimits::MaxRangeBytes + 1);
        QTRY_VERIFY(!rig.clipboard.clientFileTransferActive());
        QVERIFY(rig.data.isEmpty());
    }
    void aFailedAnswerEndsTheTransfer()
    {
        Rig rig;
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1));
        QTRY_VERIFY(!rig.sent.requests.isEmpty());
        rig.contentsResponse(rig.sent.requests.first().streamId, {}, false);
        QTRY_VERIFY(!rig.clipboard.clientFileTransferActive());
        QVERIFY(rig.noticeEvents().last().startsWith(u"failed:"_s));
    }
    void answersNobodyAskedForAbortAtTheThird()
    {
        Rig rig;
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1));
        QTRY_VERIFY(!rig.sent.requests.isEmpty());
        for (uint32_t id : {0xdead0001u, 0xdead0002u}) rig.contentsResponse(id, QByteArray(4, 'x'));
        QTest::qWait(40);
        QVERIFY(rig.clipboard.clientFileTransferActive());
        rig.contentsResponse(0xdead0003u, QByteArray(4, 'x'));
        QTRY_VERIFY(!rig.clipboard.clientFileTransferActive());
        QVERIFY(rig.data.isEmpty());
    }
    void answersWithNoTransferAreDropped()
    {
        Rig rig;
        rig.contentsResponse(5, QByteArray(10, 'x'));
        QTest::qWait(40);
        QVERIFY(rig.data.isEmpty());
        QVERIFY(!rig.clipboard.clientFileTransferActive());
    }

    // ---- replace, repeat, cancel, end ----
    void aRepeatOfTheSameListIsTheSameCopy()
    {
        Rig rig;
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1));
        rig.offerFiles(); // a stock client announcing again, same files
        QTest::qWait(60);
        QCOMPARE(rig.offers.size(), 1);
        QVERIFY(rig.cancels.isEmpty());
        QVERIFY(rig.clipboard.clientFileTransferActive());
        // the second lock was released; the first is still held
        QCOMPARE(rig.sent.locks.size(), 2);
        QCOMPARE(rig.sent.unlocks, QList<uint32_t>({rig.sent.locks.at(1)}));
    }
    void aDifferentListReplacesTheOldTransfer()
    {
        Rig rig;
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1));
        QTRY_VERIFY(!rig.sent.requests.isEmpty());
        const auto other = descriptorOf({{u"new.txt"_s, false, 7, std::nullopt}});
        rig.offerFiles(other);
        QTRY_COMPARE(rig.offers.size(), 2);
        QVERIFY(!rig.cancels.isEmpty());
        QCOMPARE(rig.cancels.first().first, quint64(1));
        QCOMPARE(rig.cancels.first().second, int(Clipboard::ClientFilesCancel::Replaced));
        QVERIFY(rig.sent.unlocks.contains(rig.sent.locks.first())); // the old lock is released, the new one held
        QVERIFY(!rig.sent.unlocks.contains(rig.sent.locks.last()));
        // a late answer to the old transfer's request is dropped, the new transfer proceeds
        const auto before = rig.sent.requests.size();
        rig.clipboard.clientFilesReplied(rig.accepted(2, other));
        QTRY_VERIFY(rig.sent.requests.size() > before);
        QCOMPARE(rig.sent.requests.last().length, 7u);
    }
    void aCopyOfSomethingElseCancelsTheTransferAndAnEmptyListToo()
    {
        {
            Rig rig;
            rig.offerFiles();
            QTRY_COMPARE(rig.offers.size(), 1);
            rig.clipboard.clientFilesReplied(rig.accepted(1));
            rig.formatList({{CF_UNICODETEXT, {}}});
            QTRY_VERIFY(!rig.clipboard.clientFileTransferActive());
            QCOMPARE(rig.cancels.last().first, quint64(1));
            QCOMPARE(rig.sent.unlocks, rig.sent.locks);
        }
        Rig clear;
        clear.offerFiles();
        QTRY_COMPARE(clear.offers.size(), 1);
        clear.clipboard.clientFilesReplied(clear.accepted(1));
        clear.formatList({});
        QTRY_VERIFY(!clear.clipboard.clientFileTransferActive());
        QVERIFY(!clear.cancels.isEmpty());
    }
    void aFinishedCopyIsReplacedByTheNextOneAndRemovedWithTheConnection()
    {
        Rig rig;
        const auto small = descriptorOf({{u"s.txt"_s, false, 5, std::nullopt}});
        rig.offerFiles(small);
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1, small));
        rig.drive();
        rig.clipboard.clientFilesState(1, Clipboard::ClientFilesPhase::Done, QString(), 5, 5);
        QVERIFY(rig.cancels.isEmpty());
        // a different copy later: the finished one's staging goes
        rig.offerFiles(descriptorOf({{u"t.txt"_s, false, 5, std::nullopt}}));
        QTRY_COMPARE(rig.offers.size(), 2);
        QCOMPARE(rig.cancels.first().first, quint64(1));
        QCOMPARE(rig.cancels.first().second, int(Clipboard::ClientFilesCancel::Replaced));
        QVERIFY(rig.clipboard.clientFileTransferActive());
    }
    void resetAndDenyCancelEverythingIncludingAFinishedCopy()
    {
        Rig rig;
        const auto small = descriptorOf({{u"s.txt"_s, false, 5, std::nullopt}});
        rig.offerFiles(small);
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1, small));
        rig.drive();
        rig.clipboard.clientFilesState(1, Clipboard::ClientFilesPhase::Done, QString(), 5, 5);
        rig.clipboard.resetClientSide(); // control moved, viewer demotion
        QCOMPARE(rig.cancels.last().first, quint64(1));
        QCOMPARE(rig.cancels.last().second, int(Clipboard::ClientFilesCancel::Policy));
        // a transfer in flight and the direction denied
        rig.offerFiles();
        QTRY_COMPARE(rig.offers.size(), 2);
        rig.clipboard.clientFilesReplied(rig.accepted(2));
        rig.clipboard.setDirections(true, false);
        QVERIFY(!rig.clipboard.clientFileTransferActive());
        QCOMPARE(rig.cancels.last().first, quint64(2));
    }
    // spec 12.1: the lock only denies NEW transfers. A finished copy stays when the flag goes; a running transfer is stopped only when the
    // planner really withdraws the flag (it keeps it for a transfer that started), and then what it staged goes.
    void theFlagGoingKeepsAFinishedCopyAndStopsARunningTransfer()
    {
        Rig rig;
        const auto small = descriptorOf({{u"s.txt"_s, false, 5, std::nullopt}});
        rig.offerFiles(small);
        QTRY_COMPARE(rig.offers.size(), 1);
        rig.clipboard.clientFilesReplied(rig.accepted(1, small));
        rig.drive();
        rig.clipboard.clientFilesState(1, Clipboard::ClientFilesPhase::Done, QString(), 5, 5);
        rig.clipboard.setFileDirections(false, false); // the screen locked after the copy: new transfers are denied
        QVERIFY(rig.cancels.isEmpty()); // what was received stays for the paste
        QVERIFY(!rig.clipboard.clientFilesAllowed());
        rig.clipboard.setFileDirections(false, true);
        rig.offerFiles(descriptorOf({{u"big"_s, false, 50ULL * 1024 * 1024, std::nullopt}}));
        QTRY_COMPARE(rig.offers.size(), 2);
        rig.clipboard.clientFilesReplied(rig.accepted(2, descriptorOf({{u"big"_s, false, 50ULL * 1024 * 1024, std::nullopt}})));
        QVERIFY(rig.clipboard.clientFileTransferActive());
        rig.clipboard.setFileDirections(false, false); // withdrawn while running
        QVERIFY(!rig.clipboard.clientFileTransferActive());
        QCOMPARE(rig.cancels.last().first, quint64(2));
        QCOMPARE(rig.cancels.last().second, int(Clipboard::ClientFilesCancel::Policy));
        QCOMPARE(rig.sent.unlocks.size(), rig.sent.locks.size());
    }
    void destroyingTheConnectionCancelsTheTransfer()
    {
        QList<QPair<quint64, int>> cancels;
        {
            Rig rig;
            rig.offerFiles();
            QTRY_COMPARE(rig.offers.size(), 1);
            rig.clipboard.clientFilesReplied(rig.accepted(1));
            auto backend = Clipboard::ClientFileBackend();
            backend.offer = [](const QByteArray &, bool) -> quint64 { return 0; };
            backend.data = [](quint64, quint32, quint64, const QByteArray &) { return true; };
            backend.cancel = [&cancels](quint64 seq, int reason) { cancels.append({seq, reason}); };
            rig.clipboard.setClientFileBackend(backend);
        }
        QVERIFY(!cancels.isEmpty());
        QCOMPARE(cancels.last().first, quint64(0)); // every transfer of the connection
        QCOMPARE(cancels.last().second, int(Clipboard::ClientFilesCancel::ConnectionEnded));
    }
    void copiesInQuickSuccessAreRateLimited()
    {
        Rig rig;
        int refused = 0;
        for (int i = 0; i < 12; ++i) {
            rig.offerFiles(descriptorOf({{QStringLiteral("f%1").arg(i), false, 5, std::nullopt}}));
            QTest::qWait(30);
        }
        refused = int(rig.clipboard.counters().clientFileRefused);
        QVERIFY2(rig.offers.size() < 12 && refused > 0, qPrintable(QStringLiteral("%1 offers, %2 refused").arg(rig.offers.size()).arg(refused)));
    }

    // ---- logs (spec 4.6) ----
    void namesAndContentsNeverReachALog()
    {
        qInstallMessageHandler(capture);
        {
            Rig rig;
            const auto good = descriptorOf({{u"dir-"_s + Canary, true, 0, std::nullopt}, {u"dir-"_s + Canary + u"\\file-"_s + Canary, false, 20, std::nullopt}});
            rig.offerFiles(good);
            QTRY_COMPARE(rig.offers.size(), 1);
            rig.clipboard.clientFilesReplied(rig.accepted(1, good));
            QTRY_VERIFY(!rig.sent.requests.isEmpty());
            rig.contentsResponse(rig.sent.requests.first().streamId, QByteArray(20, 'x'));
            QTRY_VERIFY(!rig.data.isEmpty());
            rig.clipboard.clientFilesState(1, Clipboard::ClientFilesPhase::Failed, u"write-failed"_s, 1, 2);
            // hostile
            rig.offerFiles(descriptorOf({{u"..\\"_s + Canary, false, 1, std::nullopt}}));
            QTest::qWait(60);
            for (const auto &notice : std::as_const(rig.notices)) QVERIFY(!QJsonDocument(notice).toJson().contains(Canary.toUtf8()));
        }
        qInstallMessageHandler(nullptr);
        for (const auto &line : std::as_const(g_messages)) QVERIFY2(!line.contains(Canary), qPrintable(line));
    }
};

QTEST_GUILESS_MAIN(ClipboardClientFilesBridgeTest)
#include "ClipboardClientFilesBridgeTest.moc"
