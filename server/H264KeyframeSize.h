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
 * AUD-FIX10: what one keyframe codes and what it shows. `coded` is the picture the decoder
 * produces before any crop (H.264 macroblocks, HEVC pic_*_in_luma_samples, AV1 FrameWidth x
 * FrameHeight). `display` is the size the stream says to show: H.264 frame cropping, the HEVC
 * conformance window, AV1 render_size(). `displaySignalled` is false only for AV1 with
 * render_and_frame_size_different = 0, where the render size just repeats the frame size: AMD VCN
 * (FFmpeg av1_vaapi on radeonsi) codes a 1920x1080 picture as 1920x1082 (1366x768 as 1408x768)
 * that way, with no size of its own for the real picture.
 */
struct EncodedKeyframe {
    QSize coded;
    QSize display;
    bool displaySignalled = true;
};

/**
 * AUD-FIX7: the same evidence for the codec the worker's encoder produced (VideoFrame::codec):
 * h264KeyframeSize() for AVC; for HEVC and AV1 FFmpeg's parser must report a 4:2:0 keyframe of
 * bounded, even dimensions, and a software decoder, when this FFmpeg has one, must decode it
 * cleanly to that size. AV1 also needs a sequence header and a shown key frame header this
 * parser can read (for the render size).
 */
std::optional<EncodedKeyframe> encodedKeyframe(VideoCodec codec, const QByteArray &packet);
/// encodedKeyframe()'s display size.
std::optional<QSize> encodedKeyframeSize(VideoCodec codec, const QByteArray &packet);
/**
 * The alignment (width, height) an encoder may pad a coded picture to when the stream has no
 * display size of its own. AV1: 64 x 16, what AMD VCN does (measured on Hal's 780M and cray's
 * Strix Halo, Mesa radeonsi via FFmpeg av1_vaapi): widths round up to 64 (1366 -> 1408, 1680 ->
 * 1728), heights to 16, or by 2 when height % 16 == 8 (1080 -> 1082, 1000 -> 1002).
 */
QSize encodedKeyframeAlignment(VideoCodec codec);
/**
 * AUD-FIX10 R5: whether this keyframe is proof of an output of \a pixels. The display size must
 * equal \a pixels exactly. Only when the stream signals no display size (see EncodedKeyframe),
 * the coded size may instead lie, per dimension, between \a pixels and \a pixels rounded up to
 * encodedKeyframeAlignment() (the AMD AV1 1920x1082 and 1408x768 pictures). krdp-client (0.5.4,
 * PrivateFrameGeometry.h) crops exactly these AMD sizes to the surface.
 */
bool encodedKeyframeShows(VideoCodec codec, const QByteArray &packet, QSize pixels);
}
