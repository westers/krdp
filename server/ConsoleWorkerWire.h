// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>
#include <cmath>
#include <utility>

#include <QByteArray>
#include <QDataStream>
#include <QEvent>
#include <QIODevice>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QSet>

#include "VideoFrame.h"

namespace KRdp::ConsoleWorkerWire
{
constexpr quint16 ProtocolVersion = 2;
constexpr quint32 MaxRecordBytes = 64 * 1024 * 1024;

enum class Kind : quint8 {
    Hello = 1,
    Ready,
    Frame,
    Input,
    Stop,
    RequestKeyFrame,
    Media,
    Audio,
    Error,
    Outputs,
    ControlState,
    LocalTakeover,
    Resize,
    ResizeResult,
    VideoQuality,
    MicrophonePolicy,
    MicrophoneResult,
    MicrophoneAudio,
    Position,
    PositionResult,
    AddVirtual,
    AddVirtualResult,
    RemoveVirtual,
    RemoveVirtualResult,
};

struct Record {
    Kind kind = Kind::Error;
    QByteArray payload;
    bool operator==(const Record &) const = default;
};

inline QByteArray frame(Kind kind, const QByteArray &payload = {});

struct Resize {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString output;
    QSize pixels;
    double scale = 1;
    bool operator==(const Resize &) const = default;
};

struct ResizeResult {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString error; // Empty only after verified mode/scale readback.
    bool operator==(const ResizeResult &) const = default;
};

struct Position {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString output;
    QPoint globalLogical;
    bool operator==(const Position &) const = default;
};

struct PositionResult {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString error; // Empty only after fresh KScreen readback and captured keyframes.
    bool operator==(const PositionResult &) const = default;
};

struct AddVirtual {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString output; // Full, unique compositor name, prefixed Virtual-.
    QSize pixels;
    double scale = 1;
    QPoint globalLogical;
    bool operator==(const AddVirtual &) const = default;
};

struct AddVirtualResult {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString error; // Empty only after independent KScreen and all new encoded keyframes agree.
    bool operator==(const AddVirtualResult &) const = default;
};

struct RemoveVirtual {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString output; // Must be an Add-created output owned by this live worker.
    bool operator==(const RemoveVirtual &) const = default;
};

struct RemoveVirtualResult {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString error;
    bool operator==(const RemoveVirtualResult &) const = default;
};

inline QByteArray frame(const RemoveVirtual &request)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.requestId << request.generation << request.output;
    return frame(Kind::RemoveVirtual, payload);
}

inline std::optional<RemoveVirtual> removeVirtual(const Record &record)
{
    if (record.kind != Kind::RemoveVirtual || record.payload.size() > 512) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    RemoveVirtual request;
    stream >> request.requestId >> request.generation >> request.output;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !request.requestId || !request.generation
        || !request.output.startsWith(QStringLiteral("Virtual-krdp-added-"))
        || request.output.size() <= QStringLiteral("Virtual-krdp-added-").size() || request.output.size() > 128) return {};
    for (const auto character : request.output) {
        if (!character.isLetterOrNumber() && character != QLatin1Char('-') && character != QLatin1Char('_')) return {};
    }
    return request;
}

inline QByteArray frame(const RemoveVirtualResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.requestId << result.generation << result.error;
    return frame(Kind::RemoveVirtualResult, payload);
}

inline std::optional<RemoveVirtualResult> removeVirtualResult(const Record &record)
{
    if (record.kind != Kind::RemoveVirtualResult || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    RemoveVirtualResult result;
    stream >> result.requestId >> result.generation >> result.error;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !result.requestId || !result.generation
        || result.error.size() > 1024) return {};
    return result;
}

inline QByteArray frame(const AddVirtual &request)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.requestId << request.generation << request.output << request.pixels << request.scale << request.globalLogical;
    return frame(Kind::AddVirtual, payload);
}

inline std::optional<AddVirtual> addVirtual(const Record &record)
{
    if (record.kind != Kind::AddVirtual || record.payload.size() > 512) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    AddVirtual request;
    stream >> request.requestId >> request.generation >> request.output >> request.pixels >> request.scale >> request.globalLogical;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !request.requestId || !request.generation
        || !request.output.startsWith(QStringLiteral("Virtual-")) || request.output.size() <= 8 || request.output.size() > 128
        || request.pixels.width() < 320 || request.pixels.width() > 4096
        || request.pixels.height() < 200 || request.pixels.height() > 4096
        || !std::isfinite(request.scale) || request.scale < 1 || request.scale > 4
        || request.globalLogical.x() < -32768 || request.globalLogical.x() > 32768
        || request.globalLogical.y() < -32768 || request.globalLogical.y() > 32768) return {};
    for (const auto character : request.output) {
        if (!character.isLetterOrNumber() && character != QLatin1Char('-') && character != QLatin1Char('_')) return {};
    }
    return request;
}

