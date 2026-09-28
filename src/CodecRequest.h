// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "VideoCodecSupport.h"
#include "krdp_export.h"

#include <QJsonObject>
#include <QStringList>
#include <QVector>

#include <optional>

namespace KRdp
{
class VideoStream;

/**
 * The KRDPCTL `codec` request (KRDPCTL-V2-CONTRACT.md (e)), shared by krdpserver and the console
 * and virtual brokers (AUD-FIX7), so every host parses and answers it the same way. The caller
 * removes `requestId` first (LayoutControl::takeRequestId()) and puts it back on the reply.
 */
namespace CodecRequest
{
struct Request {
    QVector<VideoCodec> codecs; ///< Hevc/Av1 the client decodes; empty = AVC only
    QStringList names; ///< as the client sent them (lower case), for the log
    bool adaptive = true;
    bool operator==(const Request &) const = default;
};

/// nullopt: `codecs` is not an array of "hevc"/"av1" strings (answer invalidRecord()).
KRDP_EXPORT std::optional<Request> parse(const QJsonObject &record);
KRDP_EXPORT QJsonObject invalidRecord();

/**
 * Hands \a request to \a stream's codec policy (VideoStream::setPrivateCodecPolicy(): the choice
 * takes effect at once) and returns the `codec` reply, without requestId. \a log gets the line
 * the host logs.
 */
KRDP_EXPORT QJsonObject apply(VideoStream &stream, const Request &request, QString *log = nullptr);
}
}
