// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "BrokerHostAdmin.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <optional>

// Envelope that lets one privileged helper process save several host scopes
// (Console, Virtual, new-desktop hardware) for a single authorization. It adds
// no operation of its own: every entry is an ordinary one-scope "save" request
// that the helper validates and commits independently (own lock, own revision
// check, own reply), so a refusal in one scope never blocks or rolls back another.
namespace KRdp::BrokerHostBatch {
using namespace Qt::StringLiterals;
constexpr int MaximumRequests = 3;
constexpr qsizetype MaximumInputBytes = BrokerHostAdmin::MaximumRequestBytes * MaximumRequests;

inline QJsonObject request(const QList<QJsonObject> &saves)
{
    QJsonArray entries;
    for (const auto &save : saves) entries.append(save);
    return {{u"version"_s, 1}, {u"operation"_s, u"save-batch"_s}, {u"requests"_s, entries}};
}

// Helper side: the validated single save requests, or nothing.
inline std::optional<QList<QJsonObject>> parseRequest(const QJsonObject &value)
{
    if (value.size() != 3 || !value[u"version"_s].isDouble() || value[u"version"_s].toDouble() != 1
        || value[u"operation"_s].toString() != u"save-batch" || !value[u"requests"_s].isArray()) return std::nullopt;
    const auto entries = value[u"requests"_s].toArray();
    if (entries.isEmpty() || entries.size() > MaximumRequests) return std::nullopt;
    QList<QJsonObject> result;
    QStringList seen;
    for (const auto &entry : entries) {
        if (!entry.isObject()) return std::nullopt;
        const auto save = entry.toObject();
        const auto scope = save[u"scope"_s].toString();
        if (save[u"operation"_s].toString() != u"save" || !BrokerHostAdmin::scope(scope) || seen.contains(scope)
            || QJsonDocument(save).toJson(QJsonDocument::Compact).size() > BrokerHostAdmin::MaximumRequestBytes) return std::nullopt;
        seen.append(scope);
        result.append(save);
    }
    return result;
}

struct Entry {
    QString scope;
    int status = 1;
    QJsonObject reply;
};

// Client side: exactly one result per requested scope, in request order.
inline std::optional<QList<Entry>> parseReply(const QJsonObject &value, const QStringList &scopes)
{
    if (value.size() != 1 || !value[u"results"_s].isArray()) return std::nullopt;
    const auto results = value[u"results"_s].toArray();
    if (results.size() != scopes.size()) return std::nullopt;
    QList<Entry> entries;
    for (int i = 0; i < results.size(); ++i) {
        if (!results[i].isObject()) return std::nullopt;
        const auto item = results[i].toObject();
        if (item.size() != 3 || item[u"scope"_s].toString() != scopes[i] || !item[u"status"_s].isDouble()
            || !item[u"reply"_s].isObject()) return std::nullopt;
        entries.append({scopes[i], item[u"status"_s].toInt(), item[u"reply"_s].toObject()});
    }
    return entries;
}
}
