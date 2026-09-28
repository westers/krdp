// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QSize>
#include <optional>

#include "VideoCodec.h"

namespace KRdp
{
// Independent payload evidence for a resize completion. Called only on
// candidate keyframes while Fit/recovery is gated, never on normal video.
// Requires self-contained Annex-B SPS/PPS/IDR, bounded dimensions and a clean
// software decode. No hardware device or persistent decoder is created.
std::optional<QSize> h264KeyframeSize(const QByteArray &packet);
/**
 * AUD-FIX7: the same evidence for the codec the worker's encoder produced (VideoFrame::codec):
 * h264KeyframeSize() for AVC; for HEVC and AV1 FFmpeg's parser must report a 4:2:0 keyframe of
 * bounded, even dimensions, and a software decoder, when this FFmpeg has one, must decode it
 * cleanly to that size.
 */
std::optional<QSize> encodedKeyframeSize(VideoCodec codec, const QByteArray &packet);
}
