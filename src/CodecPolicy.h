// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
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
/// How long the link must look slow (or fast again) before the state flips.
constexpr auto LinkHold = std::chrono::seconds(5);
/// No two switches closer than this (the first choice is not a switch).
constexpr auto MinSwitchInterval = std::chrono::seconds(10);
/// The software encoder's p95 per-frame encode time must stay under this share of the frame budget.
constexpr double CpuGuardLimit = 0.70;
/// A software backend the guard stepped away from is not picked again for this long.
constexpr auto CpuBlockFor = std::chrono::seconds(120);

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
/// least MinTargetKbps, at most the slow-link threshold; it grows by TargetGrowth per
/// MinReconfigureInterval while the link is not congested, and moves only by at least
/// TargetMinChange.
constexpr double TargetShareOfGoodput = 0.85;
constexpr quint32 MinTargetKbps = 300;
constexpr double TargetGrowth = 1.25;
constexpr double TargetMinChange = 0.15;

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
 * RestartBitrateMinChange: an AV1 reopen (~150 ms) then costs at most 3 % of the time.
 */
constexpr auto RestartBitrateInterval = std::chrono::seconds(5);
constexpr double RestartBitrateMinChange = 0.20;
static_assert(std::chrono::milliseconds(150) * 33 <= RestartBitrateInterval, "a bitrate reopen stall must stay under ~3 % of its interval");

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
};

struct State {
    std::optional<Choice> current;
    bool slowLink = false;
    Clock::time_point slowSince{}; ///< epoch = not counting
    Clock::time_point fastSince{};
    Clock::time_point lastSwitch{};
    std::array<Clock::time_point, 3> softwareBlockedUntil{}; ///< per Family, by the CPU guard
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
    if (!in.bandwidthKbps) {
        wanted = state.linkKbps ? state.linkKbps : bounded(cap);
    } else if (const double link = *in.bandwidthKbps * TargetShareOfGoodput; state.linkKbps == 0) {
        wanted = bounded(link);
    } else if (in.congested) {
        // Goodput is demand-limited: while the link keeps up it only shows what the encoder sent,
        // so it lowers the cap only under congestion and the cap grows back otherwise.
        wanted = bounded(std::min(double(state.linkKbps), link));
    } else {
        wanted = bounded(state.linkKbps * TargetGrowth);
    }
    if (wanted == state.linkKbps) return;
    const double change = state.linkKbps ? std::abs(double(wanted) - state.linkKbps) / state.linkKbps : 1.0;
    if (state.linkKbps == 0 || (now - state.linkChangedAt >= MinReconfigureInterval && change >= TargetMinChange)) {
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
    if (state.linkKbps) target = std::min(target, state.linkKbps);
    return std::max(target, MinTargetKbps);
}
}

inline Decision step(State &state, const Input &in, Clock::time_point now)
{
    QString linkReason;
    // Link state, with hysteresis. Goodput alone is demand-limited (a still desktop sends
    // little), so "slow" also needs congestion; "fast again" needs goodput proving capacity.
    if (in.adaptive && in.bandwidthKbps) {
        const double kbps = *in.bandwidthKbps;
        if (!state.slowLink) {
            const bool slowSample = kbps < slowBelowKbps(in.pixels) && in.congested;
            if (!slowSample) {
                state.slowSince = {};
            } else if (state.slowSince == Clock::time_point{}) {
                state.slowSince = now;
            } else if (now - state.slowSince >= LinkHold) {
                state.slowLink = true;
                state.slowSince = {};
                linkReason = QStringLiteral("slow link (%1 kbit/s)").arg(*in.bandwidthKbps);
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
                linkReason = QStringLiteral("link recovered (%1 kbit/s)").arg(*in.bandwidthKbps);
            }
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
    // Second step: step away from this software codec (blocked for CpuBlockFor).
    if (overBudget && !presetStepPending && reconfigureAllowed) {
        auto &blocked = state.softwareBlockedUntil[size_t(state.current->family)];
        if (now >= blocked) {
            blocked = now + CpuBlockFor;
            guardReason = loadText();
        }
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
                apply = !presetStepPending && now - state.lastReconfigure >= RestartBitrateInterval && change >= RestartBitrateMinChange;
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
        return finish({want, true, QStringLiteral("initial choice")});
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
        return finish({want, false, {}});
    }
    const QString reason = !guardReason.isEmpty() ? guardReason
        : !linkReason.isEmpty()                    ? linkReason
                                                   : QStringLiteral("encoders or client codecs changed");
    if (now - state.lastSwitch < MinSwitchInterval) {
        return finish({*state.current, false, reason + QStringLiteral(" (waiting for the switch interval)")});
    }
    // A new encoder: the guard's frame-rate step and bitrate start over. After the guard left a
    // software codec, the next software codec starts at its fastest preset (PERF.md: AV1 M11,
    // then HEVC ultrafast); otherwise at the efficient one.
    const bool fromGuard = !guardReason.isEmpty();
    state.current = want;
    state.lastSwitch = now;
    state.lastReconfigure = now;
    state.guardFrameRate.reset();
    state.targetKbps = 0;
    state.linkKbps = 0;
    state.lowLoadSince = {};
    state.presetRaisedAt = {};
    state.presetRaiseBlockedUntil = {};
    state.presetFlaps = 0;
    state.preset = fromGuard && softwarePrivate(want) ? Preset::Fastest : Preset::Efficient;
    return finish({want, true, reason});
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
