// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "EncoderSupport.h"

#include "RdpConnection.h"
#include "RenderNodes.h"
#include "krdp_logging.h"

#include <PipeWireEncodedStream>

#include <QDir>
#include <QElapsedTimer>

#include <algorithm>
#include <mutex>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
}

namespace KRdp::EncoderSupport
{
namespace
{
using CodecPolicy::Backends;
using CodecPolicy::Encoders;
using CodecPolicy::Family;

bool hasEncoder(const char *name)
{
    return avcodec_find_encoder_by_name(name) != nullptr;
}

/// A real open of \a encoder on \a device (small NV12 surfaces), not a profile lookup.
bool trialOpen(AVBufferRef *device, const char *encoder)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(encoder);
    if (!codec) {
        return false;
    }
    AVBufferRef *frames = av_hwframe_ctx_alloc(device);
    if (!frames) {
        return false;
    }
    auto *framesContext = reinterpret_cast<AVHWFramesContext *>(frames->data);
    framesContext->format = AV_PIX_FMT_VAAPI;
    framesContext->sw_format = AV_PIX_FMT_NV12;
    framesContext->width = 256;
    framesContext->height = 256;
    framesContext->initial_pool_size = 4;
    bool ok = false;
    if (av_hwframe_ctx_init(frames) >= 0) {
        AVCodecContext *context = avcodec_alloc_context3(codec);
        if (context) {
            context->width = 256;
            context->height = 256;
            context->time_base = {1, 60};
            context->framerate = {60, 1};
            context->pix_fmt = AV_PIX_FMT_VAAPI;
            context->hw_frames_ctx = av_buffer_ref(frames);
            ok = avcodec_open2(context, codec, nullptr) >= 0;
            avcodec_free_context(&context);
        }
    }
    av_buffer_unref(&frames);
    return ok;
}

struct Hardware {
    bool avc = false;
    bool hevc = false;
    bool av1 = false;
    QString node;
};

/// Like KPipeWire's VaapiUtils: the first render node that can encode H.264 is the VAAPI device.
/// (Name order; in a virtual desktop there is only the granted node.)
Hardware probeHardware()
{
    Hardware hw;
    if (!hasEncoder("h264_vaapi")) {
        return hw;
    }
    const int previousLevel = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET); // a failing trial is expected, not an error
    // AUD-FIX8 B3: stat()-based, so a bind-mounted node in a virtual desktop's sandbox counts;
    // the node the launcher granted (KRDP_RENDER_NODE) first.
    const auto nodes = RenderNodes::ordered(RenderNodes::preferredFromEnvironment());
    for (const QString &node : nodes) {
        const QByteArray path = QFile::encodeName(node);
        AVBufferRef *device = nullptr;
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, path.constData(), nullptr, 0) < 0) {
            continue;
        }
        if (trialOpen(device, "h264_vaapi")) {
            hw.avc = true;
            hw.hevc = trialOpen(device, "hevc_vaapi");
            hw.av1 = trialOpen(device, "av1_vaapi");
            hw.node = QString::fromLatin1(path);
        }
        av_buffer_unref(&device);
        if (hw.avc) {
            break;
        }
    }
    av_log_set_level(previousLevel);
    return hw;
}

/// The KPipeWire encoder value of \a family (H264Main for AVC); nullopt: this KPipeWire has none.
template<typename Stream>
std::optional<typename Stream::Encoder> encoderOf(Family family)
{
    switch (family) {
    case Family::Avc:
        return Stream::H264Main;
    case Family::Hevc:
        if constexpr (requires { Stream::HEVCMain; }) {
            return Stream::HEVCMain;
        }
        break;
    case Family::Av1:
        if constexpr (requires { Stream::AV1Main; }) {
            return Stream::AV1Main;
        }
        break;
    }
    return std::nullopt;
}

template<typename Stream>
constexpr bool kpipewireHasBackends()
{
    return requires(typename Stream::Encoder e) { Stream::availableEncoderBackends(e); };
}

/**
 * Whether this KPipeWire build can be asked for \a family in hardware. With backend reporting
 * (WS-E) that is the Hardware bit of availableEncoderBackends(): suggestedEncoders() now also
 * lists HEVC/AV1 when only libx265/libsvtav1 exist. A KPipeWire without it suggests HEVC/AV1
 * only for VA-API, and a stock one has no HEVC/AV1 at all.
 */
