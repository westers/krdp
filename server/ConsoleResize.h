// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSize>
#include <QStringList>
#include <cmath>
#include <limits>

namespace KRdp::ConsoleResize
{
// Planning only: kscreen-doctor must run inside the selected worker's session,
// never in the privileged broker. Native pixels and desktop scale are separate.
struct Plan {
    QString error;
    QString output;
    QString mode;
    QString previousMode;
    double scale = 1;
    double previousScale = 1;
    QStringList apply;
    QStringList restore;
    bool valid() const { return error.isEmpty() && !apply.isEmpty(); }
};

inline bool safeToken(const QString &value)
{
    if (value.isEmpty() || value.size() > 128) {
        return false;
    }
    for (const QChar c : value) {
        if (!((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'-' || c == u'_')) {
            return false;
        }
    }
    return true;
}

// Readback predicate for successful apply and conditional restore. A local
// change means restoration must not overwrite the person's new choice.
inline bool matches(const QByteArray &snapshot, const Plan &plan, bool original = false)
{
    if (!plan.valid()) {
        return false;
    }
    const auto document = QJsonDocument::fromJson(snapshot);
    int found = 0;
    bool match = false;
    for (const auto entry : document.object().value(QStringLiteral("outputs")).toArray()) {
        const auto output = entry.toObject();
        if (output.value(QStringLiteral("name")).toString() != plan.output) {
            continue;
        }
        ++found;
        const double scale = output.value(QStringLiteral("scale")).toDouble(0);
        match = output.value(QStringLiteral("connected")).toBool() && output.value(QStringLiteral("enabled")).toBool()
            && output.value(QStringLiteral("currentModeId")).toString() == (original ? plan.previousMode : plan.mode)
            && std::isfinite(scale) && std::abs(scale - (original ? plan.previousScale : plan.scale)) < 0.000001;
    }
    return found == 1 && match;
}

inline Plan plan(const QByteArray &snapshot, const QString &name, QSize pixels, double scale)
{
    Plan result;
    const auto fail = [&result](const QString &error) { result.error = error; return result; };
    if (!safeToken(name) || name.startsWith(QStringLiteral("Virtual-")) || pixels.width() < 320 || pixels.height() < 200
        || pixels.width() > 4096 || pixels.height() > 4096 || !std::isfinite(scale) || scale < 1 || scale > 4) {
        return fail(QStringLiteral("invalid physical output, pixel size or scale"));
    }
    const auto document = QJsonDocument::fromJson(snapshot);
    if (!document.isObject() || !document.object().value(QStringLiteral("outputs")).isArray()) {
        return fail(QStringLiteral("invalid output snapshot"));
    }
    QJsonObject output;
    for (const auto entry : document.object().value(QStringLiteral("outputs")).toArray()) {
        const auto candidate = entry.toObject();
        if (candidate.value(QStringLiteral("name")).toString() == name) {
            if (!output.isEmpty()) {
                return fail(QStringLiteral("ambiguous output name"));
            }
            output = candidate;
        }
    }
    if (output.isEmpty() || !output.value(QStringLiteral("connected")).toBool() || !output.value(QStringLiteral("enabled")).toBool()) {
        return fail(QStringLiteral("physical output is not connected and enabled"));
    }
    result.output = name;
    result.previousMode = output.value(QStringLiteral("currentModeId")).toString();
    result.previousScale = output.value(QStringLiteral("scale")).toDouble(0);
    result.scale = scale;
    if (!safeToken(result.previousMode) || !std::isfinite(result.previousScale) || result.previousScale <= 0) {
        return fail(QStringLiteral("cannot record the current mode and scale for rollback"));
    }
    const auto modes = output.value(QStringLiteral("modes")).toArray();
    double previousRefresh = 0;
    for (const auto entry : modes) {
        const auto mode = entry.toObject();
        if (mode.value(QStringLiteral("id")).toString() == result.previousMode) {
            previousRefresh = mode.value(QStringLiteral("refreshRate")).toDouble();
        }
    }
    if (!std::isfinite(previousRefresh) || previousRefresh <= 0) {
        return fail(QStringLiteral("current mode is missing from the output snapshot"));
    }
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const auto entry : modes) {
        const auto mode = entry.toObject();
        const auto size = mode.value(QStringLiteral("size")).toObject();
        const QString id = mode.value(QStringLiteral("id")).toString();
        const double refresh = mode.value(QStringLiteral("refreshRate")).toDouble();
        if (!safeToken(id) || !std::isfinite(refresh) || refresh <= 0
            || QSize(size.value(QStringLiteral("width")).toInt(), size.value(QStringLiteral("height")).toInt()) != pixels) {
            continue;
        }
        // A no-op preserves the exact mode; otherwise keep refresh as close as possible.
        const double distance = id == result.previousMode ? -1 : std::abs(refresh - previousRefresh);
        if (distance < bestDistance) {
            result.mode = id;
            bestDistance = distance;
        }
    }
    if (result.mode.isEmpty()) {
        return fail(QStringLiteral("requested resolution is not an advertised physical mode"));
    }
    const QString prefix = QStringLiteral("output.%1.").arg(name);
    result.apply = {prefix + QStringLiteral("mode.") + result.mode, prefix + QStringLiteral("scale.") + QString::number(scale, 'g', 12)};
    result.restore = {prefix + QStringLiteral("mode.") + result.previousMode,
                      prefix + QStringLiteral("scale.") + QString::number(result.previousScale, 'g', 12)};
    return result;
}
}
