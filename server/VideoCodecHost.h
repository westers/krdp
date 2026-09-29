// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QString>
#include <QtGlobal>

#include <optional>

#include "CodecPolicy.h"
#include "EncoderSupport.h"

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
};

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
