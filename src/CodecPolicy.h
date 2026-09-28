// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>

/**
 * Which video codec, and which encoder backend, a connection that asked for the private
 * codecs gets (AUD-FIX2, `SoftwareEncoding`; KRDPCTL-V2-CONTRACT.md "video"). Pure: VideoStream
 * feeds it the encoders this host really has, what the client decodes, the measured link and
 * the software encoder's cost, and applies the answer. Stock clients never get here: they
 * decode the AVC codec their RDPGFX caps select, whatever backend KPipeWire picks for it.
 */
namespace KRdp::CodecPolicy
{
using Clock = std::chrono::steady_clock;

/// krdpserverrc `SoftwareEncoding`.
enum class SoftwareEncoding {
    Auto, ///< hardware codecs; the best-compressing one, even in software, only on a slow link
    Never, ///< hardware codecs only; H.264 in software only as the last resort
    Prefer, ///< always the best-compressing codec, software included (CPU guard permitting)
};

inline std::optional<SoftwareEncoding> parseSoftwareEncoding(QStringView value)
{
    const QString v = value.trimmed().toString().toLower();
    if (v == QLatin1String("auto")) return SoftwareEncoding::Auto;
    if (v == QLatin1String("never")) return SoftwareEncoding::Never;
    if (v == QLatin1String("prefer")) return SoftwareEncoding::Prefer;
    return std::nullopt;
}
inline const char *softwareEncodingName(SoftwareEncoding mode)
{
    switch (mode) {
    case SoftwareEncoding::Auto: return "auto";
    case SoftwareEncoding::Never: return "never";
    case SoftwareEncoding::Prefer: return "prefer";
    }
    return "?";
}

/// Codec families, in ascending order of compression. Avc = the RDPGFX AVC codec (420/444/444v2)
/// the client's caps select; Hevc/Av1 = the private codecs (0x8001/0x8002).
enum class Family { Avc = 0, Hevc = 1, Av1 = 2 };
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

struct Choice {
    Family family = Family::Avc;
    bool hardware = false;
    bool operator==(const Choice &) const = default;
};

/// Speed step of a software HEVC/AV1 encoder (KPipeWire `SoftwarePreset`): x265 veryfast /
/// superfast / ultrafast, SVT-AV1 M10 / M11 / M12 (WS-E PERF.md).
enum class Preset { Efficient = 0, Balanced = 1, Fastest = 2 };
inline const char *presetName(Preset p)
{
    switch (p) {
    case Preset::Efficient: return "efficient";
    case Preset::Balanced: return "balanced";
    case Preset::Fastest: return "fastest";
    }
    return "?";
}
/// The encoder setting \a p really selects for \a f: SVT-AV1 2.3 maps preset 12 to 11 in low-delay
/// mode, so AV1 Fastest is the same encoder as Balanced (PERF.md). libx264 has no preset step here
/// (KRdp always runs it at ultrafast), so AVC has one level.
inline int presetLevel(Family f, Preset p)
{
    if (f == Family::Avc) return 0;
    if (f == Family::Av1 && p == Preset::Fastest) return int(Preset::Balanced);
    return int(p);
}
/// The next faster preset that changes the encoder of \a f, skipping a step that does nothing.
inline std::optional<Preset> nextPreset(Family f, Preset p)
{
    for (int next = int(p) + 1; next <= int(Preset::Fastest); ++next) {
        if (presetLevel(f, Preset(next)) != presetLevel(f, p)) return Preset(next);
    }
    return std::nullopt;
}
/// The next slower (better-compressing) preset that changes the encoder of \a f, skipping a
/// step that does nothing (AV1 Fastest -> Balanced is the same SVT-AV1 preset, so -> Efficient).
inline std::optional<Preset> previousPreset(Family f, Preset p)
{
    for (int previous = int(p) - 1; previous >= int(Preset::Efficient); --previous) {
        if (presetLevel(f, Preset(previous)) != presetLevel(f, p)) return Preset(previous);
    }
    return std::nullopt;
}

/**
 * What the running encoder is told besides its codec (WS-E): the backend, the software preset,
 * the target bitrate (0 = quality mode; software HEVC/AV1 always run in bitrate mode, the target
 * following adaptive quality, see qualityKbps()) and a frame-rate cap (0 = none).
 */
struct EncoderSettings {
    bool hardware = false;
    Preset preset = Preset::Efficient;
    quint32 targetKbps = 0;
    int maxFrameRate = 0;
    bool operator==(const EncoderSettings &) const = default;
};

// Link thresholds for a 1920x1080 stream, scaled with the pixel count (never below a tenth).
constexpr double SlowBelowMbps1080p = 15.0;
constexpr double FastAboveMbps1080p = 25.0;
constexpr qint64 ReferencePixels = 1920LL * 1080LL;
/// How long the link must look slow without a break (or fast again) before the state flips.
constexpr auto LinkHold = std::chrono::seconds(5);
/**
 * AUD-FIX5 D2: the slow-link judgement over a sliding window. Since the AUD-FIX4 D1 in-flight cap,
 * a saturated link no longer stays congested: the cap drains the backlog and adaptive quality
 * drops the bitrate, so congestion clears within 1.5-6 s and comes back 9-14 s later (Sol, 6 Mbit/s
 * tbf, 2026-09-28: congested in 20-40 % of the 1.5 s samples, adaptive quality swinging 30-65
 * under its cap of 75, goodput 2-4.7 Mbit/s). "LinkHold without a break" never saw that. Over the
 * last SlowWindow, with the goodput of every sample under the slow threshold (the link delivers
 * less than a normal link would even while saturated), the link is slow when
 * - it was congested in at least SlowCongestedShare of the samples, or
 * - congestion came back (at least SlowEpisodes separate congested runs) while adaptive quality
 *   never got back to its cap (the link cannot carry what is asked for), or
 * - adaptive quality sat at its floor (SlowQualityFloor or less, under its cap) and was still hit
 *   by congestion.
 * A normal link with a congestion blip now and then gets back to its cap between blips, and its
 * goodput under motion passes the threshold. The old rule (LinkHold of unbroken congestion) still
 * applies. The window is judged once its oldest sample is SlowWindowFull old (samples come every
 * 1.5 s) and starts empty after every change of the link state.
 */
constexpr auto SlowWindow = std::chrono::seconds(15);
constexpr auto SlowWindowFull = std::chrono::milliseconds(13000);
constexpr double SlowCongestedShare = 0.6;
constexpr int SlowEpisodes = 2;
constexpr int SlowQualityFloor = 15; ///< AdaptiveQuality::MinQuality plus one step up
/// No two switches closer than this (the first choice is not a switch).
constexpr auto MinSwitchInterval = std::chrono::seconds(10);
/// The software encoder's p95 per-frame encode time must stay under this share of the frame budget.
constexpr double CpuGuardLimit = 0.70;
/**
 * A software backend the guard stepped away from is not picked again for CpuBlockFor, doubled for
 * every earlier time the guard stepped away from it (AUD-FIX5 D5: 5, 10, 20, 40, then 60 min).
 * With a fixed 120 s, full-screen motion on Sol cycled AV1 -> HEVC -> AVC every 2 min, each
 * cycle three encoder reopens and keyframes. When the guard steps away from a codec it had
 * already stepped away from before, the other codecs it rejected earlier are held back as long,
 * so a failed retry falls straight through to the codec that carried the load. The count starts
 * over (GuardForgiveAfter) once the running software encoder has stayed under PresetRecoverBelow
 * of its frame budget for that long: the content or the load really got lighter.
 */
constexpr auto CpuBlockFor = std::chrono::minutes(5);
constexpr auto CpuBlockMax = std::chrono::minutes(60);
constexpr auto GuardForgiveAfter = std::chrono::minutes(5);
inline std::chrono::seconds guardBlockFor(int earlierRejections)
{
    auto block = std::chrono::duration_cast<std::chrono::seconds>(CpuBlockFor);
    for (int i = 0; i < earlierRejections && block < CpuBlockMax; ++i) {
        block *= 2;
    }
    return std::min(block, std::chrono::duration_cast<std::chrono::seconds>(CpuBlockMax));
}

/**
 * A live preset or bitrate change on software HEVC/AV1 reopens the encoder: one stall of about
 * this long (PERF.md, Sol: x265 38-48 ms, SVT-AV1 145-148 ms) plus a keyframe. libx264 changes
 * its bitrate in place.
 */
inline std::chrono::milliseconds reopenStall(Family f)
{
    switch (f) {
    case Family::Hevc: return std::chrono::milliseconds(45);
    case Family::Av1: return std::chrono::milliseconds(150);
    case Family::Avc: break;
    }
    return std::chrono::milliseconds(0);
}
/// No two encoder reconfigurations (preset step, bitrate change, guard frame-rate step) closer
/// than this, like codec switches: an AV1 reopen then costs at most 1.5 % of the time.
constexpr auto MinReconfigureInterval = std::chrono::seconds(10);
static_assert(std::chrono::milliseconds(150) * 50 <= MinReconfigureInterval, "a reopen stall must stay under 2 % of the interval");
/// No software HEVC/AV1 preset sustains 1080p60 full-screen motion (PERF.md): cap them at 30 fps.
constexpr int SoftwarePrivateMaxFrameRate = 30;
/// The frame rate sessions run at without a cap (AbstractSession's default).
constexpr int DefaultFrameRate = 60;
/// The CPU guard's last step halves the frame rate, never below this.
constexpr int MinFrameRate = 15;
/// The guard steps a software preset back up (slower, better compression) once the p95 load has
/// stayed under this share of the frame budget for PresetRecoverHold. Hysteresis against
/// CpuGuardLimit: a slower preset costs up to ~1.6x (PERF.md: x265 superfast -> veryfast), so
/// 0.40 lands at most around 0.64, still under the 0.70 limit.
constexpr double PresetRecoverBelow = 0.40;
constexpr auto PresetRecoverHold = std::chrono::seconds(30);
/// A preset the guard had to step down again soon after raising it (within this long) is not
/// raised again for this long, doubled for every further such flap (up to 8x): a load that
/// only the faster preset can carry settles on it instead of flapping.
constexpr auto PresetFlapWindow = std::chrono::seconds(120);
constexpr int PresetFlapMaxDoublings = 3;
/// Slow-link cap on the target bitrate: this share of the measured goodput while congested, at
/// least MinTargetKbps, at most the slow-link threshold (the probe ceiling); it moves only by at
/// least TargetMinChange and at most once per MinReconfigureInterval.
constexpr double TargetShareOfGoodput = 0.85;
constexpr quint32 MinTargetKbps = 300;
constexpr double TargetMinChange = 0.15;
/**
 * AUD-FIX4 D2: the way back from a slow link. Goodput only counts what the server sends, and a
 * software HEVC/AV1 target never exceeded the slow threshold (at the default quality 75 it is
 * 5.2 Mbit/s at 1080p), so "goodput above FastAboveMbps" could not happen and the slow-link
 * choice was one-way. A stream that only sends what its quality asks for proves nothing about
 * the link either: 5.2 Mbit/s fits a 6 Mbit/s throttle and a gigabit LAN alike. So on a slow link
 * the software encoder's target *is* the probed link cap, and the cap is probed upward:
 * - "clear" = no congestion signal at all: no RTT inflation, the client never several frames
 *   behind, and the in-flight window never full (VideoStream's window pressure). While clear the
 *   target is the cap; under congestion it is also held under adaptive quality's bitrate, so it
 *   drops at once where the encoder changes its bitrate live;
 * - "sending at the cap" = clear, the encoder runs at (about) the cap, and the goodput is at
 *   least ProbeFillShare of its target: the content really uses what the link is given;
 * - after ProbeHold sending at the cap, the cap grows by ProbeGrowth (an up-probe, the target
 *   with it). Congestion drops it to TargetShareOfGoodput of the goodput; congestion soon after
 *   a probe is a failed probe, and each one doubles the next ProbeHold (up to
 *   2^ProbeMaxDoublings), so a link that really is slow is only probed now and then;
 * - once the cap is at the probe ceiling (slowBelowKbps) and the stream kept sending at it for
 *   RecoverHold, the link has shown sustained headroom: back to the normal choice (the hardware
 *   or preferred codec). Without a software HEVC/AV1 encoder to probe with (the slow-link choice
 *   was AVC anyway), RecoverHold clear is enough.
 * Content that never fills the cap (a still desktop) proves nothing and keeps the slow-link
 * codec, which costs little on such content; the first motion that fills it resumes the probe.
 * The old rule (goodput above FastAboveMbps for LinkHold) still recovers at once.
 * Anti-flap: switches stay MinSwitchInterval apart, and a slow link that comes back within
 * RecoverFlapWindow of a recovery doubles the next RecoverHold (up to 2^RecoverFlapMaxDoublings).
 */
constexpr auto ProbeHold = std::chrono::seconds(10);
constexpr double ProbeGrowth = 1.5;
constexpr double ProbeFillShare = 0.6;
constexpr int ProbeMaxDoublings = 3;
/// A cap this close to the ceiling counts as there (under 1 / (1 + TargetMinChange), so a cap
/// can never get stuck just below the ceiling).
constexpr double ProbeCeilingShare = 0.85;
/// The encoder "runs at the cap" when its target is at least this share of it.
constexpr double AtCapShare = 0.95;
constexpr auto RecoverHold = std::chrono::seconds(20);
constexpr auto RecoverFlapWindow = std::chrono::seconds(120);
constexpr int RecoverFlapMaxDoublings = 4;

/**
 * Adaptive quality on software HEVC/AV1 (AUD-SWENC): its quality steps become target-bitrate
 * changes instead of CRF changes, because a CRF change reopens the encoder (a stall and a
 * keyframe per step) while libx265 changes its bitrate in place. qualityKbps() is the mapping:
 * ReferenceBitsPerPixel at ReferenceQuality, halving every QualityPerHalving points (the VA-API /
 * x265 quality mapping moves the QP by 0.28 per point, and ~6 QP halve the bitrate), for the
 * software private-codec frame rate (SoftwarePrivateMaxFrameRate).
 */
constexpr int ReferenceQuality = 80;
constexpr double ReferenceBitsPerPixel = 0.10; ///< 1080p30: ~6.2 Mbit/s at quality 80
constexpr double QualityPerHalving = 20.0;
constexpr quint8 DefaultQuality = 80;
/// A live (in-place) bitrate change smaller than this is not worth sending.
constexpr double LiveBitrateMinChange = 0.05;
/**
 * Where a bitrate change reopens the encoder (Backends::liveBitrate false: SVT-AV1), a change
 * waits until the last reconfiguration is at least this old and must move the target by at least
 * RestartBitrateMinChange. AUD-FIX4 D4: the contract's MinReconfigureInterval (it was 5 s and
 * 20 %, and a swinging adaptive quality reopened SVT-AV1 every 6 s on Sol: 26 IDRs in 4.5 min).
 * A rise waits RestartBitrateRaiseInterval (drop fast, climb slowly); changes that come while one
 * waits merge into the one that is applied (the target is recomputed every step).
 */
constexpr auto RestartBitrateInterval = MinReconfigureInterval;
constexpr auto RestartBitrateRaiseInterval = std::chrono::seconds(20);
constexpr double RestartBitrateMinChange = 0.30;
static_assert(std::chrono::milliseconds(150) * 50 <= RestartBitrateInterval, "a bitrate reopen stall must stay under ~2 % of its interval");

inline double scale(qint64 pixels)
{
    return std::max(0.1, double(std::max<qint64>(pixels, 1)) / double(ReferencePixels));
}
inline double slowBelowKbps(qint64 pixels) { return SlowBelowMbps1080p * 1000.0 * scale(pixels); }
inline double fastAboveKbps(qint64 pixels) { return FastAboveMbps1080p * 1000.0 * scale(pixels); }

/// The target bitrate for adaptive quality \a quality (0-100) on \a pixels (all surfaces).
inline quint32 qualityKbps(quint8 quality, qint64 pixels)
{
    const double bpp = ReferenceBitsPerPixel * std::exp2((double(std::min<quint8>(quality, 100)) - ReferenceQuality) / QualityPerHalving);
    const double kbps = double(std::max<qint64>(pixels, 1)) * SoftwarePrivateMaxFrameRate * bpp / 1000.0;
    return quint32(std::max(kbps, double(MinTargetKbps)));
}

struct Input {
    SoftwareEncoding mode = SoftwareEncoding::Auto;
    Encoders encoders;
    QList<Family> client; ///< the private families the client decodes; Avc is always allowed
    bool adaptive = true; ///< false: the link never counts as slow (the client pinned its choice)
    std::optional<quint32> bandwidthKbps; ///< measured goodput (NetworkDetection), none yet = unknown
    bool congested = false; ///< RTT inflated or the client backlogged (AdaptiveQuality's signals)
    qint64 pixels = ReferencePixels; ///< all surfaces together
    /// The running *software* encoder's p95 per-frame encode time over its frame budget
    /// (1.0 = the whole budget). Unknown (or a hardware encoder): no CPU guard.
    std::optional<double> encodeLoadP95;
    /// The adaptive-quality value (the cap when adaptive quality is off); unknown = DefaultQuality.
    std::optional<quint8> quality;
    /// Adaptive quality's cap; unknown = the quality-based slow-link signals are off (SlowWindow).
    std::optional<quint8> qualityCap;
};

/// One step()'s view of the link, kept for the slow-link window (SlowWindow).
struct LinkSample {
    Clock::time_point at;
    bool congested = false;
    quint32 kbps = 0;
    bool belowCap = false; ///< adaptive quality under its cap (both known)
    bool atFloor = false; ///< ...and at SlowQualityFloor or less
};

struct State {
    std::optional<Choice> current;
    bool slowLink = false;
    Clock::time_point slowSince{}; ///< unbroken congestion since (epoch = not counting)
    QList<LinkSample> linkWindow; ///< the last SlowWindow of samples while the link is not slow
    Clock::time_point fastSince{};
    Clock::time_point lastSwitch{};
    std::array<Clock::time_point, 3> softwareBlockedUntil{}; ///< per Family, by the CPU guard
    std::array<int, 3> guardRejections{}; ///< per Family: how often the guard stepped away from it (D5 back-off)
    std::array<Clock::time_point, 3> guardBlockedAt{}; ///< per Family: the guard's last step away (epoch = retried since)
    Clock::time_point guardCalmSince{}; ///< software encoder under PresetRecoverBelow since (GuardForgiveAfter)
    Preset preset = Preset::Efficient; ///< of the current software HEVC/AV1 encoder
    std::optional<int> guardFrameRate; ///< the CPU guard's frame-rate cap (its last step)
    quint32 targetKbps = 0; ///< target bitrate of a software HEVC/AV1 encoder (quality, capped by the link)
    quint32 linkKbps = 0; ///< the slow-link cap on it (0 = none)
    Clock::time_point linkChangedAt{};
    Clock::time_point lastReconfigure{}; ///< codec switch, preset step, bitrate reopen or guard frame-rate change
    Clock::time_point lowLoadSince{}; ///< since when the load is under PresetRecoverBelow (epoch = not)
    Clock::time_point presetRaisedAt{}; ///< the last preset step back up
    Clock::time_point presetRaiseBlockedUntil{}; ///< flap guard (PresetFlapWindow)
    int presetFlaps = 0; ///< raises the guard had to undo, for the flap guard's back-off
    Clock::time_point clearSince{}; ///< slow link: since when no congestion signal (epoch = not clear)
    Clock::time_point fillSince{}; ///< slow link: since when sending at the cap (epoch = not)
    Clock::time_point lastProbeAt{}; ///< the last up-probe (epoch = none pending judgement)
    int probeFailures = 0; ///< failed up-probes in a row (congestion soon after), for the back-off
    quint32 probeFromKbps = 0; ///< the cap before the last up-probe (what a failed probe goes back to)
    Clock::time_point recoveredAt{}; ///< the last recovery from a slow link
    int recoverFlaps = 0; ///< slow links that came back soon after a recovery (RecoverFlapWindow)
    EncoderSettings applied; ///< what the last step() reported
    /// Reopens of the running encoder that settings changes caused (preset steps, a bitrate change
    /// without Backends::liveBitrate, a backend change); codec switches are not counted.
    int encoderRestarts = 0;
};

struct Decision {
    Choice choice;
    bool changed = false;
    QString reason; ///< set when changed (or when a wanted switch waits for the interval)
    EncoderSettings settings; ///< always set: what the encoder should run with now
    bool settingsChanged = false; ///< settings differ from the previous step() (or first step)
    QString settingsReason; ///< why, when settingsChanged without a codec change
    /// The settings change reopens the running encoder (see State::encoderRestarts). False for a
    /// live bitrate change and a frame-rate change; not set for a codec switch.
    bool restartsEncoder = false;
    /// Only the target bitrate changed (no preset, backend or frame-rate change).
    bool bitrateOnly = false;
};

/// A Decision with only its choice, change flag and reason set (the rest default).
inline Decision makeDecision(const Choice &choice, bool changed, const QString &reason)
{
    Decision d;
    d.choice = choice;
    d.changed = changed;
    d.reason = reason;
    return d;
}

inline bool softwarePrivate(const Choice &c)
{
    return !c.hardware && c.family != Family::Avc;
}

/// The settings for \a state's current choice.
inline EncoderSettings settingsOf(const State &state)
{
    EncoderSettings s;
    if (!state.current) return s;
    s.hardware = state.current->hardware;
    const bool privateSoftware = softwarePrivate(*state.current);
    s.preset = privateSoftware ? state.preset : Preset::Efficient;
    s.targetKbps = privateSoftware ? state.targetKbps : 0;
    int cap = privateSoftware ? SoftwarePrivateMaxFrameRate : 0;
    if (state.guardFrameRate) cap = cap ? std::min(cap, *state.guardFrameRate) : *state.guardFrameRate;
    s.maxFrameRate = cap;
    return s;
}

inline bool clientDecodes(const Input &in, Family f)
{
    return f == Family::Avc || in.client.contains(f);
}

/// The codec \a in and \a state call for now, ignoring the switch interval.
inline Choice select(const Input &in, const State &state, Clock::time_point now)
{
    const auto softwareAllowed = [&](Family f) {
        return now >= state.softwareBlockedUntil[size_t(f)];
    };
    const bool compress = in.mode == SoftwareEncoding::Prefer || (in.mode == SoftwareEncoding::Auto && in.adaptive && state.slowLink);
    if (compress) {
        for (const Family f : BestCompressionFirst) {
            if (!clientDecodes(in, f)) continue;
            const Backends &b = in.encoders.of(f);
            if (b.hardware) return {f, true};
            if (b.software && softwareAllowed(f)) return {f, false};
        }
    } else {
        for (const Family f : BestCompressionFirst) {
            if (clientDecodes(in, f) && in.encoders.of(f).hardware) return {f, true};
        }
    }
    // Nothing better: AVC, in hardware if there is one. Software H.264 is the last resort in
    // every mode, so there is always a picture (KPipeWire falls back to it by itself).
    return {Family::Avc, in.encoders.avc.hardware};
}

/**
 * Whether going from \a from to \a to reopens a running \a family encoder: a preset or backend
 * change, a switch between quality and bitrate mode, or a new bitrate on an encoder without
 * Backends::liveBitrate. A frame-rate cap only changes what the capture delivers.
 */
inline bool restartsEncoder(Family family, const Backends &backends, const EncoderSettings &from, const EncoderSettings &to)
{
    if (from.hardware != to.hardware || from.preset != to.preset) return true;
    if (from.targetKbps == to.targetKbps) return false;
    if (from.targetKbps == 0 || to.targetKbps == 0) return true;
    return family != Family::Avc && !backends.liveBitrate;
}

namespace detail
{
/// Updates \a state's slow-link cap (0 when the link is not slow or the encoder is not software HEVC/AV1).
inline void updateLinkCap(State &state, const Input &in, Clock::time_point now)
{
    if (!state.current || !softwarePrivate(*state.current) || !state.slowLink || !in.adaptive || in.mode == SoftwareEncoding::Never) {
        state.linkKbps = 0;
        return;
    }
    const double cap = slowBelowKbps(in.pixels);
    const auto bounded = [cap](double kbps) {
        return quint32(std::clamp(kbps, double(MinTargetKbps), std::max(cap, double(MinTargetKbps))));
    };
    quint32 wanted = state.linkKbps;
    bool probe = false;
    if (!in.bandwidthKbps) {
        wanted = state.linkKbps ? state.linkKbps : bounded(cap);
    } else if (const double link = *in.bandwidthKbps * TargetShareOfGoodput; state.linkKbps == 0) {
        wanted = bounded(link);
    } else if (in.congested) {
        // Goodput is demand-limited: while the link keeps up it only shows what the encoder sent,
        // so it lowers the cap only under congestion and the cap is probed back up otherwise.
        wanted = bounded(std::min(double(state.linkKbps), link));
        if (state.lastProbeAt != Clock::time_point{} && now - state.lastProbeAt < ProbeHold * 2) {
            // The probe was too much: undo it now, not after MinReconfigureInterval.
            state.probeFailures = std::min(state.probeFailures + 1, ProbeMaxDoublings);
            state.lastProbeAt = {};
            state.linkKbps = std::min(state.linkKbps, std::max(wanted, state.probeFromKbps));
            state.linkChangedAt = now;
            return;
        }
        state.lastProbeAt = {};
    } else if (state.fillSince != Clock::time_point{}
               && now - std::max(state.fillSince, state.linkChangedAt) >= ProbeHold * (1 << std::clamp(state.probeFailures, 0, ProbeMaxDoublings))) {
        // Up-probe: the stream sent at this cap, clear, for ProbeHold (longer after failed probes).
        if (state.lastProbeAt != Clock::time_point{}) {
            state.probeFailures = 0; // the previous probe held
        }
        wanted = bounded(state.linkKbps * ProbeGrowth);
        if (wanted != state.linkKbps) {
            probe = true;
            state.lastProbeAt = now;
            state.probeFromKbps = state.linkKbps;
        }
    }
    if (wanted == state.linkKbps) return;
    const double change = state.linkKbps ? std::abs(double(wanted) - state.linkKbps) / state.linkKbps : 1.0;
    // A probe step is applied whatever its size (the last one to the ceiling may be small).
    if (state.linkKbps == 0 || (now - state.linkChangedAt >= MinReconfigureInterval && (change >= TargetMinChange || probe))) {
        state.linkKbps = wanted;
        state.linkChangedAt = now;
    }
}

/// The target bitrate \a state's encoder should run with: for software HEVC/AV1 the adaptive
/// quality's bitrate, capped by the slow link; 0 (quality mode) for everything else.
inline quint32 wantedTarget(const State &state, const Input &in)
{
    if (!state.current || !softwarePrivate(*state.current)) {
        return 0;
    }
    quint32 target = qualityKbps(in.quality.value_or(DefaultQuality), in.pixels);
    if (state.linkKbps) {
        // AUD-FIX4 D2: on a slow link the cap is the target while the link is clear (the up-probe
        // needs the stream to use it); under congestion adaptive quality may hold it lower.
        target = in.congested ? std::min(target, state.linkKbps) : state.linkKbps;
    }
    return std::max(target, MinTargetKbps);
}

/**
 * AUD-FIX5 D2: adds this step's sample to \a state's slow-link window and judges the window
 * (see SlowWindow). Returns why the link is slow, or an empty string. Needs \a in's goodput.
 */
inline QString judgeSlowWindow(State &state, const Input &in, Clock::time_point now)
{
    LinkSample sample;
    sample.at = now;
    sample.congested = in.congested;
    sample.kbps = *in.bandwidthKbps;
    if (in.quality && in.qualityCap) {
        sample.belowCap = *in.quality < *in.qualityCap;
        sample.atFloor = sample.belowCap && *in.quality <= SlowQualityFloor;
    }
    auto &window = state.linkWindow;
    window.append(sample);
    while (!window.isEmpty() && now - window.first().at > SlowWindow) {
        window.removeFirst();
    }
    if (now - window.first().at < SlowWindowFull) {
        return {};
    }
    const double threshold = slowBelowKbps(in.pixels);
    int congested = 0;
    int episodes = 0;
    bool belowCap = true;
    bool atFloor = true;
    quint32 maxKbps = 0;
    for (qsizetype i = 0; i < window.size(); ++i) {
        const auto &s = window.at(i);
        if (s.kbps >= threshold) {
            return {}; // the link delivered what a normal one does
        }
        maxKbps = std::max(maxKbps, s.kbps);
        if (s.congested) {
            ++congested;
            if (i == 0 || !window.at(i - 1).congested) ++episodes;
        }
        belowCap = belowCap && s.belowCap;
        atFloor = atFloor && s.atFloor;
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - window.first().at).count();
    const QString counts = QStringLiteral("congested in %1 of %2 samples over %3 s, at most %4 kbit/s").arg(congested).arg(window.size()).arg(seconds).arg(maxKbps);
    if (congested >= SlowCongestedShare * window.size()) {
        return counts;
    }
    if (belowCap && episodes >= SlowEpisodes) {
        return counts + QStringLiteral(", %1 times, adaptive quality under its cap throughout").arg(episodes);
    }
    if (atFloor && congested > 0) {
        return counts + QStringLiteral(", adaptive quality at its floor");
    }
    return {};
}
}

inline Decision step(State &state, const Input &in, Clock::time_point now)
{
    QString linkReason;
    // The clear run (no congestion signal) and the run of sending at the cap, which the up-probe
    // and the recovery count on (AUD-FIX4 D2).
    if (!state.slowLink || !in.adaptive || in.congested) {
        state.clearSince = {};
    } else if (state.clearSince == Clock::time_point{}) {
        state.clearSince = now;
    }
    const bool atCap = state.linkKbps > 0 && state.targetKbps >= state.linkKbps * AtCapShare;
    const bool filling = state.clearSince != Clock::time_point{} && atCap && in.bandwidthKbps && *in.bandwidthKbps >= ProbeFillShare * state.targetKbps;
    if (!filling) {
        state.fillSince = {};
    } else if (state.fillSince == Clock::time_point{}) {
        state.fillSince = now;
    }
    // Link state, with hysteresis. Goodput alone is demand-limited (a still desktop sends
    // little), so "slow" also needs congestion, unbroken for LinkHold or (AUD-FIX5 D2) recurring
    // over SlowWindow; "fast again" needs goodput proving capacity, or (AUD-FIX4 D2) the probed
    // cap staying clear at its ceiling.
    if (!in.adaptive) {
        state.linkWindow.clear();
    }
    if (in.adaptive && in.bandwidthKbps) {
        const double kbps = *in.bandwidthKbps;
        if (!state.slowLink) {
            const bool slowSample = kbps < slowBelowKbps(in.pixels) && in.congested;
            QString slowBecause;
            if (!slowSample) {
                state.slowSince = {};
            } else if (state.slowSince == Clock::time_point{}) {
                state.slowSince = now;
            } else if (now - state.slowSince >= LinkHold) {
                slowBecause = QStringLiteral("%1 kbit/s").arg(*in.bandwidthKbps);
            }
            if (slowBecause.isEmpty()) {
                slowBecause = detail::judgeSlowWindow(state, in, now);
            }
            if (!slowBecause.isEmpty()) {
                state.slowLink = true;
                state.slowSince = {};
                state.clearSince = {};
                state.linkWindow.clear();
                if (state.recoveredAt != Clock::time_point{}) {
                    // Slow again soon after a recovery: the next one needs a longer clear run.
                    state.recoverFlaps = now - state.recoveredAt < RecoverFlapWindow ? std::min(state.recoverFlaps + 1, RecoverFlapMaxDoublings) : 0;
                }
                linkReason = QStringLiteral("slow link (%1)").arg(slowBecause);
            }
        } else {
            const bool fastSample = kbps > fastAboveKbps(in.pixels);
            if (!fastSample) {
                state.fastSince = {};
            } else if (state.fastSince == Clock::time_point{}) {
                state.fastSince = now;
            } else if (now - state.fastSince >= LinkHold) {
                state.slowLink = false;
                state.fastSince = {};
                state.clearSince = {};
                state.fillSince = {};
                state.linkWindow.clear();
                state.recoveredAt = now;
                linkReason = QStringLiteral("link recovered (%1 kbit/s)").arg(*in.bandwidthKbps);
            }
        }
    }
    if (in.adaptive && state.slowLink && state.clearSince != Clock::time_point{}) {
        const bool probing = state.current && softwarePrivate(*state.current) && in.mode != SoftwareEncoding::Never;
        const double ceiling = slowBelowKbps(in.pixels);
        const auto hold = RecoverHold * (1 << std::clamp(state.recoverFlaps, 0, RecoverFlapMaxDoublings));
        // Probing: sending at the ceiling for the hold. Otherwise: clear for the hold.
        const auto from = probing ? state.fillSince : state.clearSince;
        const bool ready = probing ? state.fillSince != Clock::time_point{} && state.linkKbps >= ceiling * ProbeCeilingShare
                                         && now - std::max(state.fillSince, state.linkChangedAt) >= hold
                                   : now - state.clearSince >= hold;
        if (ready) {
            state.slowLink = false;
            state.slowSince = {};
            state.fastSince = {};
            state.clearSince = {};
            state.fillSince = {};
            state.linkWindow.clear();
            state.recoveredAt = now;
            const auto held = std::chrono::duration_cast<std::chrono::seconds>(now - (probing ? std::max(from, state.linkChangedAt) : from)).count();
            linkReason = probing ? QStringLiteral("link recovered (%1 kbit/s for %2 s without congestion)").arg(*in.bandwidthKbps).arg(held)
                                 : QStringLiteral("link recovered (no congestion for %1 s)").arg(held);
        }
    }

    const bool reconfigureAllowed = now - state.lastReconfigure >= MinReconfigureInterval;
    const bool overBudget = state.current && !state.current->hardware && in.encodeLoadP95 && *in.encodeLoadP95 > CpuGuardLimit;
    const auto loadText = [&] {
        return QStringLiteral("CPU guard: software %1 at %2% of the frame budget (p95)")
            .arg(QLatin1String(familyName(state.current->family)))
            .arg(qRound(*in.encodeLoadP95 * 100));
    };
    // CPU guard, first step: a faster preset of the same software encoder (a reopen, no codec
    // change). A preset that selects the same encoder (AV1 Fastest = Balanced) is skipped.
    QString guardReason;
    QString settingsReason;
    bool presetStepPending = false;
    bool presetChanged = false;
    if (overBudget && softwarePrivate(*state.current)) {
        if (const auto next = nextPreset(state.current->family, state.preset)) {
            presetStepPending = true;
            if (reconfigureAllowed) {
                if (state.presetRaisedAt != Clock::time_point{} && now - state.presetRaisedAt < PresetFlapWindow) {
                    // The preset raised shortly before could not hold: do not raise it again soon.
                    state.presetRaiseBlockedUntil = now + PresetFlapWindow * (1 << std::min(state.presetFlaps, PresetFlapMaxDoublings));
                    ++state.presetFlaps;
                }
                state.preset = *next;
                state.lastReconfigure = now;
                state.lowLoadSince = {};
                presetChanged = true;
                settingsReason = loadText() + QStringLiteral("; preset %1").arg(QLatin1String(presetName(*next)));
            }
        }
    }
    // Second step: step away from this software codec, blocked for CpuBlockFor with the AUD-FIX5
    // D5 back-off (guardBlockFor()).
    if (overBudget && !presetStepPending && reconfigureAllowed) {
        const Family family = state.current->family;
        auto &blocked = state.softwareBlockedUntil[size_t(family)];
        if (now >= blocked) {
            const auto blockFor = guardBlockFor(state.guardRejections[size_t(family)]);
            const bool again = state.guardRejections[size_t(family)] > 0;
            blocked = now + blockFor;
            state.guardRejections[size_t(family)] = std::min(state.guardRejections[size_t(family)] + 1, 16);
            state.guardBlockedAt[size_t(family)] = now;
            if (again && family != Family::Avc) {
                // A retry that failed: the other codecs the guard rejected before wait as long,
                // so this falls through to the codec that carried the load.
                for (const Family other : {Family::Hevc, Family::Av1}) {
                    const auto i = size_t(other);
                    if (other != family && state.guardRejections[i] > 0 && state.softwareBlockedUntil[i] < blocked) {
                        state.softwareBlockedUntil[i] = blocked;
                        state.guardBlockedAt[i] = now;
                    }
                }
            }
            guardReason = loadText()
                + QStringLiteral("; not retried for %1 min").arg(std::chrono::duration_cast<std::chrono::minutes>(blockFor).count());
        }
    }
    // D5: a software encoder that stays well within its budget for GuardForgiveAfter means the
    // content or the load got lighter: the back-off starts over (blocks already running stay).
    const bool calm = state.current && !state.current->hardware && in.encodeLoadP95 && *in.encodeLoadP95 < PresetRecoverBelow;
    if (!calm) {
        state.guardCalmSince = {};
    } else if (state.guardCalmSince == Clock::time_point{}) {
        state.guardCalmSince = now;
    } else if (now - state.guardCalmSince >= GuardForgiveAfter) {
        state.guardRejections.fill(0);
    }

    // Low-load window for stepping the preset back up (see PresetRecoverBelow).
    const bool lowLoad = state.current && softwarePrivate(*state.current) && in.encodeLoadP95 && *in.encodeLoadP95 < PresetRecoverBelow;
    if (!lowLoad) {
        state.lowLoadSince = {};
    } else if (state.lowLoadSince == Clock::time_point{}) {
        state.lowLoadSince = now;
    }

    const Choice want = select(in, state, now);
    const auto finish = [&](Decision d) {
        // Target bitrate (software HEVC/AV1): adaptive quality's bitrate, capped by a slow link.
        // Where the change is live (libx265) it applies at once. Where it reopens the encoder
        // (SVT-AV1) it waits RestartBitrateInterval after the last reconfiguration, needs a
        // RestartBitrateMinChange step and gives way to a CPU-guard preset step that is waiting;
        // it rides along for free with a preset step. Entering or leaving bitrate mode happens only
        // with a codec switch (the settings reach the sessions before the encoder restarts).
        detail::updateLinkCap(state, in, now);
        const quint32 target = detail::wantedTarget(state, in);
        if (target != state.targetKbps) {
            const bool first = state.targetKbps == 0 || target == 0;
            const double change = state.targetKbps ? std::abs(double(target) - state.targetKbps) / state.targetKbps : 1.0;
            const bool live = state.current && state.current->family != Family::Avc ? in.encoders.of(state.current->family).liveBitrate : true;
            bool apply = first || d.changed || presetChanged;
            if (!apply && live) {
                apply = change >= LiveBitrateMinChange;
            } else if (!apply) {
                // The slow-link cap moves by at least TargetMinChange (or a probe step) and at
                // most once per MinReconfigureInterval already; the target follows it whole.
                const bool capDriven = state.linkKbps && target == state.linkKbps;
                const auto interval = target > state.targetKbps ? RestartBitrateRaiseInterval : RestartBitrateInterval;
                apply = !presetStepPending && now - state.lastReconfigure >= interval && (change >= RestartBitrateMinChange || capDriven);
            }
            if (apply) {
                if (!d.changed && !live && !presetChanged) state.lastReconfigure = now;
                if (settingsReason.isEmpty()) {
                    settingsReason = target ? QStringLiteral("target bitrate %1 kbit/s").arg(target) : QStringLiteral("quality mode");
                }
                state.targetKbps = target;
            }
        }
        d.settings = settingsOf(state);
        d.settingsChanged = d.settings != state.applied;
        if (d.settingsChanged && !d.changed) {
            d.settingsReason = settingsReason;
            auto withoutBitrate = state.applied;
            withoutBitrate.targetKbps = d.settings.targetKbps;
            d.bitrateOnly = withoutBitrate == d.settings;
            d.restartsEncoder = state.current && restartsEncoder(state.current->family, in.encoders.of(state.current->family), state.applied, d.settings);
            if (d.restartsEncoder) ++state.encoderRestarts;
        }
        state.applied = d.settings;
        return d;
    };
    if (!state.current) {
        state.current = want;
        state.lastSwitch = now;
        state.lastReconfigure = now;
        state.preset = Preset::Efficient;
        return finish(makeDecision(want, true, QStringLiteral("initial choice")));
    }
    if (want == *state.current) {
        // Third step: nowhere left to go (no faster preset, no other codec): lower the frame rate.
        if (overBudget && !presetStepPending && reconfigureAllowed) {
            const int from = settingsOf(state).maxFrameRate ? settingsOf(state).maxFrameRate : DefaultFrameRate;
            const int to = std::max(MinFrameRate, from / 2);
            if (to < from) {
                state.guardFrameRate = to;
                state.lastReconfigure = now;
                settingsReason = loadText() + QStringLiteral("; frame rate %1").arg(to);
            }
        } else if (state.guardFrameRate && !state.current->hardware && in.encodeLoadP95 && reconfigureAllowed) {
            // Back up one step when the load at the higher rate would stay well under the limit.
            const int from = *state.guardFrameRate;
            const int to = std::min(DefaultFrameRate, from * 2);
            if (*in.encodeLoadP95 * to / from < CpuGuardLimit * 0.8) {
                state.guardFrameRate = to >= DefaultFrameRate ? std::nullopt : std::optional<int>(to);
                state.lastReconfigure = now;
                settingsReason = QStringLiteral("CPU guard: load recovered; frame rate %1").arg(to);
            }
        } else if (!state.guardFrameRate && lowLoad && reconfigureAllowed && now - state.lowLoadSince >= PresetRecoverHold && now >= state.presetRaiseBlockedUntil) {
            // The ladder back up, in reverse: the frame rate first (above), then the preset, one
            // (real) level per PresetRecoverHold of low load.
            if (const auto previous = previousPreset(state.current->family, state.preset)) {
                state.preset = *previous;
                state.lastReconfigure = now;
                state.lowLoadSince = {};
                state.presetRaisedAt = now;
                presetChanged = true;
                settingsReason = QStringLiteral("CPU guard: software %1 at %2% of the frame budget (p95) for %3 s; preset %4")
                                     .arg(QLatin1String(familyName(state.current->family)))
                                     .arg(qRound(*in.encodeLoadP95 * 100))
                                     .arg(std::chrono::duration_cast<std::chrono::seconds>(PresetRecoverHold).count())
                                     .arg(QLatin1String(presetName(*previous)));
            }
        }
        return finish(makeDecision(want, false, {}));
    }
    // AUD-FIX5 D5: a codec whose guard block ran out is a retry, not a change of encoders or of
    // what the client decodes (the reason used to say so).
    const auto wantIndex = size_t(want.family);
    const bool guardRetry = softwarePrivate(want) && state.guardBlockedAt[wantIndex] != Clock::time_point{} && now >= state.softwareBlockedUntil[wantIndex];
    const auto retryText = [&] {
        return QStringLiteral("CPU guard: retrying software %1 after %2 s held back")
            .arg(QLatin1String(familyName(want.family)))
            .arg(std::chrono::duration_cast<std::chrono::seconds>(now - state.guardBlockedAt[wantIndex]).count());
    };
    const QString reason = !guardReason.isEmpty() ? guardReason
        : !linkReason.isEmpty()                    ? linkReason
        : guardRetry                               ? retryText()
                                                   : QStringLiteral("encoders or client codecs changed");
    if (now - state.lastSwitch < MinSwitchInterval) {
        return finish(makeDecision(*state.current, false, reason + QStringLiteral(" (waiting for the switch interval)")));
    }
    // A new encoder: the guard's frame-rate step and bitrate start over. After the guard left a
    // software codec, the next software codec starts at its fastest preset (PERF.md: AV1 M11,
    // then HEVC ultrafast); so does a guard retry (it needed that preset last time); otherwise
    // the efficient one.
    const bool fromGuard = !guardReason.isEmpty() || guardRetry;
    if (softwarePrivate(want)) {
        state.guardBlockedAt[wantIndex] = {}; // this retry is used up
    }
    state.current = want;
    state.lastSwitch = now;
    state.lastReconfigure = now;
    state.guardFrameRate.reset();
    state.targetKbps = 0;
    state.linkKbps = 0;
    state.fillSince = {};
    state.lastProbeAt = {};
    state.probeFailures = 0;
    state.lowLoadSince = {};
    state.presetRaisedAt = {};
    state.presetRaiseBlockedUntil = {};
    state.presetFlaps = 0;
    state.preset = fromGuard && softwarePrivate(want) ? Preset::Fastest : Preset::Efficient;
    return finish(makeDecision(want, true, reason));
}

/**
 * AUD-FIX4 D3: the CPU guard's per-interval sample. It used to divide the process CPU time per
 * frame by every hardware thread (16 on Sol), so 0.70 of the budget meant ~11 busy cores and the
 * guard never fired, even with SVT-AV1 at 180 % CPU delivering 13-23 of 30 fps. KRdp cannot see
 * a software encoder's per-frame latency, so it estimates it: CPU time per frame over the
 * encoder's *effective* parallelism, its own thread count (KPipeWire: clamp(cores / 2, 2, 8) for
 * libx265/SVT-AV1, min(cores, 16) for libx264; KPIPEWIRE_SW_ENCODER_THREADS overrides) times
 * how much of that a frame really uses. PERF.md measured, at 1080p30 on Sol with 8 threads:
 * SVT-AV1 M10 video 63 ms CPU per frame at 18.2 ms mean latency, desktop 32-38 ms at 10.1;
 * x265 veryfast video 120 ms at 36.5, desktop 42-56 ms at 14.8-18.4 (about 3.3 threads' worth,
 * so 0.4 of 8); libx264 23 ms at 3.0 with 16 threads (0.45).
 * Delivered frames per surface under ShortfallShare of the frame-rate cap, while the encoder is
 * busy (estimate >= ShortfallMinLoad of the budget) and frames still flow (>= ShortfallFloorShare,
 * i.e. motion, not a still desktop), count too: the sample is scaled by cap / delivered.
 */
constexpr double PrivateSoftwareParallelShare = 0.40;
constexpr double AvcSoftwareParallelShare = 0.45;
constexpr double ShortfallShare = 0.75;
constexpr double ShortfallFloorShare = 0.30;
constexpr double ShortfallMinLoad = 0.35;
/// The CPU guard needs at least this many frames in an interval to take a sample.
constexpr int MinFramesPerLoadSample = 5;

/// The software encoder's thread count for \a family, as KPipeWire picks it on a host with
/// \a idealThreads hardware threads (\a forced: KPIPEWIRE_SW_ENCODER_THREADS, HEVC/AV1 only).
inline int softwareEncoderThreads(Family family, int idealThreads, std::optional<int> forced = std::nullopt)
{
    if (family == Family::Avc) return std::clamp(idealThreads, 1, 16);
    if (forced && *forced > 0) return std::min(*forced, 64);
    return std::clamp(idealThreads / 2, 2, 8);
}
/// How many threads' worth of CPU one frame of \a family's software encoder can use at once.
inline double softwareEncoderParallelism(Family family, int threads)
{
    return std::max(1.0, threads * (family == Family::Avc ? AvcSoftwareParallelShare : PrivateSoftwareParallelShare));
}
/// Estimated encode time per frame for \a cpuMs process CPU time over \a frames frames.
inline double estimatedEncodeMs(double cpuMs, int frames, Family family, int threads)
{
    return cpuMs / std::max(1, frames) / softwareEncoderParallelism(family, threads);
}
/**
 * The load sample (1.0 = the whole frame budget) for \a cpuMs process CPU time spent over
 * \a seconds, in which \a frames frames were encoded on \a surfaces surfaces at a cap of
 * \a frameRate fps. nullopt when there were too few frames to judge.
 */
inline std::optional<double> encodeLoadSample(double cpuMs, int frames, double seconds, int frameRate, Family family, int threads, int surfaces = 1)
{
    if (frames < MinFramesPerLoadSample || seconds <= 0 || frameRate <= 0) return std::nullopt;
    const double budgetMs = 1000.0 / frameRate;
    double load = estimatedEncodeMs(cpuMs, frames, family, threads) / budgetMs;
    const double delivered = frames / seconds / std::max(1, surfaces);
    if (load >= ShortfallMinLoad && delivered < frameRate * ShortfallShare && delivered >= frameRate * ShortfallFloorShare) {
        load = std::max(load, load * frameRate / delivered);
    }
    return load;
}

/**
 * AUD-FIX4 D5: why a client that asked for \a requested (HEVC/AV1) gets AVC. It used to say "no
 * usable encoder" even when software encoders exist and the policy simply did not pick them.
 */
inline QString avcChoiceReason(const Encoders &encoders, SoftwareEncoding mode, bool adaptive, const QList<Family> &requested)
{
    QStringList none, softwareOnly;
    for (const Family f : requested) {
        if (f == Family::Avc) continue;
        const Backends &b = encoders.of(f);
        if (b.hardware) continue; // a hardware one exists: AVC only if the policy stepped away
        (b.software ? softwareOnly : none).append(QString::fromLatin1(familyName(f)));
    }
    QStringList parts;
    if (!none.isEmpty()) {
        parts << QStringLiteral("no encoder for %1 on this host").arg(none.join(QLatin1Char('/')));
    }
    if (!softwareOnly.isEmpty()) {
        const QString names = softwareOnly.join(QLatin1Char('/'));
        switch (mode) {
        case SoftwareEncoding::Never:
            parts << QStringLiteral("hardware encoder not available for %1; software encoding is off (SoftwareEncoding=never)").arg(names);
            break;
        case SoftwareEncoding::Auto:
            parts << (adaptive ? QStringLiteral("hardware encoder not available for %1; software not selected (link not slow)").arg(names)
                               : QStringLiteral("hardware encoder not available for %1; software only on a slow link, and this client fixed its codec").arg(names));
            break;
        case SoftwareEncoding::Prefer:
            parts << QStringLiteral("software %1 held back by the CPU guard").arg(names);
            break;
        }
    }
    if (parts.isEmpty()) {
        return QStringLiteral("hevc/av1 held back by the codec policy");
    }
    return parts.join(QStringLiteral("; "));
}

/// p95 over a sliding window of per-interval encode-load samples (see Input::encodeLoadP95).
class LoadWindow
{
public:
    static constexpr int Size = 10; ///< 15 s at the 1.5 s adaptive-quality interval
    static constexpr int MinimumSamples = 4;
    void add(double sample)
    {
        m_samples.append(sample);
        if (m_samples.size() > Size) m_samples.removeFirst();
    }
    void clear() { m_samples.clear(); }
    std::optional<double> p95() const
    {
        if (m_samples.size() < MinimumSamples) return std::nullopt;
        QList<double> sorted = m_samples;
        std::sort(sorted.begin(), sorted.end());
        // Nearest-rank p95.
        const qsizetype rank = qsizetype(std::ceil(0.95 * double(sorted.size())));
        return sorted.at(std::clamp<qsizetype>(rank - 1, 0, sorted.size() - 1));
    }

private:
    QList<double> m_samples;
};
}

Q_DECLARE_METATYPE(KRdp::CodecPolicy::EncoderSettings)