template<typename Stream>
bool kpipewireOffersHardware(const QList<typename Stream::Encoder> &suggested, Family family)
{
    if constexpr (kpipewireHasBackends<Stream>()) {
        using Backend = typename Stream::EncoderBackend;
        const auto hardware = [](typename Stream::Encoder e) {
            return bool(Stream::availableEncoderBackends(e) & Backend::Hardware);
        };
        if (family == Family::Avc) {
            return hardware(Stream::H264Main) || hardware(Stream::H264Baseline);
        }
        const auto encoder = encoderOf<Stream>(family);
        return encoder && hardware(*encoder);
    } else {
        if (family == Family::Avc) {
            return suggested.contains(Stream::H264Main) || suggested.contains(Stream::H264Baseline);
        }
        const auto encoder = encoderOf<Stream>(family);
        return encoder && suggested.contains(*encoder);
    }
}

template<typename Stream>
constexpr bool kpipewireHasNvidia()
{
    return requires { Stream::nvidiaEncoders(); Stream::nvidiaUnavailableReason(); };
}

template<typename Stream>
QList<NvidiaEncoder> nvidiaEncodersOf(QString *note)
{
    QList<NvidiaEncoder> result;
    if constexpr (kpipewireHasNvidia<Stream>()) {
        for (const auto &info : Stream::nvidiaEncoders()) {
            NvidiaEncoder out;
            out.pciId = info.pciId;
            out.name = info.name;
            out.cudaOrdinal = info.cudaOrdinal;
            out.family = info.encoder == Stream::HEVCMain ? Family::Hevc : info.encoder == Stream::AV1Main ? Family::Av1 : Family::Avc;
            out.usable = info.usable;
            out.maxSize = info.maxSize;
            out.failure = info.failure;
            result.append(out);
        }
        if (result.isEmpty()) {
            *note = Stream::nvidiaUnavailableReason();
        }
    } else {
        *note = QStringLiteral("this KPipeWire has no NVIDIA probe");
    }
    return result;
}

template<typename Stream>
constexpr bool kpipewireHasChroma444()
{
    return requires { typename Stream::ChromaMode; };
}

/**
 * Software encoders KPipeWire's makeEncoder() can open: H.264 = libx264 or libopenh264 (its
 * fallback after h264_vaapi); HEVC = libx265 and AV1 = libsvtav1 (or libaom-av1) with a
 * KPipeWire that has the WS-E software path, which reports them itself.
 */
template<typename Stream>
bool softwareBackend(Family family)
{
    if (family == Family::Avc) {
        return hasEncoder("libx264") || hasEncoder("libopenh264");
    }
    if constexpr (kpipewireHasBackends<Stream>()) {
        const auto encoder = encoderOf<Stream>(family);
        return encoder && bool(Stream::availableEncoderBackends(*encoder) & Stream::EncoderBackend::Software);
    }
    return false; // no software HEVC/AV1 in this KPipeWire
}

/**
 * Whether the software encoder of \a family changes its target bitrate in place (no reopen):
 * KPipeWire's softwareBitrateChangeIsLive() (AUD-SWENC: libx264, libx265). False with a KPipeWire
 * without it, where every software HEVC/AV1 bitrate change reopened the encoder.
 */
template<typename Stream>
bool liveBitrateChange(Family family)
{
    if constexpr (requires(typename Stream::Encoder e) { Stream::softwareBitrateChangeIsLive(e); }) {
        const auto encoder = encoderOf<Stream>(family);
        return encoder && Stream::softwareBitrateChangeIsLive(*encoder);
    }
    return false;
}

std::optional<Backends> parseBackends(const QString &value)
{
    if (value == QLatin1String("none")) return Backends{};
    if (value == QLatin1String("hw")) return Backends{true, false};
    if (value == QLatin1String("sw")) return Backends{false, true};
    if (value == QLatin1String("hw+sw") || value == QLatin1String("sw+hw")) return Backends{true, true};
    return std::nullopt;
}
}

