// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "VideoCodecSupport.h"

#include <optional>

namespace KRdp::EncoderSelection
{
/// The colour range an encoder for \a codec must produce. AVC: full (FreeRDP's and mstsc's
/// H.264 paths honour the VUI). HEVC/AV1: limited - krdp-client converts the decoded private
/// codecs with swscale's defaults, which ignore the signalled range and assume limited (WS-E);
/// the hardware and software HEVC/AV1 paths both follow this.
enum class Range { Limited, Full };
inline Range colorRangeFor(VideoCodec codec)
{
    return codec == VideoCodec::Hevc || codec == VideoCodec::Av1 ? Range::Limited : Range::Full;
}

/// KPipeWire's backend policy for \a codec encoded on the backend the codec policy chose.
/// HEVC/AV1: exactly that backend (the CPU guard and the `backend` KRDPCTL reports rely on it).
/// AVC in hardware keeps KPipeWire's own fallback to libx264 (an output larger than VA-API
/// allows, a driver failure): H.264 is the last resort and must always give a picture.
enum class BackendPolicy { HardwareFirst, HardwareOnly, SoftwareOnly };
inline BackendPolicy backendPolicyFor(VideoCodec codec, bool hardware)
{
    if (!hardware) return BackendPolicy::SoftwareOnly;
    return codec == VideoCodec::Hevc || codec == VideoCodec::Av1 ? BackendPolicy::HardwareOnly : BackendPolicy::HardwareFirst;
}

/**
 * Asks \a stream (a KPipeWire PipeWireEncodedStream, or a test double with the same shape) for
 * the encoder of \a codec: HEVCMain/AV1Main for the private codecs when this KPipeWire has them
 * and suggests them, else H.264 (Main when suggested, else Baseline).
 *
 * \a hardware is the backend the codec policy chose (nullopt: none chosen, a stock client; the
 * KPipeWire default HardwareFirst stays). With a KPipeWire that has backend policies (WS-E) it
 * is set *before* setEncoder(), since suggestedEncoders() follows it. The colour range follows
 * colorRangeFor(). Both take effect at the next start().
 *
 * Returns whether the encoder the stream actually settled on produces \a codec (KPipeWire's
 * setEncoder() silently ignores an encoder it does not suggest). False means the stream runs
 * H.264 while the connection's codec says HEVC/AV1 (AUD-FIX2 F1: Sol labelled H.264 bytes
 * 0x8001): the caller must move the connection off \a codec before the stream starts.
 */
template<typename Stream>
bool apply(Stream *stream, VideoCodec codec, std::optional<bool> hardware = std::nullopt)
{
    if constexpr (requires(Stream *s) { s->setEncoderBackendPolicy(Stream::EncoderBackendPolicy::HardwareOnly); }) {
        using Policy = typename Stream::EncoderBackendPolicy;
        if (hardware) {
            switch (backendPolicyFor(codec, *hardware)) {
            case BackendPolicy::HardwareFirst:
                stream->setEncoderBackendPolicy(Policy::HardwareFirst);
                break;
            case BackendPolicy::HardwareOnly:
                stream->setEncoderBackendPolicy(Policy::HardwareOnly);
                break;
            case BackendPolicy::SoftwareOnly:
                stream->setEncoderBackendPolicy(Policy::SoftwareOnly);
                break;
            }
        }
    }
    if constexpr (requires(Stream *s) {
                      s->setColorRange(Stream::ColorRange::Full);
                      Stream::ColorRange::Limited;
                  }) {
        stream->setColorRange(colorRangeFor(codec) == Range::Limited ? Stream::ColorRange::Limited : Stream::ColorRange::Full);
    }

    auto encoder = Stream::H264Baseline;
    if constexpr (requires(Stream *s) { s->suggestedEncoders(); }) {
        const auto suggested = stream->suggestedEncoders();
        if (suggested.contains(Stream::H264Main)) {
            encoder = Stream::H264Main;
        }
        if constexpr (requires { Stream::HEVCMain; Stream::AV1Main; }) {
            if (codec == VideoCodec::Hevc && suggested.contains(Stream::HEVCMain)) {
                encoder = Stream::HEVCMain;
            } else if (codec == VideoCodec::Av1 && suggested.contains(Stream::AV1Main)) {
                encoder = Stream::AV1Main;
            }
        }
    }
    stream->setEncoder(encoder);
    const auto actual = stream->encoder();
    if (codec == VideoCodec::Hevc || codec == VideoCodec::Av1) {
        if constexpr (requires { Stream::HEVCMain; Stream::AV1Main; }) {
            return actual == (codec == VideoCodec::Hevc ? Stream::HEVCMain : Stream::AV1Main);
        } else {
            return false; // a KPipeWire without HEVC/AV1 at all
        }
    }
    return actual == Stream::H264Main || actual == Stream::H264Baseline;
}
}
