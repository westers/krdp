// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "CodecTypes.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <array>
#include <optional>

/**
 * OPT-062 S1: per-connection codec selection (docs/superpowers/specs/2026-10-09-per-connection-codec-selection-design.md,
 * sections 4 and 6). Pure: no channel, no encoder, no device is opened. The client chooses an ordered codec
 * list and one Hardware / Software / Any mode for each end; the host limits software encoders per codec;
 * the answer is the first codec in the list that the host can encode in the requested mode within its
 * allowance AND the client can decode in the requested mode. Nothing satisfiable: standard AVC, the
 * baseline, flagged.
 */
namespace KRdp::CodecPolicy
{
enum class Mode { Hardware, Software, Any };
inline const char *modeName(Mode m)
{
    switch (m) {
    case Mode::Hardware: return "hardware";
    case Mode::Software: return "software";
    case Mode::Any: return "any";
    }
    return "?";
}
inline std::optional<Mode> parseMode(QStringView value)
{
    const QString v = value.trimmed().toString().toLower();
    if (v == QLatin1String("hardware")) return Mode::Hardware;
    if (v == QLatin1String("software")) return Mode::Software;
    if (v == QLatin1String("any")) return Mode::Any;
    return std::nullopt;
}

/// The host's ceiling for software encoders, per codec (hardware encoders are always allowed). For AVC,
/// false means "only as a last resort": software H.264 stays possible as the baseline, never as a candidate.
struct SoftwareAllowance {
    bool avc = true;
    bool hevc = true;
    bool av1 = true;
    bool allows(Family f) const { return f == Family::Hevc ? hevc : f == Family::Av1 ? av1 : avc; }
    bool operator==(const SoftwareAllowance &) const = default;
};
/// The allowance derived from the existing `SoftwareEncoding` setting (S3 adds the per-codec keys):
/// `never` forbids software HEVC/AV1 and makes AVC a last resort; `auto` and `prefer` allow all.
inline SoftwareAllowance allowanceFromSoftwareNever(bool never)
{
    return never ? SoftwareAllowance{false, false, false} : SoftwareAllowance{};
}

/// What a client can decode a codec with (not what it will use).
struct DecoderPaths {
    bool hardware = false;
    bool software = false;
    bool operator==(const DecoderPaths &) const = default;
};

/// The connection's request: today it is built from the old `codecs`/`decode`/`adaptive` fields
/// (legacyRequest()), in S2 from the new `order`/`encode`/`decodeMode`/`decoders` fields.
struct Request {
    QList<Family> order; ///< distinct families, most wanted first; Avc is appended last when missing
    Mode encode = Mode::Any; ///< Encoding on the host
    Mode decode = Mode::Any; ///< Decoding on this computer
    std::array<DecoderPaths, 3> decoders{}; ///< indexed by Family
    bool adaptive = true;
    /// Built from an old client's record: no encode/decode mode was said, so the host's SoftwareEncoding
    /// decides the encode mode (hardware only, software on a slow link or under `prefer`) as before.
    bool legacy = false;
    const DecoderPaths &decodersOf(Family f) const { return decoders[size_t(f)]; }
    bool operator==(const Request &) const = default;
};

/// order with duplicates dropped and Avc last (the baseline is always reachable).
inline QList<Family> normalizedOrder(const QList<Family> &order)
{
    QList<Family> out;
    for (const Family f : order) {
        if (!out.contains(f)) out.append(f);
    }
    if (!out.contains(Family::Avc)) out.append(Family::Avc);
    return out;
}

enum class SkipReason {
    NoHardwareEncoder,
    NoSoftwareEncoder,
    SoftwareNotAllowed,
    NoEncoder,
    SizeLimit, ///< a hardware encoder exists but cannot encode this stream's size
    CpuGuard, ///< the software encoder is held back by the CPU guard
    ClientCannotDecodeInHardware,
    ClientCannotDecode,
    ConsoleShared, ///< S2: another viewer is admitted (AVC for everyone)
    NotController, ///< S2
    EncoderFailed, ///< S2
    DeviceFailed, ///< S2
};
inline const char *skipReasonName(SkipReason r)
{
    switch (r) {
    case SkipReason::NoHardwareEncoder: return "noHardwareEncoder";
    case SkipReason::NoSoftwareEncoder: return "noSoftwareEncoder";
    case SkipReason::SoftwareNotAllowed: return "softwareNotAllowed";
    case SkipReason::NoEncoder: return "noEncoder";
    case SkipReason::SizeLimit: return "sizeLimit";
    case SkipReason::CpuGuard: return "cpuGuard";
    case SkipReason::ClientCannotDecodeInHardware: return "clientCannotDecodeInHardware";
    case SkipReason::ClientCannotDecode: return "clientCannotDecode";
    case SkipReason::ConsoleShared: return "consoleShared";
    case SkipReason::NotController: return "notController";
    case SkipReason::EncoderFailed: return "encoderFailed";
    case SkipReason::DeviceFailed: return "deviceFailed";
    }
    return "?";
}

struct Skip {
    Family family = Family::Avc;
    QList<SkipReason> reasons;
    bool operator==(const Skip &) const = default;
};

struct Candidate {
    Family family = Family::Avc;
    bool hardware = false;
    bool operator==(const Candidate &) const = default;
};

/// What the host side knows about a stream at decision time.
struct HostLimits {
    SoftwareAllowance allowance;
    /// Per Family: the software encoder is held back by the CPU guard.
    std::array<bool, 3> softwareHeld{};
    /// Per Family: the hardware encoder cannot encode this stream's size (hardwareFits()).
    std::array<bool, 3> hardwareUnfit{};
};

/// VA-API / NVENC H.264 encode at most 4096 in either direction (Hal, spec section 7.1); HEVC and AV1 take 8192.
constexpr int AvcHardwareMaxDimension = 4096;
constexpr int PrivateHardwareMaxDimension = 8192;
inline bool hardwareFits(Family f, int width, int height)
{
    const int limit = f == Family::Avc ? AvcHardwareMaxDimension : PrivateHardwareMaxDimension;
    return width <= limit && height <= limit;
}

struct Plan {
    QList<Candidate> candidates; ///< every satisfying way, codec-major (hardware before software inside a codec)
    QList<Skip> skipped; ///< each family of the order that has no candidate, with every reason
    Candidate baseline; ///< standard AVC, used when candidates is empty
    bool useBaseline() const { return candidates.isEmpty(); }
    Candidate choice() const { return candidates.isEmpty() ? baseline : candidates.first(); }
};

/// Tables 4.1 and 4.2 of the spec, for the families of \a request.order, in that order.
inline Plan plan(const Request &request, const Encoders &encoders, const HostLimits &host)
{
    Plan out;
    out.baseline = {Family::Avc, encoders.avc.hardware && !host.hardwareUnfit[size_t(Family::Avc)]};
    for (const Family f : normalizedOrder(request.order)) {
        const Backends &b = encoders.of(f);
        const bool hwOk = b.hardware && !host.hardwareUnfit[size_t(f)];
        const bool allowed = host.allowance.allows(f);
        const bool held = f != Family::Avc && host.softwareHeld[size_t(f)];
        const bool swOk = b.software && allowed && !held;
        const bool wantHw = request.encode != Mode::Software;
        const bool wantSw = request.encode != Mode::Hardware;
        QList<SkipReason> why;
        const bool hwUsable = wantHw && hwOk;
        const bool swUsable = wantSw && swOk;
        if (!hwUsable && !swUsable) {
            if (wantHw && !hwOk) why.append(b.hardware ? SkipReason::SizeLimit : SkipReason::NoHardwareEncoder);
            if (wantSw) {
                if (!b.software) why.append(wantHw && !b.hardware ? SkipReason::NoEncoder : SkipReason::NoSoftwareEncoder);
                else if (!allowed) why.append(SkipReason::SoftwareNotAllowed);
                else if (held) why.append(SkipReason::CpuGuard);
            }
            // Any with neither backend present: one reason, not two.
            if (why.contains(SkipReason::NoEncoder)) why.removeAll(SkipReason::NoHardwareEncoder);
        }
        const DecoderPaths &dec = request.decodersOf(f);
        const bool decoded = request.decode == Mode::Hardware ? dec.hardware
            : request.decode == Mode::Software                ? dec.software
                                                              : (dec.hardware || dec.software);
        if (!decoded) {
            why.append(request.decode == Mode::Hardware ? SkipReason::ClientCannotDecodeInHardware : SkipReason::ClientCannotDecode);
        }
        if (!why.isEmpty()) {
            out.skipped.append({f, why});
            continue;
        }
        if (hwUsable) out.candidates.append({f, true});
        if (swUsable) out.candidates.append({f, false});
    }
    return out;
}

/// AVC444 follows AVC's encode mode: software H.264 is 4:2:0, so a Software request never gets AVC444.
inline bool avc444Allowed(Mode encode, bool hardwareChosen)
{
    return encode != Mode::Software && hardwareChosen;
}

/// "av1 (softwareNotAllowed), hevc (noHardwareEncoder+clientCannotDecode)"; empty when nothing was skipped.
inline QString skippedText(const QList<Skip> &skipped)
{
    QStringList parts;
    for (const Skip &s : skipped) {
        QStringList names;
        for (const SkipReason r : s.reasons) names << QLatin1String(skipReasonName(r));
        parts << QStringLiteral("%1 (%2)").arg(QLatin1String(familyName(s.family)), names.join(QLatin1Char('+')));
    }
    return parts.join(QStringLiteral(", "));
}

inline QString orderText(const QList<Family> &order)
{
    QStringList names;
    for (const Family f : order) names << QLatin1String(familyName(f));
    return names.join(QLatin1Char(','));
}

/**
 * The request an old client's `codecs`/`decode`/`adaptive` record stands for: its codecs in the order
 * it sent them, then AVC; encode and decode mode Any (the host's SoftwareEncoding still decides
 * software use, see Request::legacy); decoders from the paths it reported (Unknown = either), AVC
 * software only (our client decodes AVC in software, spec C2).
 */
inline Request legacyRequest(const QList<Family> &privateOrder, const ClientDecode &decode, bool adaptive)
{
    Request r;
    r.legacy = true;
    r.adaptive = adaptive;
    r.order = privateOrder;
    r.order.append(Family::Avc);
    const auto paths = [](DecodePath p) {
        return DecoderPaths{p != DecodePath::Software, p != DecodePath::Hardware};
    };
    r.decoders[size_t(Family::Avc)] = {false, true};
    r.decoders[size_t(Family::Hevc)] = paths(decode.hevc);
    r.decoders[size_t(Family::Av1)] = paths(decode.av1);
    return r;
}
}
