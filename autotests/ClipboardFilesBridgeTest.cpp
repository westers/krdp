// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 2: the broker's cliprdr side of host-to-client file copy (capability flags, the announcement, the descriptor, FILECONTENTS,
// locks, hostile requests, resets) against a fake CliprdrServerContext. No peer, no worker, no display.

#include <QLoggingCategory>
#include <QSignalSpy>
#include <QTest>

#include "Clipboard.h"
#include "ClipboardFileDescriptor.h"
#include "ClipboardFileLimits.h"

using namespace KRdp;

namespace
{
constexpr quint64 GiB = 1024ULL * 1024 * 1024;

struct Sent {
    QList<QList<uint32_t>> formatLists;
    QList<QStringList> formatNames;
    struct Data {
        uint16_t flags;
        QByteArray data;
    };
    QList<Data> dataResponses;
    struct Contents {
        uint32_t streamId;
        uint16_t flags;
        QByteArray data;
    };
    QList<Contents> contents;
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
    return CHANNEL_RC_OK;
}
UINT fakeDataResponse(CliprdrServerContext *, const CLIPRDR_FORMAT_DATA_RESPONSE *response)
{
    g_sent->dataResponses.append({response->common.msgFlags,
                                  response->requestedFormatData ? QByteArray(reinterpret_cast<const char *>(response->requestedFormatData), response->common.dataLen) : QByteArray()});
    return CHANNEL_RC_OK;
}
UINT fakeContentsResponse(CliprdrServerContext *, const CLIPRDR_FILE_CONTENTS_RESPONSE *response)
{
    g_sent->contents.append({response->streamId, response->common.msgFlags,
                             response->requestedData ? QByteArray(reinterpret_cast<const char *>(response->requestedData), response->cbRequested) : QByteArray()});
    return CHANNEL_RC_OK;
}

QStringList g_messages;
void capture(QtMsgType, const QMessageLogContext &, const QString &message)
{
    g_messages.append(message);
}

QByteArray descriptorOf(const QList<ClipboardFileDescriptor::EncodeItem> &items)
{
    const auto encoded = ClipboardFileDescriptor::encode(items);
    return encoded.data;
}
// 0: a 1 MiB file, 1: a directory, 2: a 5 GiB file, 3: an empty file
QList<ClipboardFileDescriptor::EncodeItem> defaultItems()
{
    return {{QStringLiteral("a.bin"), false, 1024 * 1024, std::nullopt},
            {QStringLiteral("dir"), true, 0, std::nullopt},
            {QStringLiteral("big.bin"), false, 5 * GiB, std::nullopt},
            {QStringLiteral("empty"), false, 0, std::nullopt}};
}

struct Rig {
    Sent sent;
    CliprdrServerContext context{};
    Clipboard clipboard{nullptr};
    struct Read {
        quint64 seq;
        quint32 index;
        quint64 offset;
        quint32 length;
        quint32 streamId;
    };
    QList<Read> reads;
    QList<QPair<quint64, quint32>> locks;
    QList<quint32> unlocks;
    QList<quint64> cancels;
    bool acceptReads = true;
    QSignalSpy requested{&clipboard, &Clipboard::hostFilesRequested};
    QSignalSpy ended{&clipboard, &Clipboard::hostFileTransferEnded};
    explicit Rig(bool negotiateStream = true, bool lock = true, bool huge = true)
    {
        g_sent = &sent;
        context.ServerFormatList = fakeFormatList;
        context.ServerFormatListResponse = fakeFormatListResponse;
        context.ServerFormatDataResponse = fakeDataResponse;
        context.ServerFileContentsResponse = fakeContentsResponse;
        Clipboard::applyFileCapabilities(&context, true);
        // what the capability exchange leaves behind: the intersection with the client's flags
        context.streamFileClipEnabled = negotiateStream;
        context.canLockClipData = lock;
        context.hasHugeFileSupport = huge;
        Clipboard::installClientCallbacks(&context, &clipboard);
        clipboard.attachContextForTest(&context);
        clipboard.setFilesEnabled(true);
        clipboard.setDirections(true, true);
        clipboard.setFileDirections(true, false);
        Clipboard::HostFileBackend backend;
        backend.read = [this](quint64 seq, quint32 index, quint64 offset, quint32 length, quint32 streamId) {
            if (!acceptReads) return false;
            reads.append({seq, index, offset, length, streamId});
            return true;
        };
        backend.lock = [this](quint64 seq, quint32 id) { locks.append({seq, id}); };
        backend.unlock = [this](quint32 id) { unlocks.append(id); };
        backend.cancel = [this](quint64 seq) { cancels.append(seq); };
        clipboard.setHostFileBackend(backend);
        clipboard.setGraphicsDelivered();
    }
    ~Rig() { g_sent = nullptr; }