bool softwareForced()
{
    const QByteArray value = qgetenv("FARSIDE_FORCE_SOFTWARE_ENCODING");
    return !value.isEmpty() && value != "0";
}

void applyProcessOverrides()
{
    if (!softwareForced() || qEnvironmentVariableIsSet("KPIPEWIRE_FORCE_ENCODER")) {
        return;
    }
    const QByteArray encoder = hasEncoder("libx264") ? QByteArrayLiteral("libx264") : QByteArrayLiteral("libopenh264");
    qputenv("KPIPEWIRE_FORCE_ENCODER", encoder);
    qCInfo(KRDP) << "FARSIDE_FORCE_SOFTWARE_ENCODING: KPipeWire H.264 forced to" << encoder;
}

bool applyOverride(Encoders &encoders, const QString &spec)
{
    Encoders result = encoders;
    for (const QString &item : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const auto parts = item.trimmed().split(QLatin1Char('='));
        if (parts.size() != 2) return false;
        const auto backends = parseBackends(parts.at(1).trimmed().toLower());
        if (!backends) return false;
        const QString codec = parts.at(0).trimmed().toLower();
        Backends *target = codec == QLatin1String("avc") ? &result.avc
            : codec == QLatin1String("hevc")             ? &result.hevc
            : codec == QLatin1String("av1")              ? &result.av1
                                                         : nullptr;
        if (!target) return false;
        const bool live = target->liveBitrate; // a property of the encoder, not of the override
        *target = *backends;
        target->liveBitrate = live;
    }
    encoders = result;
    return true;
}

namespace
{
bool nvidiaHas(const QList<NvidiaEncoder> &list, Family family)
{
    return std::any_of(list.begin(), list.end(), [family](const NvidiaEncoder &e) {
        return e.family == family && e.usable;
    });
}
}

Probe assemble(const Inputs &in)
{
    Probe result;
    result.nvidia = in.nvidia;
    result.nvidiaNote = in.nvidiaNote;
    const std::array<Family, 3> families{Family::Avc, Family::Hevc, Family::Av1};
    for (std::size_t i = 0; i < families.size(); ++i) {
        const Family family = families[i];
        const bool vaapi = !in.forcedSoftware && (family == Family::Avc ? in.vaapi.avc : family == Family::Hevc ? in.vaapi.hevc : in.vaapi.av1);
        // NVENC only supplies a codec VA-API cannot: the capture GPU stays the encoder where it can (Hal's AMD),
        // and the NVIDIA GPU is not touched for it (device policy and load balancing are a later slice).
        const bool nvenc = !in.forcedSoftware && !vaapi && nvidiaHas(in.nvidia, family);
        Backends &b = result.encoders.of(family);
        b.hardware = (vaapi || nvenc) && in.kpipewireHardware[i];
        b.software = in.software[i];
        b.liveBitrate = b.software && in.liveBitrate[i];
        if (b.hardware) {
            (family == Family::Avc ? result.avcHardwareVia : family == Family::Hevc ? result.hevcHardwareVia : result.av1HardwareVia) = vaapi ? QStringLiteral("vaapi") : QStringLiteral("nvenc");
        }
    }
    for (std::size_t i = 0; i < families.size(); ++i) {
        const Family family = families[i];
        result.labels.software[i] = {in.softwareBackend[i], {}, {}};
        const QString via = result.hardwareVia(family);
        if (via.isEmpty()) continue;
        CodecPolicy::EncoderLabel label{via, {}, {}};
        if (via == QLatin1String("nvenc")) {
            const auto found = std::find_if(in.nvidia.begin(), in.nvidia.end(), [family](const NvidiaEncoder &e) { return e.family == family && e.usable; });
            if (found != in.nvidia.end()) {
                label.device = found->pciId;
                label.deviceName = found->name;
            }
        }
        result.labels.hardware[i] = label;
    }
    result.avc444Hardware = in.chroma444 && result.encoders.avc.hardware && result.avcHardwareVia == QLatin1String("vaapi");
    result.renderNode = in.forcedSoftware ? QString() : in.vaapi.node;
    if (result.renderNode.isEmpty() && !in.forcedSoftware) {
        for (const auto &e : in.nvidia) {
            if (e.usable && result.hardwareVia(e.family) == QLatin1String("nvenc")) {
                result.renderNode = QStringLiteral("NVENC %1 %2").arg(e.name, e.pciId);
                break;
            }
        }
    }
    if (!in.overrideSpec.isEmpty()) {
        if (applyOverride(result.encoders, in.overrideSpec)) {
            result.avc444Hardware = result.avc444Hardware && result.encoders.avc.hardware;
            qCInfo(KRDP) << "FARSIDE_ENCODERS override:" << in.overrideSpec;
        } else {
            qCWarning(KRDP) << "Ignoring an invalid FARSIDE_ENCODERS value:" << in.overrideSpec;
        }
    }
    return result;
}

