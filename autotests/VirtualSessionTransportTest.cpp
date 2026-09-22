// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <Server.h>
#include "VirtualSessionTransport.h"
#include "ExternalAudioQueue.h"
using namespace KRdp;
using namespace Qt::StringLiterals;

namespace KRdp {
class VirtualSessionTransportTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
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
