// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector>

namespace KRdp::PipeWireAudioRouting
{
struct PlaybackStream {
    quint32 id = 0;
    bool operator==(const PlaybackStream &) const = default;
};

/** Select only movable audio-output stream nodes from a `pw-dump` document. */
inline QVector<PlaybackStream> movablePlaybackStreams(const QByteArray &dump)
{
    const QJsonDocument document = QJsonDocument::fromJson(dump);
    if (!document.isArray()) {
        return {};
    }
    QVector<PlaybackStream> result;
    for (const QJsonValue &value : document.array()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("type")).toString() != QLatin1String("PipeWire:Interface:Node")) {
            continue;
        }
        const QJsonObject properties = object.value(QStringLiteral("info")).toObject().value(QStringLiteral("props")).toObject();
        if (properties.value(QStringLiteral("media.class")).toString() != QLatin1String("Stream/Output/Audio")
            || properties.value(QStringLiteral("node.dont-move")).toString() == QLatin1String("true")) {
            continue;
        }
        const int id = object.value(QStringLiteral("id")).toInt(-1);
        if (id >= 0) {
            result.append(PlaybackStream{quint32(id)});
        }
    }
    return result;
}
}