    /// The host copies files, the client pastes: returns once the descriptor was served (seq 1 unless given).
    void offerAndServe(const QList<ClipboardFileDescriptor::EncodeItem> &items = defaultItems(), quint64 seq = 1, quint32 skipped = 0)
    {
        const int before = requested.size();
        clipboard.announceHost(false, false, false, true);
        dataRequest(ClipboardFileDescriptor::FormatId);
        QTRY_COMPARE(requested.size(), before + 1);
        clipboard.provideHostFiles(requested.last().first().toULongLong(), seq, descriptorOf(items), skipped, true);
    }
    UINT dataRequest(uint32_t format)
    {
        CLIPRDR_FORMAT_DATA_REQUEST request{};
        request.requestedFormatId = format;
        return context.ClientFormatDataRequest(&context, &request);
    }
    UINT contentsRequest(uint32_t stream, uint32_t index, uint32_t flags, uint64_t offset, uint32_t length, std::optional<uint32_t> lock = std::nullopt)
    {
        CLIPRDR_FILE_CONTENTS_REQUEST request{};
        request.common.msgType = CB_FILECONTENTS_REQUEST;
        request.streamId = stream;
        request.listIndex = index;
        request.dwFlags = flags;
        request.nPositionLow = uint32_t(offset & 0xffffffffu);
        request.nPositionHigh = uint32_t(offset >> 32);
        request.cbRequested = length;
        request.haveClipDataId = lock.has_value();
        request.clipDataId = lock.value_or(0);
        return context.ClientFileContentsRequest(&context, &request);
    }
    UINT lock(uint32_t id)
    {
        CLIPRDR_LOCK_CLIPBOARD_DATA data{};
        data.common.msgType = CB_LOCK_CLIPDATA;
        data.clipDataId = id;
        return context.ClientLockClipboardData(&context, &data);
    }
    UINT unlock(uint32_t id)
    {
        CLIPRDR_UNLOCK_CLIPBOARD_DATA data{};
        data.common.msgType = CB_UNLOCK_CLIPDATA;
        data.clipDataId = id;
        return context.ClientUnlockClipboardData(&context, &data);
    }
    const Sent::Contents &lastContents() const { return sent.contents.last(); }
};
}

class ClipboardFilesBridgeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void fileCapabilitiesAreAdvertisedOnlyWhenTheHostOwnerAllowsFiles()
    {
        CliprdrServerContext on{};
        Clipboard::applyFileCapabilities(&on, true);
        QVERIFY(on.streamFileClipEnabled && on.fileClipNoFilePaths && on.canLockClipData && on.hasHugeFileSupport);
        CliprdrServerContext off{};
        off.streamFileClipEnabled = off.fileClipNoFilePaths = off.canLockClipData = off.hasHugeFileSupport = TRUE;
        Clipboard::applyFileCapabilities(&off, false);
        QVERIFY(!off.streamFileClipEnabled && !off.fileClipNoFilePaths && !off.canLockClipData && !off.hasHugeFileSupport);
        Clipboard clipboard(nullptr);
        QVERIFY(!clipboard.filesEnabled()); // off until the host controller says so
        clipboard.setFilesEnabled(true);
        QVERIFY(clipboard.filesEnabled());
    }

    void aFileCopyIsAnnouncedAloneAsTheRegisteredFormat()
    {
        Rig rig;
        rig.clipboard.announceHost(true, true, true, true); // a file manager also puts text on the clipboard
        QCOMPARE(rig.sent.formatLists.size(), 1);
        QCOMPARE(rig.sent.formatLists.first(), QList<uint32_t>{ClipboardFileDescriptor::FormatId});
        QCOMPARE(rig.sent.formatNames.first(), QStringList{QStringLiteral("FileGroupDescriptorW")});
        // A later text copy replaces it.
        rig.clipboard.announceHost(true, false, false, false);
        QTRY_COMPARE(rig.sent.formatLists.size(), 2);
        QCOMPARE(rig.sent.formatLists.last(), QList<uint32_t>{CF_UNICODETEXT});
    }

    void filesAreNotAnnouncedWithoutTheSettingThePolicyOrNegotiation()
    {
        {
            Rig rig;
            rig.clipboard.setFilesEnabled(false);
            rig.clipboard.announceHost(false, false, false, true);
            QVERIFY(rig.sent.formatLists.isEmpty());
        }
        {
            Rig rig;
            rig.clipboard.setFileDirections(false, false);
            rig.clipboard.announceHost(false, false, false, true);
            QVERIFY(rig.sent.formatLists.isEmpty());
        }
        {
            Rig rig(false); // the client did not negotiate streaming
            rig.clipboard.announceHost(false, false, false, true);
            QVERIFY(rig.sent.formatLists.isEmpty());
        }
        {
            // an earlier text offer is withdrawn when the files that replaced it cannot be offered
            Rig rig;
            rig.clipboard.announceHost(true, false, false, false);
            QCOMPARE(rig.sent.formatLists.size(), 1);
            rig.clipboard.setFileDirections(false, false);
            rig.clipboard.announceHost(false, false, false, true);
            QTRY_COMPARE(rig.sent.formatLists.size(), 2);
            QVERIFY(rig.sent.formatLists.last().isEmpty());
        }
    }

    void thePasteAsksTheWorkerForTheDescriptorAndServesItChecked()
    {
        Rig rig(true, true, false); // no huge support
        rig.clipboard.announceHost(false, false, false, true);
        rig.dataRequest(ClipboardFileDescriptor::FormatId);
        QTRY_COMPARE(rig.requested.size(), 1);
        QCOMPARE(rig.requested.first().at(1).toBool(), false); // the worker is told: no huge files
        const auto id = rig.requested.first().first().toULongLong();
        // a descriptor with a 5 GiB file is refused when the client has no huge support (the worker should have skipped it)
        rig.clipboard.provideHostFiles(id, 1, descriptorOf(defaultItems()), 0, true);
        QCOMPARE(rig.sent.dataResponses.size(), 1);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(rig.clipboard.counters().refusedOversize, quint64(1));
        // a fitting one is served byte for byte
        rig.dataRequest(ClipboardFileDescriptor::FormatId);
        QTRY_COMPARE(rig.requested.size(), 2);
        const auto small = descriptorOf({{QStringLiteral("a.bin"), false, 1000, std::nullopt}});
        rig.clipboard.provideHostFiles(rig.requested.last().first().toULongLong(), 2, small, 0, true);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_OK));
        QCOMPARE(rig.sent.dataResponses.last().data, small);
        QCOMPARE(rig.clipboard.counters().hostFileLists, quint64(1));
    }

    void aMalformedOrRefusedDescriptorIsNeverServed()
    {
        Rig rig;
        rig.clipboard.announceHost(false, false, false, true);
        const auto ask = [&](const QByteArray &descriptor, bool ok, quint64 seq = 1) {
            const int n = rig.requested.size();
            rig.dataRequest(ClipboardFileDescriptor::FormatId);
            QTRY_COMPARE(rig.requested.size(), n + 1);
            rig.clipboard.provideHostFiles(rig.requested.last().first().toULongLong(), seq, descriptor, 0, ok);
        };
        ask({}, false); // the worker said no
        ask(QByteArray(10, 'x'), true); // not a descriptor
        ask(QByteArray(4, '\0'), true); // no entries
        ask(descriptorOf(defaultItems()).left(100), true); // truncated
        ask(descriptorOf(defaultItems()), true, 0); // no offer id
        QCOMPARE(rig.sent.dataResponses.size(), 5);
        for (const auto &response : std::as_const(rig.sent.dataResponses)) QCOMPARE(response.flags, uint16_t(CB_RESPONSE_FAIL));
        // a request for another format is refused, and so is one while files are not allowed
        rig.dataRequest(CF_UNICODETEXT);
        QTRY_COMPARE(rig.sent.dataResponses.size(), 6);
        QCOMPARE(rig.sent.dataResponses.last().flags, uint16_t(CB_RESPONSE_FAIL));
        rig.clipboard.setFileDirections(false, false);
        QVERIFY(rig.sent.formatLists.last().isEmpty()); // withdrawn
        rig.dataRequest(ClipboardFileDescriptor::FormatId);
        QTRY_COMPARE(rig.sent.dataResponses.size(), 7);
        QCOMPARE(rig.requested.size(), 5);
    }

    void sizesAndRangesAreServedFromTheWorkerWith64BitOffsets()
    {
        Rig rig;
        rig.offerAndServe();
        QCOMPARE(rig.clipboard.counters().hostFileLists, quint64(1));
        // SIZE: answered from the snapshot at once, no worker involved
        rig.contentsRequest(11, 2, FILECONTENTS_SIZE, 0, 8);
        QTRY_COMPARE(rig.sent.contents.size(), 1);
        QCOMPARE(rig.lastContents().flags, uint16_t(CB_RESPONSE_OK));
        quint64 size = 0;
        for (int i = 0; i < 8; ++i) size |= quint64(quint8(rig.lastContents().data[i])) << (8 * i);
        QCOMPARE(size, 5 * GiB);
        QVERIFY(rig.reads.isEmpty());
        // RANGE across 4 GiB
        rig.contentsRequest(12, 2, FILECONTENTS_RANGE, 4 * GiB - 100, 200);
        QTRY_COMPARE(rig.reads.size(), 1);
        QCOMPARE(rig.reads.first().seq, quint64(1));
        QCOMPARE(rig.reads.first().index, quint32(2));
        QCOMPARE(rig.reads.first().offset, 4 * GiB - 100);
        QCOMPARE(rig.reads.first().length, quint32(200));
        QCOMPARE(rig.reads.first().streamId, quint32(12));
        QCOMPARE(rig.sent.contents.size(), 1); // not answered before the worker has
        rig.clipboard.provideHostFileRead(12, Clipboard::ReadResult::Ok, QByteArray(200, 'z'));
        QTRY_COMPARE(rig.sent.contents.size(), 2);
        QCOMPARE(rig.lastContents().streamId, quint32(12));
        QCOMPARE(rig.lastContents().flags, uint16_t(CB_RESPONSE_OK));
        QCOMPARE(rig.lastContents().data, QByteArray(200, 'z'));
        QCOMPARE(rig.clipboard.counters().hostFileRanges, quint64(1));
        QCOMPARE(rig.clipboard.counters().hostFileBytes, quint64(200));
        QVERIFY(rig.clipboard.hostFileTransferActive() == false);
    }

    void theWindowAndThePacerBoundTheReadsInFlight()
    {
        Rig rig;
        rig.offerAndServe();
        // Auto starts at the floor x4 (128 kB/s): one request in flight at a time. The rest wait for the response of the first.
        for (quint32 i = 0; i < 4; ++i) rig.contentsRequest(20 + i, 2, FILECONTENTS_RANGE, quint64(i) * 262144, 262144);
        QTRY_COMPARE(rig.reads.size(), 1);
        QVERIFY(rig.clipboard.hostFileTransferActive());
        rig.clipboard.provideHostFileRead(20, Clipboard::ReadResult::Ok, QByteArray(262144, 'a'));
        QTRY_COMPARE(rig.sent.contents.size(), 1);
        QTRY_COMPARE(rig.reads.size(), 2); // the next read goes out as the first response leaves
        QCOMPARE(rig.reads.last().streamId, quint32(21));
        // The overdrawn bucket now holds the second response back for seconds (256 KiB at 128 kB/s).
        rig.clipboard.provideHostFileRead(21, Clipboard::ReadResult::Ok, QByteArray(262144, 'b'));
        QTest::qWait(100);
        QCOMPARE(rig.sent.contents.size(), 1);
        QVERIFY(rig.clipboard.hostFileTransferActive());
        // A fixed cap above that rate lets it go sooner; the setting reaches the running transfer.
        Clipboard::FileRate fast;
        fast.mode = ClipboardFilePacing::RequestedMode::Unlimited;
        rig.clipboard.setFileTransferRequest(fast);
        QCOMPARE(rig.clipboard.effectiveFilePolicy().unlimited, true);
        rig.clipboard.setFileCeiling(ClipboardFilePacing::Ceiling::rate(5'000'000));
        QCOMPARE(rig.clipboard.effectiveFilePolicy().unlimited, false); // a ceiling beats unlimited
        QCOMPARE(rig.clipboard.effectiveFilePolicy().capBytesPerSecond, std::optional<quint64>(5'000'000));
        QTRY_VERIFY_WITH_TIMEOUT(rig.sent.contents.size() >= 2, 5000);
    }

    void hostileRequestsAreAnsweredFailedAndNeverReachTheWorker()
    {
        Rig rig;
        rig.offerAndServe();
        rig.contentsRequest(30, 9, FILECONTENTS_RANGE, 0, 10); // no such entry
        rig.contentsRequest(31, 1, FILECONTENTS_RANGE, 0, 10); // a directory
        QTRY_COMPARE(rig.sent.contents.size(), 2);
        for (const auto &response : std::as_const(rig.sent.contents)) QCOMPARE(response.flags, uint16_t(CB_RESPONSE_FAIL));
        QVERIFY(rig.reads.isEmpty());
        rig.contentsRequest(32, 0, FILECONTENTS_RANGE, 2 * 1024 * 1024, 10); // past the end: the third violation aborts the transfer
        QTRY_COMPARE(rig.sent.contents.size(), 3);
        QVERIFY(rig.cancels.contains(1));
        QCOMPARE(rig.ended.size(), 1);
        rig.contentsRequest(33, 0, FILECONTENTS_RANGE, 0, 10); // fine in itself, but the transfer is over
        QTRY_COMPARE(rig.sent.contents.size(), 4);
        QCOMPARE(rig.lastContents().flags, uint16_t(CB_RESPONSE_FAIL));
        QVERIFY(rig.reads.isEmpty());
        // a new copy starts a new transfer
        rig.offerAndServe(defaultItems(), 2);
        rig.contentsRequest(34, 0, FILECONTENTS_RANGE, 0, 10);
        QTRY_COMPARE(rig.reads.size(), 1);
        QCOMPARE(rig.reads.first().seq, quint64(2));
    }

    void aChangedFileFailsTheStreamAndEndsTheTransfer()
    {
        Rig rig;
        rig.offerAndServe();
        rig.contentsRequest(40, 0, FILECONTENTS_RANGE, 0, 1000);
        QTRY_COMPARE(rig.reads.size(), 1);
        rig.clipboard.provideHostFileRead(40, Clipboard::ReadResult::Changed, {});
        QTRY_COMPARE(rig.sent.contents.size(), 1);
        QCOMPARE(rig.lastContents().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(rig.ended.size(), 1);
        QVERIFY(rig.cancels.contains(1));
        // not retried: a request for the same transfer is failed without a read
        rig.contentsRequest(41, 0, FILECONTENTS_RANGE, 0, 1000);
        QTRY_COMPARE(rig.sent.contents.size(), 2);
        QCOMPARE(rig.reads.size(), 1);
        // a read the worker could not even be asked for ends it as well
        rig.offerAndServe(defaultItems(), 2);
        rig.acceptReads = false;
        rig.contentsRequest(42, 0, FILECONTENTS_RANGE, 0, 1000);
        QTRY_COMPARE(rig.sent.contents.size(), 3);
        QCOMPARE(rig.lastContents().flags, uint16_t(CB_RESPONSE_FAIL));
        QCOMPARE(rig.ended.size(), 2);
    }

    void locksPinTheOfferAndAreReleasedWithTheClient()
    {
        Rig rig;
        rig.lock(77); // a client may lock right after the format list, before it has the descriptor
        QTest::qWait(30);
        QVERIFY(rig.locks.isEmpty()); // nothing is announced yet
        rig.offerAndServe();
        rig.lock(77);
        QTRY_COMPARE(rig.locks.size(), 1);
        QCOMPARE(rig.locks.first(), (QPair<quint64, quint32>(1, 77)));
        // a request carrying the id is served from the locked offer, even after a newer copy
        rig.offerAndServe(defaultItems(), 2);
        rig.contentsRequest(50, 0, FILECONTENTS_RANGE, 0, 100, 77);
        QTRY_COMPARE(rig.reads.size(), 1);
        QCOMPARE(rig.reads.first().seq, quint64(1));
        rig.contentsRequest(51, 0, FILECONTENTS_RANGE, 0, 100); // no id: the current offer
        rig.clipboard.provideHostFileRead(50, Clipboard::ReadResult::Ok, QByteArray(100, 'q'));
        QTRY_COMPARE(rig.reads.size(), 2);
        QCOMPARE(rig.reads.last().seq, quint64(2));
        rig.unlock(77);
        QTRY_COMPARE(rig.unlocks, (QList<quint32>{77}));
        rig.contentsRequest(52, 0, FILECONTENTS_RANGE, 0, 100, 77); // the lock is gone
        QTRY_VERIFY(rig.sent.contents.size() >= 2);
        QCOMPARE(rig.lastContents().flags, uint16_t(CB_RESPONSE_FAIL));
        // a lock the client asks for without negotiating it is ignored
        Rig noLock(true, false);
        noLock.offerAndServe();
        noLock.lock(5);
        QTest::qWait(30);
        QVERIFY(noLock.locks.isEmpty());
    }

    void turningFilesOffEndsTheTransferWithdrawsTheOfferAndFailsEverythingPending()
    {
        Rig rig;
        rig.offerAndServe();
        for (quint32 i = 0; i < 3; ++i) rig.contentsRequest(60 + i, 0, FILECONTENTS_RANGE, quint64(i) * 1000, 1000);
        QTRY_COMPARE(rig.reads.size(), 1);
        rig.cancels.clear();
        rig.clipboard.setFileDirections(false, false);
        QCOMPARE(rig.sent.contents.size(), 3); // every pending request is answered FAIL
        for (const auto &response : std::as_const(rig.sent.contents)) QCOMPARE(response.flags, uint16_t(CB_RESPONSE_FAIL));
        QVERIFY(rig.cancels.contains(0));
        QVERIFY(rig.sent.formatLists.last().isEmpty()); // withdrawn
        QVERIFY(!rig.clipboard.hostFileTransferActive());
        // late worker answers are dropped
        rig.clipboard.provideHostFileRead(60, Clipboard::ReadResult::Ok, QByteArray(1000, 'x'));
        QTest::qWait(30);
        QCOMPARE(rig.sent.contents.size(), 3);
        // and any further request is refused outright
        rig.contentsRequest(70, 0, FILECONTENTS_RANGE, 0, 10);
        QTRY_COMPARE(rig.sent.contents.size(), 4);
        QCOMPARE(rig.lastContents().flags, uint16_t(CB_RESPONSE_FAIL));
        // the whole host side being reset (handover) does the same
        Rig second;
        second.offerAndServe();
        second.contentsRequest(80, 0, FILECONTENTS_RANGE, 0, 100);
        QTRY_COMPARE(second.reads.size(), 1);
        second.clipboard.resetHostSide();
        QCOMPARE(second.sent.contents.size(), 1);
        QCOMPARE(second.lastContents().flags, uint16_t(CB_RESPONSE_FAIL));
    }

    void requestsAreRefusedWhenNothingIsOfferedOrNegotiated()
    {
        Rig rig;
        rig.contentsRequest(90, 0, FILECONTENTS_RANGE, 0, 10); // nothing announced
        QTRY_COMPARE(rig.sent.contents.size(), 1);
        QCOMPARE(rig.lastContents().flags, uint16_t(CB_RESPONSE_FAIL));
        Rig unnegotiated(false);
        unnegotiated.contentsRequest(91, 0, FILECONTENTS_SIZE, 0, 8);
        QTRY_COMPARE(unnegotiated.sent.contents.size(), 1);
        QCOMPARE(unnegotiated.lastContents().flags, uint16_t(CB_RESPONSE_FAIL));
        QVERIFY(unnegotiated.reads.isEmpty());
        // a viewer (host to client denied) is refused whatever it sends
        Rig viewer;
        viewer.offerAndServe();
        viewer.clipboard.setDirections(false, false);
        viewer.contentsRequest(92, 0, FILECONTENTS_SIZE, 0, 8);
        QTRY_COMPARE(viewer.sent.contents.size(), 1);
        QCOMPARE(viewer.lastContents().flags, uint16_t(CB_RESPONSE_FAIL));
    }

    void aSlowOrAbsentWorkerAnswerTimesOutTheDescriptorRequest()
    {
        Rig rig;
        rig.clipboard.announceHost(false, false, false, true);
        rig.dataRequest(ClipboardFileDescriptor::FormatId);
        QTRY_COMPARE(rig.requested.size(), 1);
        QCOMPARE(rig.sent.dataResponses.size(), 0);
        // Files get the long timeout (the worker's walk has its own 5 s budget); the short text timeout does not fire for them.
        QTest::qWait(Clipboard::HostTextTimeoutMs + 300);
        QCOMPARE(rig.sent.dataResponses.size(), 0);
        QVERIFY(Clipboard::HostFilesTimeoutMs > Clipboard::HostTextTimeoutMs);
    }

    void namesAndContentsNeverReachALog()
    {
        g_messages.clear();
        const auto previous = qInstallMessageHandler(capture);
        QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true"));
        {
            Rig rig;
            rig.offerAndServe({{QStringLiteral("CANARY-name.txt"), false, 100, std::nullopt}}, 1, 2);
            rig.contentsRequest(1, 0, FILECONTENTS_RANGE, 0, 100);
            QTRY_COMPARE(rig.reads.size(), 1);
            rig.clipboard.provideHostFileRead(1, Clipboard::ReadResult::Changed, {});
            QTRY_COMPARE(rig.sent.contents.size(), 1);
        }
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(previous);
        for (const auto &message : std::as_const(g_messages)) QVERIFY2(!message.contains(QStringLiteral("CANARY")), qPrintable(message));
    }
};

QTEST_GUILESS_MAIN(ClipboardFilesBridgeTest)
#include "ClipboardFilesBridgeTest.moc"
