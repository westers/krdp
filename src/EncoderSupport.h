// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "CodecPolicy.h"
#include "LayoutControl.h"
#include "krdp_export.h"

#include <QList>
#include <QSize>
#include <QString>
#include <array>

/**
 * Which video encoders this host really has (AUD-FIX2 F1), per codec and backend.
 *
 * - **Hardware** (VAAPI): KPipeWire must offer the encoder in hardware (the Hardware bit of
 *   `availableEncoderBackends()`; with an older KPipeWire, `suggestedEncoders()`) *and* a trial
 *   `avcodec_open2()` of `h264_vaapi` / `hevc_vaapi` / `av1_vaapi` on the VAAPI render node must
 *   succeed - a driver that only advertises a profile (nvidia-vaapi-driver) does not count.
 * - **Software**: H.264 = libx264 or libopenh264 in libavcodec (KPipeWire's own fallback
 *   order after h264_vaapi). HEVC = libx265 and AV1 = libsvtav1/libaom-av1, reported by a
 *   KPipeWire with the WS-E software path (the Software bit of `availableEncoderBackends()`);
 *   false with an older or stock KPipeWire.
 *
 * Environment (tests, diagnosis):
 * - `KRDP_FORCE_SOFTWARE_ENCODING=1`: report no hardware at all, and applyProcessOverrides()
 *   makes KPipeWire skip h264_vaapi too (`KPIPEWIRE_FORCE_ENCODER=libx264`, or libopenh264).
 *   Software HEVC/AV1 stay available; the codec policy then picks them with the SoftwareOnly
 *   backend policy (EncoderSelection::apply()).
 * - `KRDP_ENCODERS=avc=hw+sw,hevc=hw,av1=none`: replace the probe's answer (per codec
 *   `hw`, `sw`, `hw+sw` or `none`; codecs not named keep the probed value).
 */
namespace KRdp::EncoderSupport
{
/// One NVENC encoder KPipeWire's NVIDIA probe found (a real trial open on that GPU).
struct NvidiaEncoder {
    QString pciId; ///< "0000:09:00.0", the stable identity (the CUDA ordinal is process-local)
    QString name;
    int cudaOrdinal = -1;
    CodecPolicy::Family family = CodecPolicy::Family::Avc;
    bool usable = false;
    QSize maxSize;
    QString failure; ///< why it is not usable ("unsupported", "session-limit", ...)
    bool operator==(const NvidiaEncoder &) const = default;
};

struct Probe {
    CodecPolicy::Encoders encoders;
    bool avc444Hardware = false; ///< AVC444 needs h264_vaapi and a KPipeWire with 4:4:4 chroma
    QString renderNode; ///< the VAAPI node the hardware trial opened on (empty: none); "NVENC <gpu> <pci id>" when NVIDIA supplies the only hardware
    /// Which hardware backend each codec's `hardware` comes from: "vaapi", "nvenc" or empty (no hardware).
    QString avcHardwareVia;
    QString hevcHardwareVia;
    QString av1HardwareVia;
    QList<NvidiaEncoder> nvidia; ///< every NVENC encoder the probe listed (empty: none, or never asked because VA-API covers all codecs)
    QString nvidiaNote; ///< why the NVIDIA stack was unavailable ("driver/library version mismatch ...", "no NVIDIA device")

    const QString &hardwareVia(CodecPolicy::Family family) const
    {
        return family == CodecPolicy::Family::Hevc ? hevcHardwareVia : family == CodecPolicy::Family::Av1 ? av1HardwareVia : avcHardwareVia;
    }
};

/**
 * Everything the probe learns from the host and from KPipeWire, as plain values, so the decision
 * (assemble()) is tested with fake hosts: Hal today, Sol, an NVIDIA-only host, a host whose NVIDIA
 * driver is broken. probeUncached() fills it from the real world.
 */
struct Inputs {
    struct Vaapi {
        bool avc = false;
        bool hevc = false;
        bool av1 = false;
        QString node;
    } vaapi; ///< trial opens of h264/hevc/av1_vaapi on the first working render node
    QList<NvidiaEncoder> nvidia; ///< KPipeWire's NVENC inventory (only asked for when VA-API lacks a codec)
    QString nvidiaNote;
    /// Indexed avc, hevc, av1: KPipeWire's Hardware bit (availableEncoderBackends(); NVENC counts there), its Software bit, in-place software bitrate changes.
    std::array<bool, 3> kpipewireHardware{true, true, true};
    std::array<bool, 3> software{true, true, true};
    std::array<bool, 3> liveBitrate{false, false, false};
    bool chroma444 = true; ///< KPipeWire has 4:4:4 chroma
    bool forcedSoftware = false;
    QString overrideSpec; ///< a FARSIDE_ENCODERS value
};

/// The decision: VA-API first (the capture GPU on Hal), NVENC only for a codec VA-API lacks; then the FARSIDE_ENCODERS override.
KRDP_EXPORT Probe assemble(const Inputs &inputs);

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
