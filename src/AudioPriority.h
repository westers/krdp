// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QJsonObject>
#include <optional>

namespace KRdp::AudioPriority
{
struct Request {
    QString id;
    bool enabled = false;
};
inline std::optional<Request> parse(const QJsonObject &record)
{
    const auto id = record.value(QStringLiteral("id"));
    const auto enabled = record.value(QStringLiteral("enabled"));
    if (record.value(QStringLiteral("type")) != QJsonValue(QStringLiteral("audio-priority"))
        || record.value(QStringLiteral("v")) != QJsonValue(1) || !id.isString()
        || id.toString().isEmpty() || id.toString().size() > 64 || !enabled.isBool()) {
        return std::nullopt;
    }
    return Request{id.toString(), enabled.toBool()};
}
inline QJsonObject reply(const QJsonObject &request, bool effective, const QString &error = {})
{
    return {{QStringLiteral("type"), QStringLiteral("audio-priority")}, {QStringLiteral("v"), 1},
            {QStringLiteral("id"), request.value(QStringLiteral("id")).toString().left(64)},
            {QStringLiteral("ok"), error.isEmpty()}, {QStringLiteral("effective"), effective},
            {QStringLiteral("message"), error}};
}
}
