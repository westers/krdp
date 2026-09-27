// SPDX-FileCopyrightText: 2026 Steve Westers
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PreAuthChannelGate.h"

#include "PeerContext_p.h"

#include "krdp_logging.h"

namespace KRdp
{

void PreAuthChannelGate::install(freerdp_peer *peer)
{
    if (!peer || !peer->context) {
        return;
    }
    reinterpret_cast<PeerContext *>(peer->context)->channelGate = this;
    if (peer->ReceiveChannelData && peer->ReceiveChannelData != &PreAuthChannelGate::receiveChannelData) {
        m_receiveChannelData = peer->ReceiveChannelData;
        peer->ReceiveChannelData = &PreAuthChannelGate::receiveChannelData;
    }
    if (peer->VirtualChannelRead && peer->VirtualChannelRead != &PreAuthChannelGate::virtualChannelRead) {
        m_virtualChannelRead = peer->VirtualChannelRead;
        peer->VirtualChannelRead = &PreAuthChannelGate::virtualChannelRead;
    }
}

void PreAuthChannelGate::authorize()
{
    m_authorized.store(true);
}

bool PreAuthChannelGate::authorized() const
{
    return m_authorized.load();
}

std::uint64_t PreAuthChannelGate::droppedPdus() const
{
    return m_dropped.load();
}

PreAuthChannelGate *PreAuthChannelGate::gateOf(freerdp_peer *peer)
{
    if (!peer || !peer->context) {
        return nullptr;
    }
    return reinterpret_cast<PeerContext *>(peer->context)->channelGate;
}

bool PreAuthChannelGate::admit()
{
    if (m_authorized.load()) {
        return true;
    }
    if (m_dropped.fetch_add(1) == 0) {
        qCWarning(KRDP) << "Dropping virtual channel data sent before authentication";
    }
    return false;
}

BOOL PreAuthChannelGate::receiveChannelData(freerdp_peer *peer, UINT16 channelId, const BYTE *data, size_t size, UINT32 flags, size_t totalSize)
{
    auto *gate = gateOf(peer);
    if (!gate || !gate->m_receiveChannelData) {
        return FALSE; // Fail closed: a peer without its gate cannot deliver anything.
    }
    if (!gate->admit()) {
        // Consumed, not an error: the caller skips the chunk and carries on
        // with the handshake, which PostConnect then decides.
        return TRUE;
    }
    return gate->m_receiveChannelData(peer, channelId, data, size, flags, totalSize);
}

int PreAuthChannelGate::virtualChannelRead(freerdp_peer *peer, HANDLE channel, BYTE *buffer, UINT32 length)
{
    auto *gate = gateOf(peer);
    if (!gate || !gate->m_virtualChannelRead) {
        return -1;
    }
    if (!gate->admit()) {
        return 0;
    }
    return gate->m_virtualChannelRead(peer, channel, buffer, length);
}

}
