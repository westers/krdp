// SPDX-FileCopyrightText: 2026 Steve Westers
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <atomic>
#include <cstdint>

#include <freerdp/peer.h>

#include "krdp_export.h"

namespace KRdp
{

/**
 * The pre-authentication gate for every static virtual channel PDU (AUD-S1).
 *
 * FreeRDP 3 only calls freerdp_peer::PostConnect, where KRdp checks the
 * credentials, once the client has sent its Confirm Active PDU and every
 * finalization PDU (peer.c: rdp_peer_handle_state_active(), reached after
 * CONNECTION_STATE_FINALIZATION_FONT_LIST). Throughout that window
 * peer_recv_tpkt_pdu() already hands any non-global MCS channel PDU to
 * freerdp_channel_peer_process(), which delivers it through
 * freerdp_peer::VirtualChannelRead or, when that is unset (KRdp's case),
 * freerdp_peer::ReceiveChannelData (WTSReceiveChannelData(), the channel
 * manager). DRDYNVC is one of those static channels, so this also covers every
 * dynamic channel.
 *
 * install() puts this gate in front of both hooks. Until authorize() is called
 * every channel PDU is consumed and dropped (counted, never queued), so nothing
 * a client sends on any channel before PostConnect authentication succeeds can
 * reach a channel handler, KRDPCTL included.
 *
 * The gate finds itself through PeerContext::channelGate, so the peer's
 * context must be a KRdp PeerContext.
 */
class KRDP_EXPORT PreAuthChannelGate
{
public:
    /** Wrap the peer's channel-data hooks. Call once the peer context (and so the channel manager) exists. */
    void install(freerdp_peer *peer);
    /** Let channel data through. Only after PostConnect authentication succeeded; never undone. */
    void authorize();
    bool authorized() const;
    /** Channel PDUs dropped because they arrived before authorize(). */
    std::uint64_t droppedPdus() const;

private:
    static BOOL receiveChannelData(freerdp_peer *peer, UINT16 channelId, const BYTE *data, size_t size, UINT32 flags, size_t totalSize);
    static int virtualChannelRead(freerdp_peer *peer, HANDLE channel, BYTE *buffer, UINT32 length);
    static PreAuthChannelGate *gateOf(freerdp_peer *peer);
    bool admit();

    std::atomic<bool> m_authorized = false;
    std::atomic<std::uint64_t> m_dropped = 0;
    psPeerReceiveChannelData m_receiveChannelData = nullptr;
    psPeerVirtualChannelRead m_virtualChannelRead = nullptr;
};

}
