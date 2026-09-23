// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QJsonObject>
#include <QSize>
#include <cmath>
#include <optional>

namespace KRdp::VirtualResizeProtocol
{
struct Request {
    QString id;
    QSize pixels;
    double scale;
};

inline std::optional<Request> parse(const QJsonObject &record)
{
    const auto id = record.value(QStringLiteral("id"));
    const auto width = record.value(QStringLiteral("width"));
    const auto height = record.value(QStringLiteral("height"));
    const auto scale = record.value(QStringLiteral("scale"));
    if (record.size() != 6 || record.value(QStringLiteral("type")) != QStringLiteral("virtual-resize")
        || !record.value(QStringLiteral("v")).isDouble() || record.value(QStringLiteral("v")).toDouble() != 1
        || !id.isString() || id.toString().isEmpty() || id.toString().size() > 64
        || !width.isDouble() || !height.isDouble() || !scale.isDouble()) return {};
    const double w = width.toDouble(), h = height.toDouble(), s = scale.toDouble();
    if (!std::isfinite(w) || !std::isfinite(h) || !std::isfinite(s)
        || w < 320 || w > 4096 || h < 200 || h > 4096 || s < 1 || s > 4
        || std::floor(w) != w || std::floor(h) != h || int(w) % 2 || int(h) % 2) return {};
    return Request{id.toString(), QSize(int(w), int(h)), std::round(s * 120) / 120};
}

inline QJsonObject reply(const QString &id, const QString &error = {})
{
    QJsonObject result{{QStringLiteral("type"), QStringLiteral("virtual-resize")}, {QStringLiteral("v"), 1},
        {QStringLiteral("id"), id}, {QStringLiteral("ok"), error.isEmpty()}};
    if (!error.isEmpty()) result.insert(QStringLiteral("message"), error);
    return result;
}
}
