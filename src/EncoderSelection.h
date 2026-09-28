// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "VideoCodecSupport.h"

namespace KRdp::EncoderSelection
{
/**
 * Asks \a stream (a KPipeWire PipeWireEncodedStream, or a test double with the same shape) for
 * the encoder of \a codec: HEVCMain/AV1Main for the private codecs when this KPipeWire has them
 * and suggests them, else H.264 (Main when suggested, else Baseline).
 *
 * Returns whether the encoder the stream actually settled on produces \a codec (KPipeWire's
 * setEncoder() silently ignores an encoder it does not suggest). False means the stream runs
 * H.264 while the connection's codec says HEVC/AV1 (AUD-FIX2 F1: Sol labelled H.264 bytes
 * 0x8001): the caller must move the connection off \a codec before the stream starts.
 */
template<typename Stream>
bool apply(Stream *stream, VideoCodec codec)
{
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
