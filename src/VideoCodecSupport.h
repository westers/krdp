#pragma once
#include <QMetaType>
#include <QString>
#include <QStringView>
#include <cstdint>
#include <optional>
#include <freerdp/channels/rdpgfx.h>
namespace KRdp
{
enum class VideoCodec { Avc420, Avc444, Avc444v2, Hevc, Av1 };
enum class CodecPreference { Auto, Avc420, Avc444 };
/// Per-second encoder cost report, mirrored from the private KPipeWire's ChromaTiming (microseconds).
struct ChromaTimingReport {
    int frames = 0, auxSent = 0, auxSkippedMotion = 0, auxRestRefresh = 0, rewriteFailures = 0;
    QString splitVariant;
    qint64 downloadAvg = 0, downloadMax = 0, splitAvg = 0, splitMax = 0, uploadAvg = 0, uploadMax = 0;
    qint64 encodeMainAvg = 0, encodeMainMax = 0, encodeAuxAvg = 0, encodeAuxMax = 0;   // queued -> packet latency per context
};
/**
 * AVC444 aux (chroma) stream timing, per connection (OPT-045b, design §10 A10.1-A10.4). Mirrors the
 * private KPipeWire's `PipeWireBaseEncodedStream::ChromaPolicy` field-for-field without exposing that
 * type outside KRdp, so the fork still links against a stock KPipeWire (AbstractSession::setChromaPolicy
 * forwards it behind an `if constexpr (requires ...)` check the same way setChromaEnabled does).
 */
struct ChromaPolicy {
    int motionGapMs = 100;
    int restMs = 150;
    int maxGapMs = 1500;

    bool operator==(const ChromaPolicy &) const = default;

    /// Each field in [16, 5000] and motionGapMs <= restMs <= maxGapMs - the same rule KPipeWire's
    /// H264VAAPIAvc444Encoder enforces on its own copy; checked here too so the server and KRDPCTL can
    /// refuse an out-of-range request before it ever reaches KPipeWire.
    bool isValid() const
    {
        constexpr int kMinMs = 16;
        constexpr int kMaxMs = 5000;
        const auto inRange = [](int v) { return v >= kMinMs && v <= kMaxMs; };
        return inRange(motionGapMs) && inRange(restMs) && inRange(maxGapMs) && motionGapMs <= restMs && restMs <= maxGapMs;
    }
};
namespace VideoCodecSupport
{
inline std::optional<CodecPreference> parseCodecPreference(QStringView value)
{
    const QString v = value.trimmed().toString().toLower();
    if (v == QLatin1String("auto")) return CodecPreference::Auto;
    if (v == QLatin1String("avc420")) return CodecPreference::Avc420;
    if (v == QLatin1String("avc444")) return CodecPreference::Avc444;
    return std::nullopt;
}
inline const char *preferenceName(CodecPreference p) { switch (p) { case CodecPreference::Auto: return "auto"; case CodecPreference::Avc420: return "avc420"; case CodecPreference::Avc444: return "avc444"; } return "?"; }
inline const char *codecName(VideoCodec c) { switch (c) { case VideoCodec::Avc420: return "avc420"; case VideoCodec::Avc444: return "avc444"; case VideoCodec::Avc444v2: return "avc444v2"; case VideoCodec::Hevc: return "hevc"; case VideoCodec::Av1: return "av1"; } return "?"; }
inline bool isAvc444(VideoCodec c) { return c != VideoCodec::Avc420; }
constexpr uint16_t PrivateHevcCodecId = 0x8001;
constexpr uint16_t PrivateAv1CodecId = 0x8002;
inline uint16_t rdpgfxCodecId(VideoCodec c) { switch (c) { case VideoCodec::Avc420: return RDPGFX_CODECID_AVC420; case VideoCodec::Avc444: return RDPGFX_CODECID_AVC444; case VideoCodec::Avc444v2: return RDPGFX_CODECID_AVC444v2; case VideoCodec::Hevc: return PrivateHevcCodecId; case VideoCodec::Av1: return PrivateAv1CodecId; } return RDPGFX_CODECID_AVC420; }
/// What the selected caps set allows (MS-RDPEGFX 2.2.3.3-2.2.3.10: AVC444 from version 10.0, AVC444v2 from 10.2, both gated by AVC_DISABLED).
inline bool clientSupportsAvc444(uint32_t capsVersion, uint32_t capsFlags) { return capsVersion >= RDPGFX_CAPVERSION_10 && !(capsFlags & RDPGFX_CAPS_FLAG_AVC_DISABLED); }
inline bool clientSupportsAvc444v2(uint32_t capsVersion, uint32_t capsFlags) { return capsVersion >= RDPGFX_CAPVERSION_102 && !(capsFlags & RDPGFX_CAPS_FLAG_AVC_DISABLED); }
inline VideoCodec codecFor(uint32_t capsVersion, uint32_t capsFlags, CodecPreference preference)
{
    if (preference == CodecPreference::Avc420) return VideoCodec::Avc420;
    if (clientSupportsAvc444v2(capsVersion, capsFlags)) return VideoCodec::Avc444v2;
    if (clientSupportsAvc444(capsVersion, capsFlags)) return VideoCodec::Avc444;
    return VideoCodec::Avc420;
}
/// The codec sessions are built for before the client's caps are known.
inline VideoCodec expectedCodec(CodecPreference preference) { return preference == CodecPreference::Avc420 ? VideoCodec::Avc420 : VideoCodec::Avc444v2; }
}
}
Q_DECLARE_METATYPE(KRdp::VideoCodec)
Q_DECLARE_METATYPE(KRdp::ChromaTimingReport)
Q_DECLARE_METATYPE(KRdp::ChromaPolicy)
