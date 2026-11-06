// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078: the relay between a connection's Clipboard and the worker's endpoint for host-to-client files: reads are issued only while the
// connection may transfer, answers are routed by read id, and the backend's calls stay harmless when the relay is gone before the connection
// (a Clipboard cancels its transfers from its destructor, which can run after the broker that owns the relay).

#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "HostFileRelay.h"

using namespace KRdp;
using namespace KRdp::ConsoleWorkerWire;

namespace
{
struct FakeWorker {
    QTemporaryDir directory;
    ConsoleWorkerEndpoint endpoint;
    ConsoleHandoff::Target target{ConsoleSeat::Adapter::PhysicalUser, QStringLiteral("3"), quint32(getuid())};
    QByteArray token = QByteArray(24, 'h');
    QLocalSocket worker;
    Deframer reader;
    bool ok = false;
    FakeWorker()
    {
        if (!endpoint.listen(directory.filePath(QStringLiteral("hostrelay.sock")), target, token)) return;
        worker.connectToServer(endpoint.socketName());
        if (!worker.waitForConnected(1000)) return;
        worker.write(frame(Hello{target.sessionId, target.uid, token}));
        worker.write(frame(Kind::Ready));
        worker.flush();
        QTRY_VERIFY_WITH_TIMEOUT(endpoint.ready(), 3000);
        endpoint.setClipboardPolicy(1, true, true, false, false, true, false);
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
};
}

class HostFileRelayTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readsAreIssuedOnlyWhileAllowedAndAnswersAreRoutedOnce()
    {
        FakeWorker w;
        QVERIFY(w.ok);
        w.drain();
        HostFileRelay relay;
        bool allowed = true;
        Clipboard clipboard(nullptr);
        const auto backend = relay.backendFor(&clipboard, [&w] { return &w.endpoint; }, [&allowed] { return allowed; });
        QVERIFY(backend.read(7, 0, 0, 100, 11));
        QCOMPARE(relay.pending(), 1);
        auto records = w.drain();
        QCOMPARE(records.size(), 1);
        const auto read = fileRead(records.first());
        QVERIFY(read);
        QCOMPARE(read->seq, quint64(7));
        allowed = false; // control moved or the policy changed: no new read, and the connection cannot end the worker's reads
        QVERIFY(!backend.read(7, 0, 100, 100, 12));
        backend.cancel(7);
        QVERIFY(w.drain().isEmpty());
        QCOMPARE(relay.pending(), 0); // (its own pending reads are forgotten either way)
        allowed = true;
        QVERIFY(backend.read(8, 0, 0, 100, 13));
        w.drain();
        relay.finished(FileReadResult{read->readId + 100, FileReadStatus::Ok, QByteArray(100, 'x')}); // an answer nobody asked for
        QCOMPARE(relay.pending(), 1);
        relay.clear();
        QCOMPARE(relay.pending(), 0);
    }

    void aRelayGoneBeforeTheConnectionLeavesTheBackendHarmless()
    {
        FakeWorker w;
        QVERIFY(w.ok);
        w.drain();
        Clipboard clipboard(nullptr);
        Clipboard::HostFileBackend backend;
        {
            auto relay = std::make_unique<HostFileRelay>();
            backend = relay->backendFor(&clipboard, [&w] { return &w.endpoint; }, [] { return true; });
            QVERIFY(backend.read(7, 0, 0, 100, 11));
            w.drain();
        }
        // the Clipboard's destructor and a late stream still call the backend after the broker (and the relay it owns) are gone
        backend.cancel(0);
        backend.cancel(7);
        QVERIFY(!backend.read(7, 0, 100, 100, 12));
        backend.lock(7, 3);
        backend.unlock(3);
        const auto records = w.drain();
        for (const auto &record : records) {
            QVERIFY2(record.kind != Kind::FileRead && record.kind != Kind::FileCancel, "a gone relay must not send reads or cancels");
        }
    }
};

QTEST_GUILESS_MAIN(HostFileRelayTest)
#include "HostFileRelayTest.moc"
