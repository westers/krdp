// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <optional>

#include <QByteArray>

#include "VideoCodec.h"

namespace KRdp
{
/**
 * AUD-FIX11 R6: whether a keyframe can start a decoder on its own - the codec's parameter sets
 * in band, as a client that never saw extradata (krdp-client's per-surface FFmpeg decoders,
 * FreeRDP's H.264) needs them: an AV1 sequence header OBU; HEVC VPS, SPS and PPS before an IRAP
 * slice; H.264 SPS and PPS before an IDR slice (Annex B). Looks at unit headers only.
 */
inline bool keyframeCarriesHeaders(VideoCodec codec, const QByteArray &packet)
{
    const auto *data = reinterpret_cast<const unsigned char *>(packet.constData());
    const qsizetype size = packet.size();
    if (codec == VideoCodec::Av1) {
        // Low-overhead OBUs, each with obu_has_size_field (KPipeWire/FFmpeg always write it).
        bool sequence = false;
        for (qsizetype i = 0; i < size;) {
            const unsigned char header = data[i];
            if ((header & 0x80) || !(header & 0x2)) return false;
            const int type = (header >> 3) & 0xf;
            qsizetype at = i + 1 + ((header & 0x4) ? 1 : 0);
            quint64 obuSize = 0;
            int n = 0;
            for (;; ++n) {
                if (n == 8 || at + n >= size) return false;
                obuSize |= quint64(data[at + n] & 0x7f) << (7 * n);
                if (!(data[at + n] & 0x80)) break;
            }
            at += n + 1;
            if (obuSize > quint64(size - at)) return false;
            if (type == 1) sequence = true; // OBU_SEQUENCE_HEADER
            if ((type == 3 || type == 6) && sequence) return true; // OBU_FRAME_HEADER / OBU_FRAME after it
            i = at + qsizetype(obuSize);
        }
        return false;
    }
    const bool hevc = codec == VideoCodec::Hevc;
    bool vps = !hevc;
    bool sps = false;
    bool pps = false;
    for (qsizetype i = 0; i + 3 < size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) continue;
        const unsigned char header = data[i + 3];
        const int type = hevc ? (header >> 1) & 0x3f : header & 0x1f;
        if (hevc) {
            if (type == 32) vps = true;
            else if (type == 33) sps = true;
            else if (type == 34) pps = true;
            else if (type >= 16 && type <= 21) return vps && sps && pps; // BLA/IDR/CRA
        } else {
            if (type == 7) sps = true;
            else if (type == 8) pps = true;
            else if (type == 5) return sps && pps;
        }
        i += 2;
    }
    return false;
}

/// Codec families: AVC420/AVC444/AVC444v2 are one H.264 bitstream family.
inline int codecFamily(VideoCodec codec)
{
    return codec == VideoCodec::Hevc ? 1 : codec == VideoCodec::Av1 ? 2 : 0;
}

/**
 * AUD-FIX11 R6: the rule for one RDPGFX surface: nothing of a codec goes out on it before a
 * keyframe of that codec with its headers in band has gone out on it, and a surface whose chain
 * a drop has broken waits for the next such keyframe. A fresh surface (after a graphics reset)
 * starts with no chain. Frames that do not say which codec produced them (tests' synthetic
 * payloads) are the connection's codec; only their keyframes are not inspected for headers.
 */
class SurfaceChain
{
public:
    enum class Verdict {
        Send, ///< continues (or, a keyframe, starts) this surface's chain
        WaitForKeyFrame, ///< a delta with no chain of its codec on this surface: drop, ask for a keyframe
        KeyFrameWithoutHeaders, ///< a keyframe no fresh decoder can start from: drop, ask for a keyframe
    };

    /// Whether a frame of \a codec may go out on this surface now (and, if so, record it).
    Verdict admit(VideoCodec codec, bool codecKnown, bool isKeyFrame, const QByteArray &data)
    {
        if (isKeyFrame) {
            if (codecKnown && !keyframeCarriesHeaders(codec, data)) {
                m_codec.reset();
                return Verdict::KeyFrameWithoutHeaders;
            }
            m_codec = codec;
            return Verdict::Send;
        }
        // A new AVC format also opens a new encoder/auxiliary chain. Its deltas
        // cannot reference the prior420/444/v2 encoder's decoded picture.
        return m_codec == codec ? Verdict::Send : Verdict::WaitForKeyFrame;
    }
    /// A frame of this surface's chain was not sent: the next one must be a keyframe.
    void broken()
    {
        m_codec.reset();
    }
    std::optional<int> family() const
    {
        return m_codec ? std::optional(codecFamily(*m_codec)) : std::nullopt;
    }

private:
    std::optional<VideoCodec> m_codec;
};
}
