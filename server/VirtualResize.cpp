// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualResize.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cmath>

namespace KRdp::VirtualResize
{
namespace
{
bool token(const QString &s)
{
    if (s.isEmpty() || s.size() > 128) return false;
    for (QChar c : s) {
        if (!((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'-' || c == u'_')) return false;
    }
    return true;
}
bool integer(const QJsonValue &v, int low, int high)
{
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= low && v.toDouble() <= high
        && std::floor(v.toDouble()) == v.toDouble();
}
}

bool validRequest(QSize pixels, double scale)
{
    return pixels.width() >= 320 && pixels.width() <= 4096 && pixels.height() >= 200 && pixels.height() <= 4096
        && pixels.width() % 2 == 0 && pixels.height() % 2 == 0 && std::isfinite(scale) && scale >= 1 && scale <= 4;
}

double normalizedScale(double scale)
{
    return std::round(scale * 120.0) / 120.0;
}

bool sameScale(double a, double b)
{
    // KWin uses 1/120 scale steps, transported through wl_fixed (1/256).
    // Less than half a scale step tolerates that transport, not the next step.
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1.0 / 256.0 + 0.000001;
}

std::optional<Snapshot> snapshot(const QByteArray &json, QString *error)
{
    const auto fail = [error](const QString &message) -> std::optional<Snapshot> {
        if (error) *error = message;
        return {};
    };
    if (json.isEmpty() || json.size() > 1024 * 1024) return fail(QStringLiteral("virtual output snapshot size invalid"));
    const auto doc = QJsonDocument::fromJson(json);
    const auto outputs = doc.object().value(QStringLiteral("outputs"));
    if (!doc.isObject() || !outputs.isArray() || outputs.toArray().size() != 1 || !outputs.toArray().first().isObject())
        return fail(QStringLiteral("virtual Fit requires exactly one private output"));
    const auto obj = outputs.toArray().first().toObject();
    Snapshot result;
    result.name = obj.value(QStringLiteral("name")).toString();
    const auto id = obj.value(QStringLiteral("id"));
    const auto pos = obj.value(QStringLiteral("pos")).toObject();
    const auto scale = obj.value(QStringLiteral("scale"));
    const auto rotation = obj.value(QStringLiteral("rotation"));
    if (!token(result.name) || !integer(id, 0, 2147483647)
        || !integer(pos.value(QStringLiteral("x")), -32768, 32768) || !integer(pos.value(QStringLiteral("y")), -32768, 32768)
        || !scale.isDouble() || !std::isfinite(scale.toDouble()) || scale.toDouble() < 1 || scale.toDouble() > 4
        || !integer(rotation, 1, 1) || obj.value(QStringLiteral("connected")) != QJsonValue(true)
        || obj.value(QStringLiteral("enabled")) != QJsonValue(true))
        return fail(QStringLiteral("virtual output identity, orientation or geometry invalid"));
    result.id = id.toInt();
    result.position = QPoint(pos.value(QStringLiteral("x")).toInt(), pos.value(QStringLiteral("y")).toInt());
    result.scale = scale.toDouble();
    const auto list = obj.value(QStringLiteral("modes"));
    if (!list.isArray() || list.toArray().isEmpty() || list.toArray().size() > 512)
        return fail(QStringLiteral("virtual output modes missing or oversized"));
    QSet<QString> ids;
    const QString current = obj.value(QStringLiteral("currentModeId")).toString();
    for (const auto value : list.toArray()) {
        if (!value.isObject()) return fail(QStringLiteral("invalid virtual mode"));
        const auto mode = value.toObject();
        Mode parsed;
        parsed.id = mode.value(QStringLiteral("id")).toString();
        const auto size = mode.value(QStringLiteral("size")).toObject();
        const auto refresh = mode.value(QStringLiteral("refreshRate"));
        if (!token(parsed.id) || ids.contains(parsed.id) || !integer(size.value(QStringLiteral("width")), 1, 32768)
            || !integer(size.value(QStringLiteral("height")), 1, 32768) || !refresh.isDouble()
            || !std::isfinite(refresh.toDouble()) || refresh.toDouble() < 0.001 || refresh.toDouble() > 1000)
            return fail(QStringLiteral("invalid or duplicate virtual mode"));
        parsed.pixels = QSize(size.value(QStringLiteral("width")).toInt(), size.value(QStringLiteral("height")).toInt());
        parsed.refresh = qRound(refresh.toDouble() * 1000.0);
        ids.insert(parsed.id);
        result.modes.append(parsed);
        if (parsed.id == current) result.current = parsed;
    }
    if (result.current.id.isEmpty()) return fail(QStringLiteral("current virtual mode is not advertised"));
    // ConfigSerializer in libkscreen 6.6.4 omits capabilities/custom-mode flags.
    // No fabricated JSON field is used here; new-mode support is verified by
    // fresh discovery after the fixed addCustomMode operation.
    return result;
}

bool sameOutput(const Snapshot &a, const Snapshot &b)
{
    return a.name == b.name && a.id == b.id && a.position == b.position;
}

bool matches(const Snapshot &state, const Plan &plan)
{
    return sameOutput(state, plan.before) && state.current.pixels == plan.pixels
        && state.current.refresh == plan.before.current.refresh && sameScale(state.scale, plan.scale);
}

std::optional<Mode> matchingMode(const Snapshot &state, QSize pixels, int refresh)
{
    if (state.current.pixels == pixels && state.current.refresh == refresh) return state.current;
    for (const auto &mode : state.modes) {
        if (mode.pixels == pixels && mode.refresh == refresh) return mode;
    }
    return {};
}

QStringList select(const Snapshot &state, const Mode &mode, double scale)
{
    return {QStringLiteral("output.%1.mode.%2").arg(state.name, mode.id),
            QStringLiteral("output.%1.scale.%2").arg(state.name, QString::number(scale, 'g', 12))};
}

QStringList add(const Plan &plan)
{
    return {QStringLiteral("output.%1.addCustomMode.%2.%3.%4.full").arg(plan.before.name)
                .arg(plan.pixels.width()).arg(plan.pixels.height()).arg(plan.before.current.refresh)};
}
}
