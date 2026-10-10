// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QtGlobal>

#include <optional>

#include "CodecPolicy.h"
#include "EncoderSupport.h"
#include "VideoStream.h"

namespace KRdp
{
/**
 * AUD-FIX7: what a console or virtual broker advertises in `capabilities.video` and seeds each
 * connection's codec policy with, before any worker exists: the broker's own encoder probe at
 * startup (EncoderSupport::probe()) and the host's SoftwareEncoding. The worker's own probe
 * replaces the encoders once it reports (WorkerCodecBridge).
 */
struct VideoCodecHost {
    EncoderSupport::Probe probe;
    CodecPolicy::SoftwareEncoding mode = CodecPolicy::SoftwareEncoding::Auto;
    /// AV1-Q: `--av1-tiles` / KRDP_*_AV1_TILES (CodecPolicy::parseAv1Tiles(); 0 = automatic).
    int av1Tiles = CodecPolicy::Av1TilesAutomatic;
    /// OPT-062 S3: the per-codec software ceiling (`--software-avc|hevc|av1`). Unset: derived from \a mode.
    std::optional<CodecPolicy::SoftwareAllowance> ceiling;

    /// The software ceiling for one connection: the host's, tightened (never loosened) by the user's own SoftwareEncoding (spec C5).
    CodecPolicy::SoftwareAllowance allowance(std::optional<CodecPolicy::SoftwareEncoding> user = {}) const
    {
        const auto host = ceiling ? *ceiling : CodecPolicy::allowanceFor(mode);
        return user ? CodecPolicy::intersect(host, CodecPolicy::allowanceFor(*user)) : host;
    }
    /// The host as one user's connection sees it: their own SoftwareEncoding replaces the mode (old clients) and can only tighten the ceiling.
    VideoCodecHost withUser(std::optional<CodecPolicy::SoftwareEncoding> user) const
    {
        VideoCodecHost copy = *this;
        copy.ceiling = allowance(user);
        copy.mode = user.value_or(mode);
        return copy;
    }
    /// Seeds \a stream's codec policy: the probe's encoders and labels, the mode and the ceiling.
    void apply(VideoStream &stream, std::optional<CodecPolicy::SoftwareEncoding> user = {}) const
    {
        stream.setEncoderPolicy(probe.encoders, user.value_or(mode));
        stream.setEncoderLabels(probe.labels);
        stream.setSoftwareAllowance(allowance(user));
    }
};

/// The probe's encoders as the public snapshot's `videoEncoders` (everything found, whatever the software ceiling says).
inline QJsonArray publicVideoEncoders(const EncoderSupport::Probe &probe)
{
    QJsonArray result;
    const auto video = EncoderSupport::videoCapabilities(probe, CodecPolicy::SoftwareEncoding::Auto, CodecPolicy::SoftwareAllowance{});
    for (const auto &encoder : video.encoders) {
        QJsonObject entry{{QStringLiteral("codec"), encoder.codec}, {QStringLiteral("backend"), encoder.backend}, {QStringLiteral("hw"), encoder.hardware}};
        if (!encoder.device.isEmpty()) entry.insert(QStringLiteral("device"), encoder.device);
        if (!encoder.name.isEmpty()) entry.insert(QStringLiteral("name"), encoder.name);
        result.append(entry);
    }
    return result;
}

/// `--software-avc|hevc|av1` / KRDP_*_SOFTWARE_*: the ceiling of one family; nullopt for an unknown value.
inline std::optional<CodecPolicy::CeilingSetting> parseHostCeiling(const QString &value, CodecPolicy::Family family)
{
    return CodecPolicy::parseCeilingSetting(value, family);
}
/// The host's ceilings from the three settings and the old mode (auto follows \a mode).
inline CodecPolicy::SoftwareAllowance resolveHostCeiling(CodecPolicy::SoftwareEncoding mode, CodecPolicy::CeilingSetting avc, CodecPolicy::CeilingSetting hevc,
                                                         CodecPolicy::CeilingSetting av1)
{
    const bool never = mode == CodecPolicy::SoftwareEncoding::Never;
    return {CodecPolicy::ceilingAllows(avc, never), CodecPolicy::ceilingAllows(hevc, never), CodecPolicy::ceilingAllows(av1, never)};
}

/**
 * AV1-Q: `--av1-tiles` / KRDP_*_AV1_TILES: auto (default, also for an empty value), 1, 2, 4, 8 or
 * 16. Anything else is logged and taken as auto: a typo must not keep the broker from starting.
 */
inline int parseHostAv1Tiles(const QString &value, const char *who)
{
    if (const auto tiles = CodecPolicy::parseAv1Tiles(value)) return *tiles;
    qWarning("%s: unknown AV1 tile setting '%s' (auto, 1, 2, 4, 8 or 16); using auto", who, qPrintable(value));
    return CodecPolicy::Av1TilesAutomatic;
}

/// `--software-encoding` / KRDP_*_SOFTWARE_ENCODING: auto (default, also for an empty value), never or prefer.
inline std::optional<CodecPolicy::SoftwareEncoding> parseHostSoftwareEncoding(const QString &value)
{
    const QString v = value.trimmed();
    return v.isEmpty() ? std::optional(CodecPolicy::SoftwareEncoding::Auto) : CodecPolicy::parseSoftwareEncoding(v);
}
}
