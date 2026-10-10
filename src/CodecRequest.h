// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "CodecPolicy.h"
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
    /// AV1-Q: `decode`, the client's decode path per codec ("hw"/"sw"; anything else, or no
    /// `decode`, is Unknown: optional and never an error, so older clients are unaffected).
    CodecPolicy::ClientDecode decode;
    /// OPT-062 S2: the request the selection runs on, built from `order` / `encode` / `decodeMode` / `decoders`
    /// (and the old fields for what they leave out). Set only when the record has at least one of the new fields;
    /// otherwise from `codecs` / `decode` / `adaptive` by CodecPolicy::requestFromRecord() when applied.
    std::optional<CodecPolicy::Request> selection;
    QString orderText; ///< the new fields as the client sent them, for the log ("order [av1,hevc,avc] encode any decode software")
    bool operator==(const Request &) const = default;
};

/**
 * nullopt: the record is invalid (answer invalidRecord(\a error)): `codecs` is not an array of "hevc"/"av1" strings,
 * or one of the S2 fields is malformed: `order` (distinct "avc"/"hevc"/"av1", at most 3; empty = standard AVC only),
 * `encode` / `decodeMode` ("hardware" | "software" | "any"), `decoders` (an object with only the keys avc/hevc/av1,
 * each an array of at most two distinct "hw"/"sw" strings, [] = cannot decode). \a error gets a short reason.
 */
KRDP_EXPORT std::optional<Request> parse(const QJsonObject &record, QString *error = nullptr);
KRDP_EXPORT QJsonObject invalidRecord(const QString &message = {});

/**
 * Hands \a request to \a stream's codec policy (VideoStream::setPrivateCodecPolicy(): the choice
 * takes effect at once) and returns the `codec` reply, without requestId. \a log gets the line
 * the host logs.
 */
KRDP_EXPORT QJsonObject apply(VideoStream &stream, const Request &request, QString *log = nullptr);
}
}