Probe probeUncached()
{
    QElapsedTimer timer;
    timer.start();
    // AUD-TESTFIX: probe with the VAAPI driver the encoders will use. Without
    // LIBVA_DRIVER_NAME on a mixed AMD + NVIDIA host (Hal), libva loads the decode-only
    // nvidia driver for the NVIDIA node, which answers vaGetConfigAttributes(EncSlice)
    // with success and leaves the value unset; KPipeWire's VaapiUtils reads that
    // uninitialised value and so picks its render node at random (then AV1/HEVC hardware
    // came and went between runs). selectVaapiDriver() sets radeonsi there, as krdpserver
    // and the worker do at start-up; the virtual and console hosts did not, until now.
    // Also when software is forced: KPipeWire's VaapiUtils is a per-process singleton that
    // this probe creates (suggestedEncoders()), and later encoders and probes reuse it.
    selectVaapiDriver();
    Inputs in;
    in.forcedSoftware = softwareForced();
    const Hardware hw = in.forcedSoftware ? Hardware{} : probeHardware();
    in.vaapi = {hw.avc, hw.hevc, hw.av1, hw.node};
    // NVENC supplies the codecs VA-API lacks (Sol's NVIDIA worker has no VA-API encoder at all). When VA-API
    // covers every codec (Hal) the NVIDIA stack is not touched.
    if (!in.forcedSoftware && !(hw.avc && hw.hevc && hw.av1)) {
        in.nvidia = nvidiaEncodersOf<PipeWireEncodedStream>(&in.nvidiaNote);
    }

    PipeWireEncodedStream stream;
    const auto suggested = stream.suggestedEncoders();
    const bool avcOffered = suggested.contains(PipeWireEncodedStream::H264Main) || suggested.contains(PipeWireEncodedStream::H264Baseline);
    const std::array<Family, 3> families{Family::Avc, Family::Hevc, Family::Av1};
    for (std::size_t i = 0; i < families.size(); ++i) {
        const Family family = families[i];
        in.kpipewireHardware[i] = kpipewireOffersHardware<PipeWireEncodedStream>(suggested, family);
        in.software[i] = (family == Family::Avc ? avcOffered : true) && softwareBackend<PipeWireEncodedStream>(family);
        in.liveBitrate[i] = liveBitrateChange<PipeWireEncodedStream>(family);
    }
    in.softwareBackend[0] = hasEncoder("libx264") ? QStringLiteral("libx264") : QStringLiteral("libopenh264");
    in.softwareBackend[2] = hasEncoder("libsvtav1") ? QStringLiteral("libsvtav1") : QStringLiteral("libaom-av1");
    in.chroma444 = kpipewireHasChroma444<PipeWireEncodedStream>();
    in.overrideSpec = qEnvironmentVariable("FARSIDE_ENCODERS");
    Probe result = assemble(in);
    qCInfo(KRDP).noquote() << QStringLiteral("Video encoders: %1 (probed in %2 ms)").arg(describe(result)).arg(timer.elapsed());
    if (!result.nvidiaNote.isEmpty() && result.nvidia.isEmpty()) {
        qCInfo(KRDP).noquote() << QStringLiteral("NVIDIA encoders unavailable: %1").arg(result.nvidiaNote);
    }
    return result;
}

const Probe &probe()
{
    static std::once_flag once;
    static Probe cached;
    std::call_once(once, [] {
        cached = probeUncached();
    });
    return cached;
}

