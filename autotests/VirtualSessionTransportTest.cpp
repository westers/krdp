// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <Server.h>
#include <VideoStream.h>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QEvent>
#include <limits>
#include "VirtualSessionTransport.h"
#include "ExternalAudioQueue.h"
using namespace KRdp;
using namespace Qt::StringLiterals;

namespace KRdp {
class VirtualSessionTransportTest : public QObject
{
    Q_OBJECT
    ConsoleWorkerWire::Deframer m_workerDeframer;
    static QJsonObject media(bool mic = true, bool playback = true, bool camera = false) {
        return {{u"type"_s, u"media"_s}, {u"v"_s, 1}, {u"playback"_s, playback},
            {u"microphone"_s, mic}, {u"camera"_s, camera}, {u"silenceHost"_s, playback}};
    }
    void microphoneFixture(const std::function<void(VirtualSessionTransport &, VirtualSessionControl &,
                                                    ConsoleWorkerEndpoint &, QLocalSocket &)> &test) {
        m_workerDeframer = {};
        VirtualSessionSupervisor supervisor([](quint32, const auto &) -> std::optional<VirtualSessionSupervisor::Launch> {
            return VirtualSessionSupervisor::Launch{u"/usr/bin/sleep"_s, {u"60"_s}, {}, {}};
        });
        QPointer<VirtualSessionTransport> transport;
        VirtualSessionControl control(supervisor, [&](quint64, const auto &) { if (transport) transport->revoke(); });
        const auto handle = supervisor.create(1000); QVERIFY(handle);
        bool ready = false; QTRY_VERIFY(ready || (ready = supervisor.captureReady(*handle)));
        QVERIFY(control.request(1000, 1, {{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1},
            {u"id"_s, u"attach"_s}, {u"action"_s, u"attach"_s}, {u"session"_s, handle->id}}).value(u"ok"_s).toBool());
        QTemporaryDir dir; ConsoleWorkerEndpoint endpoint; QLocalSocket worker;
        const QByteArray token(32, 't');
        QVERIFY(endpoint.listen(dir.filePath(u"worker.sock"_s), {ConsoleSeat::Adapter::VirtualUser, handle->id, 1000}, token));
        worker.connectToServer(endpoint.socketName()); QVERIFY(worker.waitForConnected(1000));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{handle->id, 1000, token}));
        worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        QVERIFY(worker.waitForBytesWritten(1000)); QTRY_VERIFY(endpoint.ready());
        Server server; RdpConnection connection(&server, -1); quint64 sequence = 0;
        // No socket/PAM/capture: remove only this fixture's queued initialization.
        QCoreApplication::removePostedEvents(&connection, QEvent::MetaCall);
        transport = new VirtualSessionTransport(1, &connection, control, {}, sequence, &connection);
        QVERIFY(transport->m_externalMicrophone);
        QVERIFY(!transport->activateBinding(*handle, &endpoint)); // No fabricated production PAM success.
        QVERIFY(!transport->authorized()); QVERIFY(transport->authorized(1000));
        test(*transport, control, endpoint, worker);
        delete transport.data();
    }
    QList<ConsoleWorkerWire::Record> workerRecords(QLocalSocket &worker) {
        QCoreApplication::processEvents();
        worker.waitForReadyRead(20);
        m_workerDeframer.feed(worker.readAll());
        QList<ConsoleWorkerWire::Record> records;
        while (const auto record = m_workerDeframer.next()) records.append(*record);
        return records;
    }
