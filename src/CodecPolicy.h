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

/**
 * What the running encoder is told besides its codec (WS-E): the backend, the software preset,
 * the target bitrate (0 = quality mode, driven by adaptive quality) and a frame-rate cap
 * (0 = none).
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
/// Slow-link target bitrate: this share of the measured goodput while congested, at least
/// MinTargetKbps, at most the slow-link threshold; it grows by TargetGrowth per step while the
/// link is not congested, and changes only by at least TargetMinChange (a reopen per change).
constexpr double TargetShareOfGoodput = 0.85;
constexpr quint32 MinTargetKbps = 300;
constexpr double TargetGrowth = 1.25;
constexpr double TargetMinChange = 0.15;

inline double scale(qint64 pixels)
{
    return std::max(0.1, double(std::max<qint64>(pixels, 1)) / double(ReferencePixels));
}
inline double slowBelowKbps(qint64 pixels) { return SlowBelowMbps1080p * 1000.0 * scale(pixels); }
inline double fastAboveKbps(qint64 pixels) { return FastAboveMbps1080p * 1000.0 * scale(pixels); }

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
    quint32 targetKbps = 0; ///< slow-link target bitrate of a software HEVC/AV1 encoder
    Clock::time_point lastReconfigure{}; ///< codec switch, preset step, bitrate or guard frame-rate change
    EncoderSettings applied; ///< what the last step() reported
};

struct Decision {
    Choice choice;
    bool changed = false;
    QString reason; ///< set when changed (or when a wanted switch waits for the interval)
    EncoderSettings settings; ///< always set: what the encoder should run with now
    bool settingsChanged = false; ///< settings differ from the previous step() (or first step)
    QString settingsReason; ///< why, when settingsChanged without a codec change
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

namespace detail
{
/// The slow-link target bitrate for \a state's software HEVC/AV1 encoder (0: quality mode).
inline quint32 wantedTarget(const State &state, const Input &in)
{
    if (!state.current || !softwarePrivate(*state.current) || !state.slowLink || !in.adaptive || in.mode == SoftwareEncoding::Never) {
        return 0;
    }
    const double cap = slowBelowKbps(in.pixels);
    const auto bounded = [cap](double kbps) {
        return quint32(std::clamp(kbps, double(MinTargetKbps), std::max(cap, double(MinTargetKbps))));
    };
    if (!in.bandwidthKbps) {
        return state.targetKbps ? state.targetKbps : bounded(cap);
    }
    const double link = *in.bandwidthKbps * TargetShareOfGoodput;
    if (state.targetKbps == 0) {
        return bounded(link);
    }
    // Goodput is demand-limited: while the link keeps up it only shows what the encoder sent,
    // so it lowers the target only under congestion and the target grows back otherwise.
    if (in.congested) {
        return bounded(std::min(double(state.targetKbps), link));
    }
    return bounded(state.targetKbps * TargetGrowth);
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
    if (overBudget && softwarePrivate(*state.current)) {
        if (const auto next = nextPreset(state.current->family, state.preset)) {
            presetStepPending = true;
            if (reconfigureAllowed) {
                state.preset = *next;
                state.lastReconfigure = now;
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

    const Choice want = select(in, state, now);
    const auto finish = [&](Decision d) {
        // Bitrate (slow link, software HEVC/AV1): a change reopens the encoder, so it waits for
        // the reconfiguration interval and a meaningful step. Leaving bitrate mode is immediate.
        const quint32 target = detail::wantedTarget(state, in);
        if (target != state.targetKbps) {
            const bool first = state.targetKbps == 0 || target == 0;
            const double change = state.targetKbps ? std::abs(double(target) - state.targetKbps) / state.targetKbps : 1.0;
            const bool allowed = now - state.lastReconfigure >= MinReconfigureInterval; // not in the step that reconfigured
            if (first || (allowed && change >= TargetMinChange)) {
                if (!d.changed) state.lastReconfigure = now;
                if (settingsReason.isEmpty()) {
                    settingsReason = target ? QStringLiteral("target bitrate %1 kbit/s").arg(target) : QStringLiteral("quality mode");
                }
                state.targetKbps = target;
            }
        }
        d.settings = settingsOf(state);
        d.settingsChanged = d.settings != state.applied;
        if (d.settingsChanged && !d.changed) d.settingsReason = settingsReason;
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
