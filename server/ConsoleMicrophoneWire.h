// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "ConsoleWorkerWire.h"

namespace KRdp::ConsoleWorkerWire
{
struct MicrophonePolicy {
    quint64 generation = 0; // Controller ownership epoch.
    quint64 requestId = 0; // Unique activation, also changes on same-owner off/on.
    bool enabled = false;
    bool operator==(const MicrophonePolicy &) const = default;
};
struct MicrophoneResult {
    quint64 generation = 0;
    quint64 requestId = 0;
    QString error;
    bool operator==(const MicrophoneResult &) const = default;
};
struct MicrophoneAudio {
    quint64 generation = 0;
    quint64 requestId = 0;
    QByteArray pcm; // 48kHz stereo S16LE, max20ms.
    bool operator==(const MicrophoneAudio &) const = default;
};
inline QByteArray frame(const MicrophonePolicy &value)
{
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s << value.generation << value.requestId << quint8(value.enabled);
    return frame(Kind::MicrophonePolicy, bytes);
}
inline std::optional<MicrophonePolicy> microphonePolicy(const Record &record)
{
    if (record.kind != Kind::MicrophonePolicy || record.payload.size() != 17) return {};
    QDataStream s(record.payload);
    MicrophonePolicy value;
    quint8 enabled = 0;
    s >> value.generation >> value.requestId >> enabled;
    if (s.status() != QDataStream::Ok || !s.atEnd() || !value.generation || !value.requestId || enabled > 1) return {};
    value.enabled = enabled;
    return value;
}
inline QByteArray frame(const MicrophoneResult &value)
{
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s << value.generation << value.requestId << value.error;
    return frame(Kind::MicrophoneResult, bytes);
}
inline std::optional<MicrophoneResult> microphoneResult(const Record &record)
{
    if (record.kind != Kind::MicrophoneResult || record.payload.size() > 4096) return {};
    QDataStream s(record.payload);
    MicrophoneResult value;
    s >> value.generation >> value.requestId >> value.error;
    if (s.status() != QDataStream::Ok || !s.atEnd() || !value.generation || !value.requestId || value.error.size() > 1024) return {};
    return value;
}
inline QByteArray frame(const MicrophoneAudio &value)
{
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s << value.generation << value.requestId << value.pcm;
    return frame(Kind::MicrophoneAudio, bytes);
}
inline std::optional<MicrophoneAudio> microphoneAudio(const Record &record)
{
    if (record.kind != Kind::MicrophoneAudio || record.payload.size() > 3860) return {};
    QDataStream s(record.payload);
    MicrophoneAudio value;
    s >> value.generation >> value.requestId >> value.pcm;
    if (s.status() != QDataStream::Ok || !s.atEnd() || !value.generation || !value.requestId
        || value.pcm.isEmpty() || value.pcm.size() > 3840 || value.pcm.size() % 4) return {};
    return value;
}
}
