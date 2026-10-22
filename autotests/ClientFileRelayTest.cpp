// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 3: the relay between a connection's Clipboard and the worker's endpoint for client-to-host files: ownership of transfers,
// authorization, routing of the worker's replies, and that a connection ending cancels only ITS transfers.

#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "ClientFileRelay.h"

using namespace KRdp;
using namespace KRdp::ConsoleWorkerWire;
using namespace Qt::StringLiterals;

namespace
{
struct FakeWorker {
    QTemporaryDir directory;
    ConsoleWorkerEndpoint endpoint;
    ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), quint32(getuid())};
    QByteArray token = QByteArray(24, 'r');
    QLocalSocket worker;
    Deframer reader;
    bool ok = false;
    FakeWorker()
    {
        if (!endpoint.listen(directory.filePath(QStringLiteral("relay.sock")), target, token)) return;
        worker.connectToServer(endpoint.socketName());
        if (!worker.waitForConnected(1000)) return;
        worker.write(frame(Hello{target.sessionId, target.uid, token}));
        worker.write(frame(Kind::Ready));
        worker.flush();
        QTRY_VERIFY_WITH_TIMEOUT(endpoint.ready(), 3000);
        endpoint.setClipboardPolicy(1, true, true, false, false, false, true);
        ok = true;
    }
    QList<Record> drain()
    {
        QTest::qWait(30);
        reader.feed(worker.readAll());
        QList<Record> records;
        while (const auto record = reader.next()) records.append(*record);
        return records;
    }
    void send(const QByteArray &record)
    {
        worker.write(record);
        worker.flush();
    }
};
QByteArray descriptor()
{
    return ClipboardFileDescriptor::encode({{u"a.txt"_s, false, 10, std::nullopt}}).data;
}
}

class ClientFileRelayTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void offersDataAndRepliesAreRoutedToTheOwner()
    {
        FakeWorker w;
        QVERIFY(w.ok);
        w.drain();
        ClientFileRelay relay;
        bool allowed = true;
        Clipboard owner(nullptr);
        QPointer<Clipboard> pointer(&owner);
        const auto backend = relay.backendFor(pointer, [&w] { return &w.endpoint; }, [&allowed] { return allowed; }, [] { return quint64(5) << 30; });
        const quint64 seq = backend.offer(descriptor(), true);
        QVERIFY(seq);
        QCOMPARE(relay.owned(), 1);
        auto records = w.drain();
        QCOMPARE(records.size(), 1);
        const auto offer = filePushOffer(records.first());
        QVERIFY(offer);
        QCOMPARE(offer->reserveBytes, quint64(5) << 30); // the host owner's reserve travels with the offer
        QVERIFY(offer->hugeFiles);
        // data is refused until the worker accepted (the endpoint's rule), and then needs the owner and the authorization
        QVERIFY(!backend.data(seq, 0, 0, QByteArray(10, 'x')));
        w.send(frame(FilePushReply{seq, true, FilePushReason::None, 1, 0, 0, 10, 0, 0}));
        QTest::qWait(60);
        QVERIFY(backend.data(seq, 0, 0, QByteArray(10, 'x')));
        allowed = false;
        QVERIFY(!backend.data(seq, 0, 0, QByteArray(10, 'x'))); // the planner withdrew files: no more bytes
        QCOMPARE(backend.offer(descriptor(), true), quint64(0)); // and no new offer
        allowed = true;
    }

    // A viewer's connection ending must not cancel the controller's transfer.
    void aConnectionCancelsOnlyItsOwnTransfers()
    {
        FakeWorker w;
        QVERIFY(w.ok);
        w.drain();
        ClientFileRelay relay;
        Clipboard controller(nullptr), viewer(nullptr);
        const auto controllerBackend = relay.backendFor(&controller, [&w] { return &w.endpoint; }, [] { return true; }, [] { return quint64(0); });
        const auto viewerBackend = relay.backendFor(&viewer, [&w] { return &w.endpoint; }, [] { return true; }, [] { return quint64(0); });
        const quint64 first = controllerBackend.offer(descriptor(), true);
        const quint64 second = viewerBackend.offer(descriptor(), true);
        QVERIFY(first && second && first != second);
        w.drain();
        viewerBackend.cancel(0, int(Clipboard::ClientFilesCancel::ConnectionEnded));
        auto records = w.drain();
        QCOMPARE(records.size(), 1);
        QCOMPARE(filePushCancel(records.first()), (std::optional<FilePushCancel>(FilePushCancel{second, FilePushCancelReason::ConnectionEnded})));
        QCOMPARE(relay.owned(), 1);
        // the viewer cannot cancel or feed the controller's transfer
        viewerBackend.cancel(first, int(Clipboard::ClientFilesCancel::Failed));
        QVERIFY(w.drain().isEmpty());
        w.send(frame(FilePushReply{first, true, FilePushReason::None, 1, 0, 0, 10, 0, 0}));
        QTest::qWait(60);
        QVERIFY(!viewerBackend.data(first, 0, 0, QByteArray(10, 'x')));
        QVERIFY(controllerBackend.data(first, 0, 0, QByteArray(10, 'x')));
        // the controller's own connection ending cancels its transfer
        controllerBackend.cancel(0, int(Clipboard::ClientFilesCancel::ConnectionEnded));
        records = w.drain();
        QCOMPARE(records.last().kind, Kind::FilePushCancel);
        QCOMPARE(filePushCancel(records.last())->seq, first);
        QCOMPARE(relay.owned(), 0);
    }

    void refusedAndFailedTransfersAreForgottenAndStrayAnswersDropped()
    {
        FakeWorker w;
        QVERIFY(w.ok);
        w.drain();
        ClientFileRelay relay;
        Clipboard owner(nullptr);
        const auto backend = relay.backendFor(&owner, [&w] { return &w.endpoint; }, [] { return true; }, [] { return quint64(0); });
        const quint64 refused = backend.offer(descriptor(), false);
        w.drain();
        // routed answers for an owner that is not attached to a context: only the bookkeeping is checked here
        relay.replied(FilePushReply{refused, false, FilePushReason::NoSpace, 0, 0, 0, 0, 3, 7});
        QCOMPARE(relay.owned(), 0);
        relay.replied(FilePushReply{999, true, FilePushReason::None, 1, 0, 0, 1, 0, 0}); // nobody's
        relay.acked(FilePushAck{999, 0, 0, 1, ClipboardFileFetch::AckStatus::Ok});
        relay.stateChanged(FilePushState{999, FilePushPhase::Failed, FilePushReason::Cancelled, 0, 1});
        const quint64 failed = backend.offer(descriptor(), false);
        QCOMPARE(relay.owned(), 1);
        relay.stateChanged(FilePushState{failed, FilePushPhase::Failed, FilePushReason::WriteFailed, 0, 10});
        QCOMPARE(relay.owned(), 0);
        // a finished copy stays owned until its connection cancels it (the staging still exists)
        const quint64 done = backend.offer(descriptor(), false);
        relay.stateChanged(FilePushState{done, FilePushPhase::Done, FilePushReason::None, 10, 10});
        QCOMPARE(relay.owned(), 1);
        backend.cancel(done, int(Clipboard::ClientFilesCancel::Replaced));
        QCOMPARE(relay.owned(), 0);
    }

    void aWorkerGoneMeansNothingIsSentAndNothingBreaks()
    {
        ClientFileRelay relay;
        Clipboard owner(nullptr);
        const auto backend = relay.backendFor(&owner, [] { return static_cast<ConsoleWorkerEndpoint *>(nullptr); }, [] { return true; }, [] { return quint64(0); });
        QCOMPARE(backend.offer(descriptor(), true), quint64(0));
        QVERIFY(!backend.data(1, 0, 0, QByteArray(1, 'x')));
        backend.cancel(0, 0);
        backend.cancel(5, 0);
        QCOMPARE(relay.owned(), 0);
    }
};

QTEST_GUILESS_MAIN(ClientFileRelayTest)
#include "ClientFileRelayTest.moc"
