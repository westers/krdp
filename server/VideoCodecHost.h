// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QString>

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
};

/// `--software-encoding` / KRDP_*_SOFTWARE_ENCODING: auto (default, also for an empty value), never or prefer.
inline std::optional<CodecPolicy::SoftwareEncoding> parseHostSoftwareEncoding(const QString &value)
{
    const QString v = value.trimmed();
    return v.isEmpty() ? std::optional(CodecPolicy::SoftwareEncoding::Auto) : CodecPolicy::parseSoftwareEncoding(v);
}
}