LayoutControl::VideoCapabilities videoCapabilities(const Probe &probe, CodecPolicy::SoftwareEncoding mode)
{
    LayoutControl::VideoCapabilities video;
    video.softwareEncoding = QString::fromLatin1(CodecPolicy::softwareEncodingName(mode));
    const auto &e = probe.encoders;
    video.codecs.append({QStringLiteral("avc420"), e.avc.hardware, e.avc.software});
    if (probe.avc444Hardware) {
        video.codecs.append({QStringLiteral("avc444"), true, false});
    }
    const bool softwareUsable = mode != CodecPolicy::SoftwareEncoding::Never;
    const auto offer = [&](const char *name, const Backends &b) {
        if (b.hardware || (softwareUsable && b.software)) {
            video.codecs.append({QString::fromLatin1(name), b.hardware, softwareUsable && b.software});
        }
    };
    offer("hevc", e.hevc);
    offer("av1", e.av1);
    return video;
}

LayoutControl::VideoCapabilities videoCapabilities(const Probe &probe, CodecPolicy::SoftwareEncoding mode, const CodecPolicy::SoftwareAllowance &allowance)
{
    LayoutControl::VideoCapabilities video;
    video.softwareEncoding = QString::fromLatin1(CodecPolicy::softwareEncodingName(mode));
    video.preferences = 1;
    video.softwareAvc = QString::fromLatin1(CodecPolicy::allowanceName(Family::Avc, allowance.avc));
    video.softwareHevc = QString::fromLatin1(CodecPolicy::allowanceName(Family::Hevc, allowance.hevc));
    video.softwareAv1 = QString::fromLatin1(CodecPolicy::allowanceName(Family::Av1, allowance.av1));
    const auto &e = probe.encoders;
    // AVC420 is always offered: software H.264 is the last resort even when the ceiling says so.
    video.codecs.append({QStringLiteral("avc420"), e.avc.hardware, e.avc.software});
    if (probe.avc444Hardware) {
        video.codecs.append({QStringLiteral("avc444"), true, false});
    }
    const auto offer = [&](const char *name, const Backends &b, bool softwareUsable) {
        if (b.hardware || (softwareUsable && b.software)) {
            video.codecs.append({QString::fromLatin1(name), b.hardware, softwareUsable && b.software});
        }
    };
    offer("hevc", e.hevc, allowance.hevc);
    offer("av1", e.av1, allowance.av1);
    for (const Family family : {Family::Avc, Family::Hevc, Family::Av1}) {
        const Backends &b = e.of(family);
        const QString codec = QString::fromLatin1(CodecPolicy::familyName(family));
        if (b.hardware) {
            const auto &label = probe.labels.of(family, true);
            video.encoders.append({codec, label.backend, true, label.device, label.deviceName});
        }
        if (b.software && (family == Family::Avc || allowance.allows(family))) {
            video.encoders.append({codec, probe.labels.of(family, false).backend, false, {}, {}});
        }
    }
    return video;
}

QString describe(const Probe &probe)
{
    const auto one = [](const char *name, const Backends &b, const QString &via) {
        const QString hw = via == QLatin1String("nvenc") ? QStringLiteral("hw(nvenc)") : QStringLiteral("hw");
        const QString backends = b.hardware && b.software ? hw + QStringLiteral("+sw")
            : b.hardware                                  ? hw
            : b.software                                  ? QStringLiteral("sw")
                                                          : QStringLiteral("none");
        return QStringLiteral("%1 %2").arg(QLatin1String(name), backends);
    };
    QString text = QStringList{one("avc", probe.encoders.avc, probe.avcHardwareVia), one("hevc", probe.encoders.hevc, probe.hevcHardwareVia), one("av1", probe.encoders.av1, probe.av1HardwareVia)}.join(QStringLiteral(", "));
    text += probe.avc444Hardware ? QStringLiteral(", avc444 hw") : QStringLiteral(", avc444 none");
    if (!probe.renderNode.isEmpty()) {
        text += QStringLiteral(" on %1").arg(probe.renderNode);
    }
    if (softwareForced()) {
        text += QStringLiteral(" (FARSIDE_FORCE_SOFTWARE_ENCODING)");
    }
    return text;
}
}
