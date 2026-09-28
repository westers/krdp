// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QList>
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

/// What this host can encode. A software backend a KPipeWire build does not have yet
/// (HEVC, AV1 today) is simply false and skipped.
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
};

struct Decision {
    Choice choice;
    bool changed = false;
    QString reason; ///< set when changed (or when a wanted switch waits for the interval)
};

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

    QString guardReason;
    if (state.current && !state.current->hardware && in.encodeLoadP95 && *in.encodeLoadP95 > CpuGuardLimit) {
        auto &blocked = state.softwareBlockedUntil[size_t(state.current->family)];
        if (now >= blocked) {
            blocked = now + CpuBlockFor;
            guardReason = QStringLiteral("CPU guard: software %1 at %2% of the frame budget (p95)")
                              .arg(QLatin1String(familyName(state.current->family)))
                              .arg(qRound(*in.encodeLoadP95 * 100));
        }
    }

    const Choice want = select(in, state, now);
    if (!state.current) {
        state.current = want;
        state.lastSwitch = now;
        return {want, true, QStringLiteral("initial choice")};
    }
    if (want == *state.current) {
        return {want, false, {}};
    }
    const QString reason = !guardReason.isEmpty() ? guardReason
        : !linkReason.isEmpty()                    ? linkReason
                                                   : QStringLiteral("encoders or client codecs changed");
    if (now - state.lastSwitch < MinSwitchInterval) {
        return {*state.current, false, reason + QStringLiteral(" (waiting for the switch interval)")};
    }
    state.current = want;
    state.lastSwitch = now;
    return {want, true, reason};
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
