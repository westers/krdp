// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "EncoderSupport.h"

#include "krdp_logging.h"

#include <PipeWireEncodedStream>

#include <QDir>
#include <QElapsedTimer>

#include <mutex>

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
Hardware probeHardware()
{
    Hardware hw;
    if (!hasEncoder("h264_vaapi")) {
        return hw;
    }
    const int previousLevel = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET); // a failing trial is expected, not an error
    const auto nodes = QDir(QStringLiteral("/dev/dri")).entryList({QStringLiteral("renderD*")}, QDir::System, QDir::Name);
    for (const QString &name : nodes) {
        const QByteArray path = QByteArrayLiteral("/dev/dri/") + name.toLatin1();
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

/// What this KPipeWire build can be asked for (a stock KPipeWire has no HEVC/AV1 at all).
template<typename Stream>
Backends kpipewireOffers(const QList<typename Stream::Encoder> &suggested, Family family)
{
    Backends offered;
    switch (family) {
    case Family::Avc:
        offered.hardware = offered.software = suggested.contains(Stream::H264Main) || suggested.contains(Stream::H264Baseline);
        break;
    case Family::Hevc:
        if constexpr (requires { Stream::HEVCMain; }) {
            offered.hardware = suggested.contains(Stream::HEVCMain);
        }
        break;
    case Family::Av1:
        if constexpr (requires { Stream::AV1Main; }) {
            offered.hardware = suggested.contains(Stream::AV1Main);
        }
        break;
    }
    return offered;
}

template<typename Stream>
constexpr bool kpipewireHasChroma444()
{
    return requires { typename Stream::ChromaMode; };
}

/**
 * Software encoders KPipeWire's makeEncoder() falls back to. The hook for software HEVC
 * (libx265) and AV1 (SVT-AV1): report them here once KPipeWire can encode them.
 */
bool softwareBackend(Family family)
{
    switch (family) {
    case Family::Avc:
        return hasEncoder("libx264") || hasEncoder("libopenh264");
    case Family::Hevc:
    case Family::Av1:
        return false; // not in KPipeWire yet
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
    const QByteArray value = qgetenv("KRDP_FORCE_SOFTWARE_ENCODING");
    return !value.isEmpty() && value != "0";
}

void applyProcessOverrides()
{
    if (!softwareForced() || qEnvironmentVariableIsSet("KPIPEWIRE_FORCE_ENCODER")) {
        return;
    }
    const QByteArray encoder = hasEncoder("libx264") ? QByteArrayLiteral("libx264") : QByteArrayLiteral("libopenh264");
    qputenv("KPIPEWIRE_FORCE_ENCODER", encoder);
    qCInfo(KRDP) << "KRDP_FORCE_SOFTWARE_ENCODING: KPipeWire H.264 forced to" << encoder;
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
        if (codec == QLatin1String("avc")) result.avc = *backends;
        else if (codec == QLatin1String("hevc")) result.hevc = *backends;
        else if (codec == QLatin1String("av1")) result.av1 = *backends;
        else return false;
    }
    encoders = result;
    return true;
}

Probe probeUncached()
{
    QElapsedTimer timer;
    timer.start();
    Probe result;
    const bool forced = softwareForced();
    const Hardware hw = forced ? Hardware{} : probeHardware();

    PipeWireEncodedStream stream;
    const auto suggested = stream.suggestedEncoders();
    for (const Family family : CodecPolicy::BestCompressionFirst) {
        const Backends offered = kpipewireOffers<PipeWireEncodedStream>(suggested, family);
        const bool hardware = family == Family::Avc ? hw.avc : family == Family::Hevc ? hw.hevc : hw.av1;
        Backends &b = result.encoders.of(family);
        b.hardware = offered.hardware && hardware;
        b.software = (family == Family::Avc ? offered.software : true) && softwareBackend(family);
    }
    result.avc444Hardware = kpipewireHasChroma444<PipeWireEncodedStream>() && result.encoders.avc.hardware;
    result.renderNode = hw.node;

    const QString spec = qEnvironmentVariable("KRDP_ENCODERS");
    if (!spec.isEmpty()) {
        if (applyOverride(result.encoders, spec)) {
            result.avc444Hardware = result.avc444Hardware && result.encoders.avc.hardware;
            qCInfo(KRDP) << "KRDP_ENCODERS override:" << spec;
        } else {
            qCWarning(KRDP) << "Ignoring an invalid KRDP_ENCODERS value:" << spec;
        }
    }
    qCInfo(KRDP).noquote() << QStringLiteral("Video encoders: %1 (probed in %2 ms)").arg(describe(result)).arg(timer.elapsed());
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

QString describe(const Probe &probe)
{
    const auto one = [](const char *name, const Backends &b) {
        const QString backends = b.hardware && b.software ? QStringLiteral("hw+sw")
            : b.hardware                                  ? QStringLiteral("hw")
            : b.software                                  ? QStringLiteral("sw")
                                                          : QStringLiteral("none");
        return QStringLiteral("%1 %2").arg(QLatin1String(name), backends);
    };
    QString text = QStringList{one("avc", probe.encoders.avc), one("hevc", probe.encoders.hevc), one("av1", probe.encoders.av1)}.join(QStringLiteral(", "));
    text += probe.avc444Hardware ? QStringLiteral(", avc444 hw") : QStringLiteral(", avc444 none");
    if (!probe.renderNode.isEmpty()) {
        text += QStringLiteral(" on %1").arg(probe.renderNode);
    }
    if (softwareForced()) {
        text += QStringLiteral(" (KRDP_FORCE_SOFTWARE_ENCODING)");
    }
    return text;
}
}