inline QByteArray frame(const AddVirtualResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.requestId << result.generation << result.error;
    return frame(Kind::AddVirtualResult, payload);
}

inline std::optional<AddVirtualResult> addVirtualResult(const Record &record)
{
    if (record.kind != Kind::AddVirtualResult || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    AddVirtualResult result;
    stream >> result.requestId >> result.generation >> result.error;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !result.requestId || !result.generation
        || result.error.size() > 1024) return {};
    return result;
}

inline QByteArray frame(const Position &request)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.requestId << request.generation << request.output << request.globalLogical;
    return frame(Kind::Position, payload);
}

inline std::optional<Position> position(const Record &record)
{
    if (record.kind != Kind::Position || record.payload.size() > 512) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    Position request;
    stream >> request.requestId >> request.generation >> request.output >> request.globalLogical;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !request.requestId || !request.generation
        || request.output.isEmpty() || request.output.size() > 128
        || request.globalLogical.x() < -32768 || request.globalLogical.x() > 32768
        || request.globalLogical.y() < -32768 || request.globalLogical.y() > 32768) return {};
    return request;
}

inline QByteArray frame(const PositionResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.requestId << result.generation << result.error;
    return frame(Kind::PositionResult, payload);
}

inline std::optional<PositionResult> positionResult(const Record &record)
{
    if (record.kind != Kind::PositionResult || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    PositionResult result;
    stream >> result.requestId >> result.generation >> result.error;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !result.requestId || !result.generation
        || result.error.size() > 1024) return {};
    return result;
}

inline QByteArray frame(const Resize &request)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.requestId << request.generation << request.output << request.pixels << request.scale;
    return frame(Kind::Resize, payload);
}

inline std::optional<Resize> resize(const Record &record)
{
    if (record.kind != Kind::Resize || record.payload.size() > 512) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    Resize request;
    stream >> request.requestId >> request.generation >> request.output >> request.pixels >> request.scale;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !request.requestId || !request.generation
        || request.output.isEmpty() || request.output.size() > 128 || request.pixels.width() < 320 || request.pixels.height() < 200
        || request.pixels.width() > 4096 || request.pixels.height() > 4096 || !std::isfinite(request.scale) || request.scale < 1 || request.scale > 4) {
        return std::nullopt;
    }
    return request;
}

inline QByteArray frame(const ResizeResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.requestId << result.generation << result.error;
    return frame(Kind::ResizeResult, payload);
}

inline std::optional<ResizeResult> resizeResult(const Record &record)
{
    if (record.kind != Kind::ResizeResult || record.payload.size() > 4096) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    ResizeResult result;
    stream >> result.requestId >> result.generation >> result.error;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !result.requestId || !result.generation || result.error.size() > 1024) {
        return std::nullopt;
    }
    return result;
}

struct ControlState {
    quint64 generation = 0;
    bool active = false;
    bool operator==(const ControlState &) const = default;
};

struct VideoQuality {
    quint64 generation = 0;
    quint8 quality = 80;
    bool operator==(const VideoQuality &) const = default;
};

inline QByteArray frame(const VideoQuality &quality)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quality.generation << quality.quality;
    return frame(Kind::VideoQuality, payload);
}

inline std::optional<VideoQuality> videoQuality(const Record &record)
{
    if (record.kind != Kind::VideoQuality || record.payload.size() != 9) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    VideoQuality quality;
    stream >> quality.generation >> quality.quality;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !quality.generation || quality.quality < 10 || quality.quality > 100) {
        return std::nullopt;
    }
    return quality;
}

inline bool mayApplyQuality(const VideoQuality &quality, const ControlState &control)
{
    return control.active && control.generation != 0 && quality.generation == control.generation
        && quality.quality >= 10 && quality.quality <= 100;
}

inline QByteArray frame(const ControlState &state, Kind kind = Kind::ControlState)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << state.generation << state.active;
    return frame(kind, payload);
}

inline std::optional<ControlState> controlState(const Record &record, Kind kind = Kind::ControlState)
{
    if (record.kind != kind) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    ControlState state;
    stream >> state.generation >> state.active;
    return stream.status() == QDataStream::Ok && stream.atEnd() ? std::optional<ControlState>(state) : std::nullopt;
}

