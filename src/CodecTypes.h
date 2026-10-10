// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <array>
#include <QtGlobal>

// The small vocabulary shared by CodecPolicy.h and CodecSelection.h (moved out of CodecPolicy.h
// unchanged, OPT-062 S1, so the selection layer can be included by the policy).
namespace KRdp::CodecPolicy
{
/// AV1-Q: how the client decodes a codec (KRDPCTL `codec` request `decode`; Unknown = not said).
enum class DecodePath { Unknown, Hardware, Software };
inline const char *decodePathName(DecodePath path)
{
    switch (path) {
    case DecodePath::Hardware: return "hw";
    case DecodePath::Software: return "sw";
    case DecodePath::Unknown: break;
    }
    return "unknown";
}
struct ClientDecode {
    DecodePath avc = DecodePath::Unknown;
    DecodePath hevc = DecodePath::Unknown;
    DecodePath av1 = DecodePath::Unknown;
    bool operator==(const ClientDecode &) const = default;
};

/// Codec families, in ascending order of compression. Avc = the RDPGFX AVC codec (420/444/444v2)
/// the client's caps select; Hevc/Av1 = the private codecs (0x8001/0x8002).
enum class Family { Avc = 0, Hevc = 1, Av1 = 2 };
inline DecodePath decodePathOf(const ClientDecode &decode, Family f)
{
    return f == Family::Hevc ? decode.hevc : f == Family::Av1 ? decode.av1 : decode.avc;
}
constexpr std::array<Family, 3> BestCompressionFirst{Family::Av1, Family::Hevc, Family::Avc};
inline const char *familyName(Family f)
{
    switch (f) {
    case Family::Avc: return "avc";
    case Family::Hevc: return "hevc";
    case Family::Av1: return "av1";
    }
    return "?";
}

struct Backends {
    bool hardware = false;
    bool software = false;
    /// The software encoder applies a new target bitrate in place, without a reopen (KPipeWire
    /// softwareBitrateChangeIsLive(): libx264, libx265). False: every change reopens (SVT-AV1).
    bool liveBitrate = false;
    bool any() const { return hardware || software; }
    bool operator==(const Backends &) const = default;
};

/// What this host can encode. A software backend the linked KPipeWire does not have
/// (HEVC/AV1 before WS-E, or a stock KPipeWire) is simply false and skipped.
struct Encoders {
    Backends avc;
    Backends hevc;
    Backends av1;
    const Backends &of(Family f) const { return f == Family::Hevc ? hevc : f == Family::Av1 ? av1 : avc; }
    Backends &of(Family f) { return f == Family::Hevc ? hevc : f == Family::Av1 ? av1 : avc; }
    bool operator==(const Encoders &) const = default;
};

}
