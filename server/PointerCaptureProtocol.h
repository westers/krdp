// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>
namespace KRdp::PointerCaptureProtocol {
inline bool number(const QJsonValue &v, bool zero = false) {
    bool ok = false; const auto s = v.toString(); const auto n = s.toULongLong(&ok);
    return v.isString() && ok && (zero || n) && QString::number(n) == s;
}
inline bool epoch(const QJsonValue &v) { return v.isString() && !QUuid(v.toString()).isNull() && v.toString().size() == 36; }
inline bool request(const QJsonObject &o) {
    static const QRegularExpression id(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return QJsonDocument(o).toJson(QJsonDocument::Compact).size() <= 4096 && o.value(QStringLiteral("v")) == 1 && epoch(o.value(QStringLiteral("epoch")))
        && number(o.value(QStringLiteral("generation"))) && o.value(QStringLiteral("enabled")).isBool()
        && id.match(o.value(QStringLiteral("id")).toString()).hasMatch();
}
inline bool state(const QJsonObject &o) {
    if (o.value(QStringLiteral("v")) != 1 || !number(o.value(QStringLiteral("generation")), true)
        || !o.value(QStringLiteral("supported")).isBool()) return false;
    if (!o.value(QStringLiteral("supported")).toBool()) return o.value(QStringLiteral("error")).toString().size() <= 1024;
    if (!epoch(o.value(QStringLiteral("epoch"))) || !number(o.value(QStringLiteral("revision")), true)) return false;
    for (const auto key : {"requested", "locked", "permitted", "leased", "blocked"})
        if (!o.value(QLatin1String(key)).isBool()) return false;
    return true;
}
inline QJsonObject decode(const QByteArray &bytes) {
    if (bytes.size() > 4096) return {};
    return QJsonDocument::fromJson(bytes).object();
}
}