struct Output {
    QString name;
    QRect geometry; // Logical coordinates, normalized to the captured workspace.
    double scale = 1;
    bool primary = false;
    bool operator==(const Output &) const = default;
};

struct Outputs {
    QVector<Output> monitors;
    QPoint compositorOrigin = QPoint(0, 0); // Global KWin top-left; monitor geometry is RDP-normalized.
    bool operator==(const Outputs &) const = default;
};

inline QByteArray frame(const Outputs &outputs)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(outputs.monitors.size());
    for (const auto &output : outputs.monitors) {
        stream << output.name << output.geometry << output.scale << output.primary;
    }
    stream << outputs.compositorOrigin;
    return frame(Kind::Outputs, payload);
}

inline std::optional<Outputs> outputs(const Record &record)
{
    if (record.kind != Kind::Outputs || record.payload.size() > 32768) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 count = 0;
    stream >> count;
    if (!count || count > 32) {
        return std::nullopt;
    }
    Outputs result;
    QSet<QString> names;
    for (quint32 i = 0; i < count; ++i) {
        Output output;
        stream >> output.name >> output.geometry >> output.scale >> output.primary;
        if (stream.status() != QDataStream::Ok || output.name.isEmpty() || output.name.size() > 256 || names.contains(output.name)
            || output.geometry.x() < 0 || output.geometry.y() < 0 || output.geometry.x() > 32768 || output.geometry.y() > 32768
            || output.geometry.width() <= 0 || output.geometry.height() <= 0 || output.geometry.width() > 32768 || output.geometry.height() > 32768
            || !std::isfinite(output.scale) || output.scale < 0.25 || output.scale > 8) {
            return std::nullopt;
        }
        names.insert(output.name);
        result.monitors.append(output);
    }
    stream >> result.compositorOrigin;
    if (stream.status() != QDataStream::Ok || result.compositorOrigin.x() < -32768 || result.compositorOrigin.x() > 32768
        || result.compositorOrigin.y() < -32768 || result.compositorOrigin.y() > 32768) return std::nullopt;
    return stream.atEnd() ? std::optional<Outputs>(result) : std::nullopt;
}

/** Authenticates one worker to the broker endpoint created for its logind session. */
struct Hello {
    QString sessionId;
    quint32 uid = 0;
    QByteArray token;
    bool operator==(const Hello &) const = default;
};

/** Explicit per-connection playback policy sent by the console host. */
struct Media {
    bool playback = false;
    bool silenceHost = false;
    bool operator==(const Media &) const = default;
};

inline QByteArray frame(const Media &media)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << media.playback << media.silenceHost;
    return frame(Kind::Media, payload);
}

inline std::optional<Media> media(const Record &record)
{
    if (record.kind != Kind::Media) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    Media result;
    stream >> result.playback >> result.silenceHost;
    return stream.status() == QDataStream::Ok && stream.atEnd() ? std::optional<Media>(result) : std::nullopt;
}

/** 44.1 kHz stereo S16 PCM, bounded to one or a few 20-ms RDPSND packets. */
struct Audio {
    QByteArray pcm;
    bool operator==(const Audio &) const = default;
};

inline QByteArray frame(const Audio &audio)
{
    return frame(Kind::Audio, audio.pcm);
}

inline std::optional<Audio> audio(const Record &record)
{
    if (record.kind != Kind::Audio || record.payload.isEmpty() || record.payload.size() > 32768 || record.payload.size() % 4 != 0) {
        return std::nullopt;
    }
    return Audio{record.payload};
}

inline QByteArray frame(const Hello &hello)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << hello.sessionId << hello.uid << hello.token;
    return frame(Kind::Hello, payload);
}

inline std::optional<Hello> hello(const Record &record)
{
    if (record.kind != Kind::Hello) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    Hello result;
    stream >> result.sessionId >> result.uid >> result.token;
    return stream.status() == QDataStream::Ok && stream.atEnd() && !result.sessionId.isEmpty() && result.uid != 0 && !result.token.isEmpty()
        ? std::optional<Hello>(result)
        : std::nullopt;
}

/** Input independent of Qt object lifetimes, valid across the broker socket. */
struct Input {
    enum class Type : quint8 {
        Mouse,
        Wheel,
        Key,
    };

    Type type = Type::Mouse;
    QEvent::Type eventType = QEvent::None;
    QPointF position;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    QPoint angleDelta;
    quint32 nativeScanCode = 0;
    quint32 nativeVirtualKey = 0;
    QString text;
    bool operator==(const Input &) const = default;
};

