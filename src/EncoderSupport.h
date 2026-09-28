// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "CodecPolicy.h"
#include "LayoutControl.h"
#include "krdp_export.h"

#include <QString>

/**
 * Which video encoders this host really has (AUD-FIX2 F1), per codec and backend.
 *
 * - **Hardware** (VAAPI): KPipeWire must offer the encoder (`suggestedEncoders()`, and for
 *   HEVC/AV1 a KPipeWire built with them) *and* a trial `avcodec_open2()` of `h264_vaapi` /
 *   `hevc_vaapi` / `av1_vaapi` on the VAAPI render node must succeed - a driver that only
 *   advertises a profile (nvidia-vaapi-driver) does not count.
 * - **Software**: H.264 = libx264 or libopenh264 in libavcodec (KPipeWire's own fallback
 *   order after h264_vaapi). HEVC and AV1 in software are false until KPipeWire's
 *   `makeEncoder()` gains a software path for them (libx265 / SVT-AV1, a separate
 *   workstream); softwareBackend() is the one place that then reports them.
 *
 * Environment (tests, diagnosis):
 * - `KRDP_FORCE_SOFTWARE_ENCODING=1`: report no hardware at all, and applyProcessOverrides()
 *   makes KPipeWire skip h264_vaapi too (`KPIPEWIRE_FORCE_ENCODER=libx264`, or libopenh264).
 * - `KRDP_ENCODERS=avc=hw+sw,hevc=hw,av1=none`: replace the probe's answer (per codec
 *   `hw`, `sw`, `hw+sw` or `none`; codecs not named keep the probed value).
 */
namespace KRdp::EncoderSupport
{
struct Probe {
    CodecPolicy::Encoders encoders;
    bool avc444Hardware = false; ///< AVC444 needs h264_vaapi and a KPipeWire with 4:4:4 chroma
    QString renderNode; ///< the VAAPI node the hardware trial opened on (empty: none)
};

/// The probe, run once per process (the first call can take a few hundred ms).
KRDP_EXPORT const Probe &probe();
/// Runs the probe again, bypassing the cache (tests).
KRDP_EXPORT Probe probeUncached();

KRDP_EXPORT bool softwareForced();
/**
 * Call once at startup, before any encoder exists: with KRDP_FORCE_SOFTWARE_ENCODING set,
 * points KPipeWire at the software H.264 encoder (unless KPIPEWIRE_FORCE_ENCODER is set).
 */
KRDP_EXPORT void applyProcessOverrides();

/// Applies a `KRDP_ENCODERS` value to \a encoders; false if it does not parse (unchanged then).
KRDP_EXPORT bool applyOverride(CodecPolicy::Encoders &encoders, const QString &spec);

/**
 * The `video` group of `capabilities` for \a probe under \a mode: `avc420` always (H.264 in
 * software is the last resort even for `never`), `avc444` with a hardware 4:4:4 encoder,
 * `hevc`/`av1` when the policy could pick them (under `never` only with hardware).
 */
KRDP_EXPORT LayoutControl::VideoCapabilities videoCapabilities(const Probe &probe, CodecPolicy::SoftwareEncoding mode);

/// "avc hw+sw, hevc hw, av1 none" - for the startup log.
KRDP_EXPORT QString describe(const Probe &probe);
}