private Q_SLOTS:
    void microphonePendingCorrelatedReadinessAndPcm() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            QVERIFY(t.request(media(), 1000).isEmpty()); // No early success.
            QVERIFY(!t.m_microphoneReady); QVERIFY(t.m_microphoneDeadline.isActive());
            const auto policy = t.m_microphonePolicy;
            bool enabled = false;
            for (const auto &record : workerRecords(worker))
                if (auto p = ConsoleWorkerWire::microphonePolicy(record)) { QCOMPARE(*p, policy); enabled = true; }
            QVERIFY(enabled);
            QVERIFY(t.microphoneResult({policy.generation + 1, policy.requestId, {}}, 1000).isEmpty());
            QVERIFY(t.microphoneResult({policy.generation, policy.requestId + 1, {}}, 1000).isEmpty());
            QVERIFY(!t.forwardMicrophone(QByteArray(3840, 'a'), 1000));
            const auto ack = t.microphoneResult({policy.generation, policy.requestId, {}}, 1000);
            QVERIFY(ack.value(u"ok"_s).toBool()); QVERIFY(ack.value(u"microphone"_s).toBool());
            QVERIFY(ack.value(u"playback"_s).toBool()); QVERIFY(ack.value(u"silenceHost"_s).toBool());
            QVERIFY(!t.m_microphoneDeadline.isActive()); QCOMPARE(t.m_microphonePump.interval(), 20);
            QVERIFY(t.m_microphonePump.isActive());
            t.m_microphonePump.stop(); // Fixture has no PAM; timer correctly refuses that identity.
            QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).isEmpty());
            QVERIFY(!t.forwardMicrophone(QByteArray(3844, 'a'), 1000));
            QVERIFY(!t.forwardMicrophone(QByteArray(3, 'a'), 1000));
            QVERIFY(!t.forwardMicrophone(QByteArray(3840, 'a'), 1001));
            const QByteArray pcm(3840, 'a'); QVERIFY(t.forwardMicrophone(pcm, 1000));
            bool audio = false;
            for (const auto &record : workerRecords(worker)) if (auto a = ConsoleWorkerWire::microphoneAudio(record)) {
                QCOMPARE(a->generation, policy.generation); QCOMPARE(a->requestId, policy.requestId); QCOMPARE(a->pcm, pcm); audio = true;
            }
            QVERIFY(audio);
            const auto off = t.request(media(false), 1000); QVERIFY(off.value(u"ok"_s).toBool());
            QVERIFY(!off.value(u"microphone"_s).toBool()); QVERIFY(!t.m_microphoneReady);
            QVERIFY(!t.m_microphonePump.isActive());
            // No AUDIN producer is injected here. Queue generation/expiry/clearing
            // are exercised separately by RdpAudioPriorityTest, not this empty queue.
            QVERIFY(t.m_connection->takeExternalMicrophone().isEmpty());
            QVERIFY(!t.forwardMicrophone(pcm, 1000));
            QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).isEmpty());
            bool disabled = false;
            for (const auto &record : workerRecords(worker)) if (auto p = ConsoleWorkerWire::microphonePolicy(record)) {
                QVERIFY(!p->enabled); QCOMPARE(p->generation, policy.generation); QVERIFY(p->requestId > policy.requestId); disabled = true;
            }
            QVERIFY(disabled);
            QVERIFY(t.request(media(), 1000).isEmpty()); const auto next = t.m_microphonePolicy;
            QCOMPARE(next.generation, policy.generation); QVERIFY(next.requestId > policy.requestId);
            QVERIFY(t.microphoneResult({policy.generation, policy.requestId, u"late old failure"_s}, 1000).isEmpty());
            QCOMPARE(t.m_microphonePolicy, next); QVERIFY(t.m_microphoneDeadline.isActive());
        });
    }
    void microphoneFailuresPreservePlaybackAndSilence() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &) {
            for (bool timeout : {false, true}) {
                QVERIFY(t.request(media(), 1000).isEmpty()); const auto policy = t.m_microphonePolicy;
                const auto reply = timeout ? t.microphoneTimeout()
                    : t.microphoneResult({policy.generation, policy.requestId, u"source failed"_s}, 1000);
                QVERIFY(!reply.value(u"ok"_s).toBool()); QVERIFY(!reply.value(u"microphone"_s).toBool());
                QVERIFY(reply.value(u"playback"_s).toBool()); QVERIFY(reply.value(u"silenceHost"_s).toBool());
                QVERIFY(!t.m_microphonePolicy.enabled); QVERIFY(!t.m_microphoneDeadline.isActive());
                QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).isEmpty());
            }
            t.m_externalMicrophone = false;
            QVERIFY(!t.request(media(), 1000).value(u"ok"_s).toBool()); QVERIFY(t.m_playback); QVERIFY(t.m_silenceHost);
            t.m_externalMicrophone = true;
            QVERIFY(!t.request(media(true, true, true), 1000).value(u"ok"_s).toBool());
        });
    }
    void microphoneDetachRebindAndOverflow() {
        microphoneFixture([&](auto &t, auto &control, auto &endpoint, auto &worker) {
            const auto handle = *t.m_handle;
            QVERIFY(t.request(media(), 1000).isEmpty()); const auto old = t.m_microphonePolicy;
            control.disconnected(1);
            QVERIFY(!t.m_endpoint); QVERIFY(!t.m_handle); QVERIFY(!t.m_microphonePolicy.enabled);
            QVERIFY(!t.m_microphoneDeadline.isActive()); QVERIFY(!t.m_microphonePump.isActive());
            bool disabled = false;
            for (const auto &record : workerRecords(worker))
                if (auto p = ConsoleWorkerWire::microphonePolicy(record); p && !p->enabled) disabled = true;
            QVERIFY(disabled);
            QVERIFY(control.request(1000, 1, {{u"type"_s, u"virtual-session"_s}, {u"v"_s, 1},
                {u"id"_s, u"reattach"_s}, {u"action"_s, u"attach"_s}, {u"session"_s, handle.id}}).value(u"ok"_s).toBool());
            QVERIFY(!t.activateBinding(handle, &endpoint)); QVERIFY(t.authorized(1000));
            QVERIFY(t.request(media(), 1000).isEmpty());
            QVERIFY(t.m_microphonePolicy.generation != old.generation);
            QVERIFY(t.microphoneResult({old.generation, old.requestId, {}}, 1000).isEmpty()); QVERIFY(!t.m_microphoneReady);
            t.stopMicrophone();
            t.m_nextMicrophoneId = std::numeric_limits<quint64>::max() - 2;
            QVERIFY(t.request(media(), 1000).isEmpty());
            QCOMPARE(t.m_microphonePolicy.requestId, std::numeric_limits<quint64>::max() - 1);
            QVERIFY(t.request(media(false), 1000).value(u"ok"_s).toBool());
            QCOMPARE(t.m_nextMicrophoneId, std::numeric_limits<quint64>::max());
            QVERIFY(!t.request(media(), 1000).value(u"ok"_s).toBool());
            QCOMPARE(t.m_nextMicrophoneId, std::numeric_limits<quint64>::max());
        });
    }
    void microphoneRechecksTargetAndProductionIdentity() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &) {
            QVERIFY(!t.request(media()).value(u"ok"_s).toBool());
            QVERIFY(!t.request(media(), 1001).value(u"ok"_s).toBool());
            const auto handle = *t.m_handle;
            t.m_handle->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QVERIFY(!t.authorized(1000)); t.m_handle = handle;
            QVERIFY(t.request(media(), 1000).isEmpty()); const auto p = t.m_microphonePolicy;
            const auto refused = t.microphoneResult({p.generation, p.requestId, {}}, std::nullopt);
            QVERIFY(!refused.value(u"ok"_s).toBool()); QVERIFY(!t.m_microphoneReady);
            QVERIFY(t.request(media(), 1000).isEmpty()); const auto active = t.m_microphonePolicy;
            QVERIFY(t.microphoneResult({active.generation, active.requestId, {}}, 1000).value(u"ok"_s).toBool());
            t.pumpMicrophone(); // Production PAM-only identity refuses fixture identity.
            QVERIFY(!t.m_microphoneReady); QVERIFY(!t.m_microphonePump.isActive());
        });
    }
    void microphoneRejectsPhysicalAndWrongSessionEndpoint() {
        microphoneFixture([&](auto &t, auto &, auto &original, auto &) {
            for (bool physical : {true, false}) {
                QTemporaryDir dir; ConsoleWorkerEndpoint other; QLocalSocket worker;
                const auto session = physical ? t.m_handle->id : QUuid::createUuid().toString(QUuid::WithoutBraces);
                const QByteArray token(32, 'x');
                QVERIFY(other.listen(dir.filePath(u"other.sock"_s),
                    {physical ? ConsoleSeat::Adapter::PhysicalUser : ConsoleSeat::Adapter::VirtualUser, session, 1000}, token));
                worker.connectToServer(other.socketName()); QVERIFY(worker.waitForConnected(1000));
                worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{session, 1000, token}));
                worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
                QVERIFY(worker.waitForBytesWritten(1000)); QTRY_VERIFY(other.ready());
                t.m_endpoint = &other;
                QVERIFY(!t.authorized(1000));
                QVERIFY(!t.request(media(), 1000).value(u"ok"_s).toBool());
                QVERIFY(!t.m_microphonePolicy.enabled);
                t.m_endpoint = &original;
            }
        });
    }
    void microphoneRevokeRejectsReentrantConsent() {
        microphoneFixture([&](auto &t, auto &, auto &, auto &worker) {
            QVERIFY(t.request(media(), 1000).isEmpty()); const auto policy = t.m_microphonePolicy;
            int callbacks = 0;
            QObject::connect(t.m_connection->videoStream(), &VideoStream::enabledChanged, &t, [&] {
                if (t.m_connection->videoStream()->enabled()) return;
                ++callbacks;
                QVERIFY(t.m_revoking); QVERIFY(!t.m_microphonePolicy.enabled);
                QVERIFY(!t.m_microphonePump.isActive()); QVERIFY(!t.m_microphoneDeadline.isActive());
                QVERIFY(t.request(media(), 1000).isEmpty());
                QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).isEmpty());
            });
            t.revoke(); QCOMPARE(callbacks, 1);
            bool sourceOff = false;
            for (const auto &record : workerRecords(worker)) {
                if (auto p = ConsoleWorkerWire::microphonePolicy(record); p && !p->enabled) sourceOff = true;
                if (record.kind == ConsoleWorkerWire::Kind::ControlState) {
                    const auto state = ConsoleWorkerWire::controlState(record); QVERIFY(state);
                    if (!state->active) QVERIFY(sourceOff); // Off dispatched before endpoint/grant teardown.
                }
            }
            QVERIFY(sourceOff); QVERIFY(!t.m_microphoneReady);
        });
    }
    void microphoneSocketLossPendingAndReady() {
        for (bool ready : {false, true}) microphoneFixture([&](auto &t, auto &control, auto &endpoint, auto &worker) {
            QVERIFY(t.request(media(), 1000).isEmpty()); const auto policy = t.m_microphonePolicy;
            if (ready) QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).value(u"ok"_s).toBool());
            worker.abort();
            QTRY_VERIFY(!endpoint.ready());
            QVERIFY(!control.attachment(1)); QVERIFY(!t.m_endpoint); QVERIFY(!t.m_handle);
            QVERIFY(!t.m_microphoneReady); QVERIFY(!t.m_microphonePolicy.enabled);
            QVERIFY(!t.m_microphoneDeadline.isActive()); QVERIFY(!t.m_microphonePump.isActive());
            QVERIFY(!t.m_playback); QVERIFY(!t.m_silenceHost);
            QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).isEmpty());
        });
    }
    void microphoneActiveRevokeMayDestroyTransport() {
        microphoneFixture([&](auto &t, auto &control, auto &, auto &worker) {
            QVERIFY(t.request(media(), 1000).isEmpty()); const auto policy = t.m_microphonePolicy;
            QVERIFY(t.microphoneResult({policy.generation, policy.requestId, {}}, 1000).value(u"ok"_s).toBool());
            QVERIFY(t.m_microphoneReady); QVERIFY(t.m_microphonePump.isActive());
            const auto connection = t.m_connection;
            QPointer<VirtualSessionTransport> alive(&t);
            QObject::connect(connection->videoStream(), &VideoStream::enabledChanged, connection, [alive] {
                if (alive && !alive->m_connection->videoStream()->enabled()) delete alive.data();
            });
            t.closed();
            QVERIFY(!alive); QVERIFY(!control.attachment(1));
            bool off = false;
            for (const auto &record : workerRecords(worker))
                if (auto p = ConsoleWorkerWire::microphonePolicy(record); p && !p->enabled) off = true;
            QVERIFY(off);
        });
    }
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
