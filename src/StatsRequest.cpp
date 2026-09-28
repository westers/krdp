// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "StatsRequest.h"

#include <cmath>
#include <limits>

#include "LayoutControl.h"
#include "VideoStream.h"

namespace KRdp::StatsRequest
{
std::optional<Request> parse(const QJsonObject &record)
{
    Request request;
    const QString action = record.value(QLatin1String("action")).toString();
    if (action == QLatin1String("subscribe")) {
        request.action = Action::Subscribe;
    } else if (action == QLatin1String("unsubscribe")) {
        request.action = Action::Unsubscribe;
        return request;
    } else {
        return std::nullopt;
    }
    const QJsonValue rate = record.value(QLatin1String("rateHz"));
    if (rate.isUndefined()) {
        return request;
    }
    if (!rate.isDouble()) {
        return std::nullopt;
    }
    const double value = rate.toDouble();
    if (!std::isfinite(value) || value <= 0) {
        return std::nullopt;
    }
    request.rateHz = int(std::min(std::round(value), double(std::numeric_limits<int>::max())));
    return request;
}

QJsonObject invalidRecord()
{
    return LayoutControl::errorRecord(
        {QStringLiteral("invalid"), QStringLiteral("stats action must be subscribe or unsubscribe, and rateHz a positive number")});
}

QJsonObject replyRecord(bool subscribed, int rateHz)
{
    QJsonObject record{
        {QStringLiteral("type"), QStringLiteral("stats")},
        {QStringLiteral("v"), LayoutControl::ProtocolVersion},
        {QStringLiteral("state"), subscribed ? QStringLiteral("subscribed") : QStringLiteral("unsubscribed")},
    };
    if (subscribed) {
        record.insert(QStringLiteral("rateHz"), rateHz);
    }
    return record;
}

QJsonObject apply(VideoStream &stream, const Request &request, QString *log)
{
    const int rate = stream.setStatsSubscription(request.action == Action::Subscribe ? std::max(request.rateHz, 1) : 0);
    if (log) {
        *log = rate > 0 ? QStringLiteral("KRDPCTL: stats subscribed at %1 Hz (asked %2)").arg(rate).arg(request.rateHz)
                        : QStringLiteral("KRDPCTL: stats unsubscribed");
    }
    return replyRecord(rate > 0, rate);
}
}
