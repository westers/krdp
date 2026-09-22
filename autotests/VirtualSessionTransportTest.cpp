// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <Server.h>
#include <VideoStream.h>
#include <QLocalSocket>
#include <QTemporaryDir>
#include "VirtualSessionTransport.h"
#include "ExternalAudioQueue.h"
using namespace KRdp;
using namespace Qt::StringLiterals;

namespace KRdp {
class VirtualSessionTransportTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void closedRejectsReentrantReplacementAttachment()
    {
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            return VirtualSessionSupervisor::Launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        });
        VirtualSessionControl control(supervisor, {});
        const auto first = supervisor.create(1000), second = supervisor.create(1000); QVERIFY(first); QVERIFY(second);
        bool readyFirst = false, readySecond = false;
        QTRY_VERIFY(readyFirst || (readyFirst = supervisor.captureReady(*first)));
        QTRY_VERIFY(readySecond || (readySecond = supervisor.captureReady(*second)));
        const auto attach = [](const QString &id, const QString &session) {
            return QJsonObject{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, id},
                {u"action"_s, u"attach"_s}, {u"session"_s, session}};
        };
        QVERIFY(control.request(1000, 1, attach(u"first"_s, first->id)).value(u"ok"_s).toBool());
        Server server; RdpConnection connection(&server, -1); quint64 sequence = 0;
        VirtualSessionTransport transport(1, &connection, control, {}, sequence);
        connection.videoStream()->setEnabled(true);
        int attempts = 0;
        QObject::connect(connection.videoStream(), &VideoStream::enabledChanged, &connection, [&] {
            if (connection.videoStream()->enabled()) return;
            ++attempts;
            QVERIFY(control.dispatchActive());
            QVERIFY(!control.attachment(1)); // Original lookup removed before revocation.
            control.disconnected(1);
            QVERIFY(!control.request(1000, 1, attach(u"nested"_s, second->id)).value(u"ok"_s).toBool());
        });
        transport.closed();
        QCOMPARE(attempts, 1); QVERIFY(!control.dispatchActive()); QVERIFY(!control.attachment(1));
        for (const auto &row : supervisor.list(1000)) QCOMPARE(row.phase, VirtualSessionState::Phase::Retained);
        // The replacement remains available for a later, nonnested command.
        QVERIFY(control.request(1000, 1, attach(u"later"_s, second->id)).value(u"ok"_s).toBool());
        QVERIFY(transport.attachmentMatches(*second));
    }
    void bindingRejectsCallbackAttachmentChanges_data()
    {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<bool>("reassign");
        for (const auto *stage : {"resolver", "revoke", "stream-active", "enabled"}) {
            QTest::newRow(qPrintable(QString::fromLatin1(stage) + u"-detach"_s)) << QString::fromLatin1(stage) << false;
            QTest::newRow(qPrintable(QString::fromLatin1(stage) + u"-reassign"_s)) << QString::fromLatin1(stage) << true;
        }
    }
    void bindingRejectsCallbackAttachmentChanges()
    {
        QFETCH(QString, stage); QFETCH(bool, reassign);
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            return VirtualSessionSupervisor::Launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        });
        QPointer<VirtualSessionTransport> transport;
        VirtualSessionControl control(supervisor, [&](quint64, const auto &) { if (transport) transport->revoke(); });
        const auto first = supervisor.create(1000), second = supervisor.create(1000); QVERIFY(first); QVERIFY(second);
        bool readyFirst = false, readySecond = false;
        QTRY_VERIFY(readyFirst || (readyFirst = supervisor.captureReady(*first)));
        QTRY_VERIFY(readySecond || (readySecond = supervisor.captureReady(*second)));
        const auto attach = [](const QString &id, const QString &session) {
            return QJsonObject{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, id},
                {u"action"_s, u"attach"_s}, {u"session"_s, session}};
        };
        QVERIFY(control.request(1000, 1, attach(u"first"_s, first->id)).value(u"ok"_s).toBool());
        QTemporaryDir dir; ConsoleWorkerEndpoint endpoint; QLocalSocket worker;
        const QByteArray token(32, 't');
        QVERIFY(endpoint.listen(dir.filePath(u"worker.sock"_s), {ConsoleSeat::Adapter::VirtualUser, first->id, 1000}, token));
        worker.connectToServer(endpoint.socketName()); QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{first->id, 1000, token}));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000)); QTRY_VERIFY(endpoint.ready());
        int changes = 0;
        const auto changeAttachment = [&] {
            ++changes;
            control.disconnected(1);
            if (reassign) QVERIFY(control.request(1000, 1, attach(u"second"_s, second->id)).value(u"ok"_s).toBool());
        };
        Server server; RdpConnection connection(&server, -1); quint64 sequence = 0;
        transport = new VirtualSessionTransport(1, &connection, control, [&](const auto &) -> ConsoleWorkerEndpoint * {
            changeAttachment(); return &endpoint;
        }, sequence);
        QVERIFY(transport->attachmentMatches(*first));
        auto wrong = *first; ++wrong.generation; QVERIFY(!transport->attachmentMatches(wrong));
        wrong = *first; wrong.manager = QUuid::createUuid(); QVERIFY(!transport->attachmentMatches(wrong));
        if (stage == u"resolver"_s) {
            QVERIFY(!transport->bind());
            QCOMPARE(sequence, quint64(0)); // Never reached the worker grant.
        } else {
            if (stage == u"revoke"_s) {
                connection.videoStream()->setEnabled(true);
                QObject::connect(connection.videoStream(), &VideoStream::enabledChanged, &connection,
                    [&] { if (!connection.videoStream()->enabled()) changeAttachment(); });
            } else if (stage == u"stream-active"_s) {
                QObject::connect(&transport->m_session, &AbstractSession::streamActiveChanged, &connection,
                    [&](bool active) { if (active) changeAttachment(); });
            } else {
                QObject::connect(connection.videoStream(), &VideoStream::enabledChanged, &connection,
                    [&] { if (connection.videoStream()->enabled()) changeAttachment(); });
            }
            // Exercise the post-validation callback continuation directly. The
            // RDP connection remains unauthenticated; no successful bind is faked.
            QVERIFY(!transport->activateBinding(*first, &endpoint));
            QCOMPARE(sequence, stage == u"revoke"_s ? quint64(0) : quint64(2)); // No grant, or grant then revoke.
        }
        QCOMPARE(changes, 1); QVERIFY(transport); QVERIFY(!transport->m_handle); QVERIFY(!transport->m_endpoint);
        QVERIFY(!connection.videoStream()->enabled());
        if (reassign) {
            QVERIFY(transport->attachmentMatches(*second));
            QVERIFY(!transport->attachmentMatches(*first));
        } else QVERIFY(!control.attachment(1));
        for (const auto &row : supervisor.list(1000)) {
            if (row.id == first->id) QCOMPARE(row.phase, VirtualSessionState::Phase::Retained);
        }
        delete transport.data();
    }
    void revokeSignalMayDestroyTransport()
    {
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            return VirtualSessionSupervisor::Launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        });
        VirtualSessionControl control(supervisor, {});
        const auto handle = supervisor.create(1000); QVERIFY(handle);
        bool ready = false;
        QTRY_VERIFY(ready || (ready = supervisor.captureReady(*handle)));
        const QJsonObject attach{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"attach"_s},
            {u"action"_s, u"attach"_s}, {u"session"_s, handle->id}};
        QVERIFY(control.request(1000, 1, attach).value(u"ok"_s).toBool());
        QVERIFY(control.attachment(1));
        QCOMPARE(supervisor.list(1000).first().phase, VirtualSessionState::Phase::Attached);
        Server server; RdpConnection connection(&server, -1); quint64 sequence = 0;
        QPointer<VirtualSessionTransport> transport = new VirtualSessionTransport(1, &connection, control, {}, sequence);
        connection.videoStream()->setEnabled(true);
        QObject::connect(connection.videoStream(), &VideoStream::enabledChanged, &connection, [&] {
            delete transport.data();
        });
        transport->unavailable();
        QVERIFY(transport.isNull());
        QCOMPARE(sequence, quint64(0));
        QVERIFY(!control.attachment(1));
        QCOMPARE(supervisor.list(1000).first().phase, VirtualSessionState::Phase::Retained);
    }
    void controlDestructionDuringRequest()
    {
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> { return {}; });
        auto control = std::make_unique<VirtualSessionControl>(supervisor, VirtualSessionControl::Release{});
        Server server; RdpConnection connection(&server, -1); quint64 sequence = 0;
        VirtualSessionTransport transport(1, &connection, *control, {}, sequence);
        int calls = 0;
        control->setCreateHandler([&](quint32) -> std::optional<VirtualSessionRegistry::Handle> {
            ++calls; control.reset(); return {};
        });
        const QJsonObject command{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"delete"_s}, {u"action"_s, u"create"_s}};
        QVERIFY(transport.request(command, 1000).isEmpty()); QCOMPARE(calls, 1);
        QVERIFY(transport.request(command, 1000).isEmpty());
        transport.closed(); // Surviving transport must not dereference dead Control.
    }
    void destructionDuringBindOrDisconnect_data()
    {
        QTest::addColumn<bool>("duringBind");
        QTest::newRow("resolver-deletes-transport") << true;
        QTest::newRow("unavailable-release-deletes-transport") << false;
    }
    void destructionDuringBindOrDisconnect()
    {
        QFETCH(bool, duringBind);
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            return VirtualSessionSupervisor::Launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        });
        QPointer<VirtualSessionTransport> transport;
        int releases = 0, resolves = 0;
        VirtualSessionControl control(supervisor, [&](quint64, const auto &) {
            ++releases;
            if (!duringBind) delete transport.data();
        });
        const auto handle = supervisor.create(1000); QVERIFY(handle);
        bool ready = false;
        QTRY_VERIFY(ready || (ready = supervisor.captureReady(*handle)));
        Server server; RdpConnection connection(&server, -1); quint64 sequence = 0;
        transport = new VirtualSessionTransport(1, &connection, control, [&](const auto &) -> ConsoleWorkerEndpoint * {
            ++resolves; delete transport.data(); return nullptr;
        }, sequence);
        const QJsonObject attach{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"attach"_s},
            {u"action"_s, u"attach"_s}, {u"session"_s, handle->id}};
        if (duringBind) QVERIFY(transport->request(attach, 1000).isEmpty());
        else {
            QVERIFY(control.request(1000, 1, attach).value(u"ok"_s).toBool());
            transport->unavailable();
        }
        QVERIFY(transport.isNull()); QCOMPARE(releases, 1);
        QCOMPARE(resolves, duringBind ? 1 : 0); QCOMPARE(sequence, quint64(0));
        QVERIFY(!control.attachment(1));
    }
    void destroyedDuringControlRequest_data()
    {
        QTest::addColumn<bool>("delivery");
        QTest::addColumn<bool>("deleteConnection");
        QTest::newRow("request-deletes-transport") << false << false;
        QTest::newRow("delivery-deletes-transport") << true << false;
        QTest::newRow("request-deletes-connection") << false << true;
        QTest::newRow("delivery-deletes-connection") << true << true;
    }
    void destroyedDuringControlRequest()
    {
        QFETCH(bool, delivery); QFETCH(bool, deleteConnection);
        QTest::failOnWarning(QRegularExpression(u"KRDPCTL: dropping.*"_s)); // No reply after destruction.
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> { return {}; });
        VirtualSessionControl control(supervisor, {});
        Server server;
        QPointer<RdpConnection> connection = new RdpConnection(&server, -1);
        quint64 sequence = 0;
        int resolved = 0, called = 0;
        QPointer<VirtualSessionTransport> transport = new VirtualSessionTransport(1, connection, control,
            [&](const auto &) -> ConsoleWorkerEndpoint * { ++resolved; return nullptr; }, sequence);
        control.setCreateHandler([&](quint32 uid) -> std::optional<VirtualSessionRegistry::Handle> {
            ++called;
            if (uid != 1000) return {};
            if (deleteConnection) delete connection.data();
            else delete transport.data();
            return {};
        });
        const QJsonObject command{{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"delete"_s}, {u"action"_s, u"create"_s}};
        // Exercise the exact production dispatch/delivery path, supplying the
        // authenticated identity at its private seam instead of opening PAM/RDP.
        if (delivery) transport->deliverControlRecord(command, 1000);
        else QVERIFY(transport->request(command, 1000).isEmpty());
        QCOMPARE(called, 1); QCOMPARE(resolved, 0); QCOMPARE(sequence, quint64(0));
        if (deleteConnection) QVERIFY(connection.isNull());
        else QVERIFY(transport.isNull());
        delete transport.data(); delete connection.data();
    }
    void externalPlaybackRevocationDropsBufferedAudio()
    {
        ExternalAudioQueue queue;
        int sent = 0;
        const auto sink = [&](const QByteArray &pcm) { sent += pcm.size(); };
        queue.submit(QByteArray(3528, 'a'));
        QVERIFY(!queue.deliver(sink));
        queue.setEnabled(true);
        queue.submit(QByteArray(7056, 'b'));
        QVERIFY(queue.deliver(sink));
        QCOMPARE(sent, 3528);
        queue.setEnabled(false);
        queue.submit(QByteArray(3528, 'c'));
        QVERIFY(!queue.deliver(sink));
        queue.setEnabled(true);
        QVERIFY(!queue.deliver(sink)); // old queued bytes cannot survive off/on
        queue.submit(QByteArray(3, 'd'));
        QVERIFY(!queue.deliver(sink));
        queue.submit(QByteArray(4, 'e'));
        QVERIFY(queue.deliver(sink));
        QCOMPARE(sent, 3532);
    }
    void refusedUpdateClearsPreviousPlayback()
    {
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> { return {}; });
        VirtualSessionControl control(supervisor, {});
        Server server;
        RdpConnection connection(&server, -1);
        quint64 sequence = 0;
        VirtualSessionTransport transport(1, &connection, control, {}, sequence);
        // Simulate an earlier consent followed by loss of authorization. No
        // real RDP socket, PipeWire graph or desktop is used by this fixture.
        transport.m_playback = true;
        const auto result = transport.request({{u"type"_s, u"media"_s}, {u"v"_s, 1},
            {u"playback"_s, false}, {u"microphone"_s, true}, {u"camera"_s, false}});
        QVERIFY(!result.value(u"ok"_s).toBool());
        QVERIFY(!result.value(u"playback"_s).toBool());
        QVERIFY(!transport.m_playback);
        QCOMPARE(sequence, quint64(0));
    }
    void unauthenticatedConnectionCannotCreateOrEnableMedia()
    {
        int launched = 0;
        VirtualSessionSupervisor supervisor([&](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            ++launched;
            return {};
        });
        VirtualSessionControl control(supervisor, {});
        Server server;
        RdpConnection connection(&server, -1);
        quint64 sequence = 0;
        VirtualSessionTransport transport(1, &connection, control, {}, sequence);
        const auto response = transport.request({{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1}, {u"id"_s, u"1"_s}, {u"action"_s, u"create"_s}});
        QVERIFY(!response.value(u"ok"_s).toBool());
        QCOMPARE(launched, 0);
        const auto media = transport.request({{u"type"_s, u"media"_s}, {u"v"_s, 1}, {u"playback"_s, true}, {u"microphone"_s, false}, {u"camera"_s, false}});
        QVERIFY(!media.value(u"ok"_s).toBool());
        QVERIFY(!control.attachment(1));
        transport.revoke();
        transport.revoke();
        QCOMPARE(sequence, quint64(0));
    }
};
}
QTEST_GUILESS_MAIN(KRdp::VirtualSessionTransportTest)
#include "VirtualSessionTransportTest.moc"
