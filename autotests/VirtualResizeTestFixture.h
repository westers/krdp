// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualResizeExecutor.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <utility>

namespace VirtualResizeFixture
{
inline QJsonObject mode(QString id, int width, int height, double refresh = 60)
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("refreshRate"), refresh},
            {QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), width}, {QStringLiteral("height"), height}}}};
}
inline QJsonObject output(bool includeRequested = true)
{
    QJsonArray modes{mode(QStringLiteral("1"), 1280, 720)};
    if (includeRequested) modes.append(mode(QStringLiteral("2"), 1920, 1080));
    return {{QStringLiteral("name"), QStringLiteral("Virtual-0")}, {QStringLiteral("id"), 1},
            {QStringLiteral("connected"), true}, {QStringLiteral("enabled"), true}, {QStringLiteral("rotation"), 1},
            {QStringLiteral("pos"), QJsonObject{{QStringLiteral("x"), 0}, {QStringLiteral("y"), 0}}},
            {QStringLiteral("scale"), 1}, {QStringLiteral("currentModeId"), QStringLiteral("1")}, {QStringLiteral("modes"), modes}};
}
inline QByteArray json(const QJsonObject &out)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("outputs"), QJsonArray{out}}}).toJson(QJsonDocument::Compact);
}
struct Pending {
    QStringList arguments;
    KRdp::VirtualResizeExecutor::Reply reply;
};
struct Deferred {
    QList<Pending> calls;
    QStringList history;
    KRdp::VirtualResizeExecutor::Runner runner() {
        return [this](const QStringList &args, auto reply) { history.append(args.join(u' ')); calls.append({args, std::move(reply)}); };
    }
    void answer(bool ok, const QByteArray &data = {}) {
        Q_ASSERT(!calls.isEmpty());
        auto call = calls.takeFirst();
        call.reply(ok, data);
    }
    void state(const QJsonObject &out) { answer(true, json(out)); }
};
}
