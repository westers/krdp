// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "krdp_export.h"

#include <QJsonObject>
#include <QString>

#include <optional>

namespace KRdp
{
class VideoStream;

/**
 * The KRDPCTL `stats` request (KRDPCTL-V2-CONTRACT.md (g), STATS-PANEL-DESIGN.md §5), shared by
 * krdpserver and the console and virtual brokers, like CodecRequest. The caller removes
 * `requestId` first (LayoutControl::takeRequestId()) and puts it back on the reply. A `stats`
 * request never closes krdpserver's 3 s first-record gate.
 */
namespace StatsRequest
{
enum class Action { Subscribe, Unsubscribe };

struct Request {
    Action action = Action::Subscribe;
    int rateHz = 1; ///< as asked (rounded); the stream clamps it (StatsReporter::clampRate())
    bool operator==(const Request &) const = default;
};

/**
 * nullopt: `action` is not "subscribe"/"unsubscribe", or a subscribe's `rateHz` is present but
 * not a positive finite number (answer invalidRecord()). A missing `rateHz` means 1 Hz; unknown
 * extra fields are ignored.
 */
KRDP_EXPORT std::optional<Request> parse(const QJsonObject &record);
KRDP_EXPORT QJsonObject invalidRecord();

/// The `stats` reply: `state` "subscribed" (with the rate samples really go out at) or "unsubscribed".
KRDP_EXPORT QJsonObject replyRecord(bool subscribed, int rateHz);

/**
 * Hands \a request to \a stream (VideoStream::setStatsSubscription()) and returns the reply,
 * without requestId. \a log gets the line the host logs (debug level: stats are not logged at
 * info, STATS-PANEL-DESIGN.md §7).
 */
KRDP_EXPORT QJsonObject apply(VideoStream &stream, const Request &request, QString *log = nullptr);
}
}
