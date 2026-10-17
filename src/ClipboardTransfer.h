// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cmath>
#include <optional>

#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>

#include "ClipboardFilePacing.h"

namespace KRdp::ClipboardTransfer
{
/**
 * OPT-078 (spec 7): the KRDPCTL `clipboard-transfer` record (v1), modelled on `audio-priority`: the client says how much of the link
 * its file transfers may take, the host answers with the effective policy (the client's request under the host owner's ceiling).
 *
 *   -> {type:"clipboard-transfer", v:1, id, mode:"auto"|"balanced"|"cap"|"unlimited", capBytesPerSecond?}
 *   <- {type, v:1, id, ok, effective:{mode, capBytesPerSecond?}, ceiling:{mode, capBytesPerSecond?}, message}
 *
 * Strict: the id is 1..64 characters; `capBytesPerSecond` is present exactly for "cap" and between 100 kB/s and 1 GB/s (the client's
 * 0.1 to 1000 MB/s). Unlimited is accepted but only honoured without a ceiling (the reply says what is in force).
 *
 * The host owner's `ClipboardFileMaxRate` setting: `auto` (no ceiling), `balanced`, or a rate in MB/s (decimal, 0.1 to 1000).
 */
constexpr quint64 MinCapBytesPerSecond = 100'000;
constexpr quint64 MaxCapBytesPerSecond = 1'000'000'000;

struct Request {
    QString id;
    ClipboardFilePacing::RequestedMode mode = ClipboardFilePacing::RequestedMode::Auto;
    std::optional<quint64> capBytesPerSecond;
};

inline std::optional<Request> parse(const QJsonObject &record)
{
    const auto id = record.value(QStringLiteral("id"));
    const auto mode = record.value(QStringLiteral("mode"));
    if (record.value(QStringLiteral("type")) != QJsonValue(QStringLiteral("clipboard-transfer")) || record.value(QStringLiteral("v")) != QJsonValue(1)
        || !id.isString() || id.toString().isEmpty() || id.toString().size() > 64 || !mode.isString()) {
        return std::nullopt;
    }
    Request request;
    request.id = id.toString();
    const QString name = mode.toString();
    const auto cap = record.value(QStringLiteral("capBytesPerSecond"));
    if (name == QLatin1String("cap")) {
        if (!cap.isDouble()) return std::nullopt;
        const double value = cap.toDouble();
        if (!std::isfinite(value) || value < double(MinCapBytesPerSecond) || value > double(MaxCapBytesPerSecond) || value != std::floor(value)) return std::nullopt;
        request.mode = ClipboardFilePacing::RequestedMode::FixedCap;
        request.capBytesPerSecond = quint64(value);
        return request;
    }
    if (!cap.isUndefined()) return std::nullopt; // a rate only goes with "cap"
    if (name == QLatin1String("auto")) request.mode = ClipboardFilePacing::RequestedMode::Auto;
    else if (name == QLatin1String("balanced")) request.mode = ClipboardFilePacing::RequestedMode::Balanced;
    else if (name == QLatin1String("unlimited")) request.mode = ClipboardFilePacing::RequestedMode::Unlimited;
    else return std::nullopt;
    return request;
}

/// What a Policy is called on the wire.
inline QJsonObject describe(const ClipboardFilePacing::Policy &policy)
{
    if (policy.unlimited) return {{QStringLiteral("mode"), QStringLiteral("unlimited")}};
    if (policy.capBytesPerSecond) {
        return {{QStringLiteral("mode"), QStringLiteral("cap")}, {QStringLiteral("capBytesPerSecond"), qint64(*policy.capBytesPerSecond)}};
    }
    if (policy.balanced) return {{QStringLiteral("mode"), QStringLiteral("balanced")}};
    return {{QStringLiteral("mode"), QStringLiteral("auto")}};
}

inline QJsonObject describe(const ClipboardFilePacing::Ceiling &ceiling)
{
    switch (ceiling.kind) {
    case ClipboardFilePacing::Ceiling::Kind::Balanced: return {{QStringLiteral("mode"), QStringLiteral("balanced")}};
    case ClipboardFilePacing::Ceiling::Kind::Rate:
        return {{QStringLiteral("mode"), QStringLiteral("cap")}, {QStringLiteral("capBytesPerSecond"), qint64(ceiling.bytesPerSecond)}};
    case ClipboardFilePacing::Ceiling::Kind::None: break;
    }
    return {{QStringLiteral("mode"), QStringLiteral("auto")}};
}

inline QJsonObject reply(const QJsonObject &request, const ClipboardFilePacing::Policy &effective, const ClipboardFilePacing::Ceiling &ceiling, const QString &error = {})
{
    return {{QStringLiteral("type"), QStringLiteral("clipboard-transfer")},
            {QStringLiteral("v"), 1},
            {QStringLiteral("id"), request.value(QStringLiteral("id")).toString().left(64)},
            {QStringLiteral("ok"), error.isEmpty()},
            {QStringLiteral("effective"), describe(effective)},
            {QStringLiteral("ceiling"), describe(ceiling)},
            {QStringLiteral("message"), error}};
}

// ---- the host owner's ceiling (ClipboardFileMaxRate) ----

/// The canonical spelling of a ClipboardFileMaxRate value: "auto", "balanced" or a decimal MB/s from 0.1 to 1000 (no sign, no exponent,
/// no leading zeros, up to three decimals). nullopt for anything else; no case folding and no trimming.
inline std::optional<QString> normalizeCeiling(const QString &value)
{
    if (value == QLatin1String("auto") || value == QLatin1String("balanced")) return value;
    static const QRegularExpression number(QStringLiteral("^(?:0|[1-9][0-9]{0,3})(?:\\.[0-9]{1,3})?$"));
    if (!number.match(value).hasMatch()) return std::nullopt;
    const double mb = value.toDouble();
    if (mb < 0.1 || mb > 1000.0) return std::nullopt;
    return value;
}

inline std::optional<ClipboardFilePacing::Ceiling> parseCeiling(const QString &value)
{
    const auto normal = normalizeCeiling(value);
    if (!normal) return std::nullopt;
    if (*normal == QLatin1String("auto")) return ClipboardFilePacing::Ceiling::none();
    if (*normal == QLatin1String("balanced")) return ClipboardFilePacing::Ceiling::balanced();
    return ClipboardFilePacing::Ceiling::rate(quint64(std::llround(normal->toDouble() * 1'000'000.0)));
}
}
