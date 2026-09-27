// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-S1: the fake-peer path. FreeRDP delivers a static channel PDU through
// freerdp_peer::ReceiveChannelData (or VirtualChannelRead when set) from
// freerdp_channel_peer_process(), in every state after licensing, including
// the CONFIRM_ACTIVE/FINALIZATION window that precedes PostConnect. This test
// stands in for that caller and for the channel manager behind the hook: the
// fake downstream deframes KRDPCTL records and "executes" them, as
// RdpConnection::readControlChannel() + SessionController would.

#include <QJsonObject>
#include <QTest>

#include "LayoutControl.h"
#include "PeerContext_p.h"
#include "PreAuthChannelGate.h"

using namespace KRdp;

namespace
{
constexpr UINT16 KrdpctlChannelId = 1004;
QStringList executed;
LayoutControl::Deframer deframer;
int downstreamCalls = 0;

void deliver(const BYTE *data, size_t size)
{
    ++downstreamCalls;
    deframer.feed(QByteArray(reinterpret_cast<const char *>(data), qsizetype(size)));
    while (auto record = deframer.next()) {
        executed.append(record->value(QLatin1String("type")).toString());
    }
}

BOOL fakeChannelManager(freerdp_peer *, UINT16 channelId, const BYTE *data, size_t size, UINT32, size_t)
{
    if (channelId == KrdpctlChannelId) {
        deliver(data, size);
    }
    return TRUE;
}

int fakeVirtualChannelRead(freerdp_peer *, HANDLE, BYTE *data, UINT32 length)
{
    deliver(data, length);
    return int(length);
}

QByteArray record(const char *type)
{
    return LayoutControl::frame(QJsonObject{{QStringLiteral("type"), QLatin1String(type)}});
}

// What freerdp_channel_peer_process() does with one complete channel PDU.
bool sendOnChannel(freerdp_peer &peer, const QByteArray &bytes)
{
    const auto *data = reinterpret_cast<const BYTE *>(bytes.constData());
    const auto flags = CHANNEL_FLAG_FIRST | CHANNEL_FLAG_LAST;
    if (peer.VirtualChannelRead) {
        return peer.VirtualChannelRead(&peer, nullptr, const_cast<BYTE *>(data), UINT32(bytes.size())) >= 0;
    }
    return peer.ReceiveChannelData(&peer, KrdpctlChannelId, data, size_t(bytes.size()), flags, size_t(bytes.size()));
}
}

class PreAuthChannelGateTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init()
    {
        executed.clear();
        deframer = LayoutControl::Deframer();
        downstreamCalls = 0;
    }

    void applyAndQueryBeforeAuthenticationNeverExecute()
    {
        PeerContext context{};
        freerdp_peer peer{};
        peer.context = &context._p;
        peer.ReceiveChannelData = fakeChannelManager;
        PreAuthChannelGate gate;
        gate.install(&peer);
        QCOMPARE(context.channelGate, &gate);
        QVERIFY(peer.ReceiveChannelData != fakeChannelManager);

        // Pre-authentication: accepted by the transport (the handshake goes
        // on, PostConnect decides), but nothing reaches the channel manager.
        QVERIFY(sendOnChannel(peer, record("apply")));
        QVERIFY(sendOnChannel(peer, record("query")));
        QVERIFY(sendOnChannel(peer, record("attach") + record("media")));
        QCOMPARE(downstreamCalls, 0);
        QVERIFY(executed.isEmpty());
        QCOMPARE(gate.droppedPdus(), std::uint64_t(3));

        // Authenticated: the same records now reach the handler, and nothing
        // dropped earlier is replayed.
        gate.authorize();
        QVERIFY(sendOnChannel(peer, record("query")));
        QVERIFY(sendOnChannel(peer, record("apply")));
        QCOMPARE(executed, (QStringList{QStringLiteral("query"), QStringLiteral("apply")}));
        QCOMPARE(gate.droppedPdus(), std::uint64_t(3));
    }

    void virtualChannelReadHookIsGatedToo()
    {
        PeerContext context{};
        freerdp_peer peer{};
        peer.context = &context._p;
        peer.ReceiveChannelData = fakeChannelManager;
        peer.VirtualChannelRead = fakeVirtualChannelRead;
        PreAuthChannelGate gate;
        gate.install(&peer);

        QVERIFY(sendOnChannel(peer, record("apply")));
        QVERIFY(executed.isEmpty());
        gate.authorize();
        QVERIFY(sendOnChannel(peer, record("query")));
        QCOMPARE(executed, QStringList{QStringLiteral("query")});
    }

    void installingTwiceKeepsTheRealHook()
    {
        PeerContext context{};
        freerdp_peer peer{};
        peer.context = &context._p;
        peer.ReceiveChannelData = fakeChannelManager;
        PreAuthChannelGate gate;
        gate.install(&peer);
        gate.install(&peer);
        gate.authorize();
        QVERIFY(sendOnChannel(peer, record("query")));
        QCOMPARE(executed, QStringList{QStringLiteral("query")});
    }

    void peerWithoutItsGateFailsClosed()
    {
        PeerContext context{};
        freerdp_peer peer{};
        peer.context = &context._p;
        peer.ReceiveChannelData = fakeChannelManager;
        {
            PreAuthChannelGate gate;
            gate.install(&peer);
        }
        context.channelGate = nullptr;
        QVERIFY(!sendOnChannel(peer, record("apply")));
        QVERIFY(executed.isEmpty());
    }
};

QTEST_GUILESS_MAIN(PreAuthChannelGateTest)
#include "PreAuthChannelGateTest.moc"