/** Serialises one record as a big-endian bounded length prefix + body. */
inline QByteArray frame(Kind kind, const QByteArray &payload)
{
    QByteArray body;
    QDataStream stream(&body, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << ProtocolVersion << quint8(kind) << payload;
    QByteArray result;
    QDataStream header(&result, QIODevice::WriteOnly);
    header.setByteOrder(QDataStream::BigEndian);
    header << quint32(body.size());
    result.append(body);
    return result;
}

inline QByteArray frame(const VideoFrame &video)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << video.size << video.data << video.aux << video.isKeyFrame << video.auxIsKeyFrame << qint32(video.monitorIndex) << quint32(video.monitors.size());
    for (const VideoMonitor &monitor : video.monitors) {
        stream << monitor.geometry << monitor.primary;
    }
    return frame(Kind::Frame, payload);
}

inline std::optional<VideoFrame> videoFrame(const Record &record)
{
    if (record.kind != Kind::Frame) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    VideoFrame video;
    qint32 monitorIndex = 0;
    quint32 count = 0;
    stream >> video.size >> video.data >> video.aux >> video.isKeyFrame >> video.auxIsKeyFrame >> monitorIndex >> count;
    if (stream.status() != QDataStream::Ok || count > 16) {
        return std::nullopt;
    }
    video.monitorIndex = monitorIndex;
    video.monitors.reserve(qsizetype(count));
    for (quint32 i = 0; i < count; ++i) {
        VideoMonitor monitor;
        stream >> monitor.geometry >> monitor.primary;
        if (stream.status() != QDataStream::Ok) {
            return std::nullopt;
        }
        video.monitors.append(monitor);
    }
    return stream.atEnd() ? std::optional<VideoFrame>(video) : std::nullopt;
}

inline QByteArray frame(const Input &input)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint8(input.type) << qint32(input.eventType) << input.position << quint32(input.button) << quint32(input.buttons) << input.angleDelta
           << input.nativeScanCode << input.nativeVirtualKey << input.text;
    return frame(Kind::Input, payload);
}

inline std::optional<Input> input(const Record &record)
{
    if (record.kind != Kind::Input) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    quint8 type = 0;
    qint32 eventType = 0;
    quint32 button = 0;
    quint32 buttons = 0;
    Input result;
    stream >> type >> eventType >> result.position >> button >> buttons >> result.angleDelta >> result.nativeScanCode >> result.nativeVirtualKey >> result.text;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || type > quint8(Input::Type::Key)
        || eventType < int(QEvent::None) || eventType > int(QEvent::User)) {
        return std::nullopt;
    }
    result.type = Input::Type(type);
    result.eventType = QEvent::Type(eventType);
    result.button = Qt::MouseButton(button);
    result.buttons = Qt::MouseButtons(buttons);
    return result;
}

/** Reassembles records arriving split or coalesced on a Unix stream socket. */
class Deframer
{
public:
    void feed(const QByteArray &data)
    {
        if (!m_overflowed) {
            m_buffer.append(data);
        }
    }

    std::optional<Record> next()
    {
        if (m_overflowed || m_buffer.size() < 4) {
            return std::nullopt;
        }
        QDataStream header(m_buffer);
        header.setByteOrder(QDataStream::BigEndian);
        quint32 length = 0;
        header >> length;
        if (length > MaxRecordBytes) {
            m_overflowed = true;
            m_buffer.clear();
            return std::nullopt;
        }
        if (m_buffer.size() < 4 + qsizetype(length)) {
            return std::nullopt;
        }
        const QByteArray body = m_buffer.mid(4, length);
        m_buffer.remove(0, 4 + length);
        QDataStream stream(body);
        stream.setByteOrder(QDataStream::BigEndian);
        quint16 version = 0;
        quint8 type = 0;
        QByteArray payload;
        stream >> version >> type >> payload;
        if (stream.status() != QDataStream::Ok || !stream.atEnd() || version != ProtocolVersion || type < quint8(Kind::Hello) || type > quint8(Kind::RemoveVirtualResult)) {
            ++m_invalid;
            return std::nullopt;
        }
        return Record{Kind(type), payload};
    }

    bool overflowed() const { return m_overflowed; }
    int takeInvalidCount() { return std::exchange(m_invalid, 0); }

private:
    QByteArray m_buffer;
    bool m_overflowed = false;
    int m_invalid = 0;
};
}
