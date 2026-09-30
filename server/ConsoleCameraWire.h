// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "ConsoleWorkerWire.h"

namespace KRdp::ConsoleWorkerWire
{
constexpr qsizetype MaxCameraJpegBytes = 8 * 1024 * 1024;

struct CameraPolicy {
    quint64 generation = 0;
    quint64 requestId = 0;
    bool enabled = false;
    QString loopbackDevice;
    bool operator==(const CameraPolicy &) const = default;
};

struct CameraFormat {
    quint64 generation = 0;
    quint64 requestId = 0;
    quint32 width = 0;
    quint32 height = 0;
    quint32 fps = 0;
    bool operator==(const CameraFormat &) const = default;
};

struct CameraFrame {
    quint64 generation = 0;
    quint64 requestId = 0;
    QByteArray jpeg;
    bool operator==(const CameraFrame &) const = default;
};

struct CameraResult {
    quint64 generation = 0;
    quint64 requestId = 0;
    QString error;
    bool operator==(const CameraResult &) const = default;
};

struct CameraDemand {
    quint64 generation = 0;
    quint64 requestId = 0;
    bool capture = false;
    bool inUse = false;
    bool operator==(const CameraDemand &) const = default;
};

inline QByteArray frame(const CameraPolicy &value)
{
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s << value.generation << value.requestId << quint8(value.enabled) << value.loopbackDevice;
    return frame(Kind::CameraPolicy, bytes);
}
inline std::optional<CameraPolicy> cameraPolicy(const Record &record)
{
    if (record.kind != Kind::CameraPolicy || record.payload.size() < 21 || record.payload.size() > 1045) return {};
    QDataStream s(record.payload);
    CameraPolicy value;
    quint8 enabled = 0;
    s >> value.generation >> value.requestId >> enabled >> value.loopbackDevice;
    if (s.status() != QDataStream::Ok || !s.atEnd() || !value.generation || !value.requestId || enabled > 1
        || value.loopbackDevice.size() > 512 || (!value.loopbackDevice.isEmpty()
            && !(value.loopbackDevice.startsWith(QLatin1String("/dev/video"))
                || value.loopbackDevice.startsWith(QLatin1String("/dev/v4l/"))))) return {};
    value.enabled = enabled;
    return value;
}

inline QByteArray frame(const CameraFormat &value)
{
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s << value.generation << value.requestId << value.width << value.height << value.fps;
    return frame(Kind::CameraFormat, bytes);
}
inline std::optional<CameraFormat> cameraFormat(const Record &record)
{
    if (record.kind != Kind::CameraFormat || record.payload.size() != 28) return {};
    QDataStream s(record.payload);
    CameraFormat value;
    s >> value.generation >> value.requestId >> value.width >> value.height >> value.fps;
    if (s.status() != QDataStream::Ok || !s.atEnd() || !value.generation || !value.requestId
        || !value.width || value.width > 4096 || !value.height || value.height > 4096
        || !value.fps || value.fps > 120) return {};
    return value;
}

inline QByteArray frame(const CameraFrame &value)
{
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s << value.generation << value.requestId << value.jpeg;
    return frame(Kind::CameraFrame, bytes);
}
inline std::optional<CameraFrame> cameraFrame(const Record &record)
{
    if (record.kind != Kind::CameraFrame || record.payload.size() > MaxCameraJpegBytes + 20) return {};
    QDataStream s(record.payload);
    CameraFrame value;
    s >> value.generation >> value.requestId >> value.jpeg;
    if (s.status() != QDataStream::Ok || !s.atEnd() || !value.generation || !value.requestId
        || value.jpeg.isEmpty() || value.jpeg.size() > MaxCameraJpegBytes
        || !value.jpeg.startsWith("\xFF\xD8") || !value.jpeg.endsWith("\xFF\xD9")) return {};
    return value;
}

inline QByteArray frame(const CameraResult &value)
{
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s << value.generation << value.requestId << value.error;
    return frame(Kind::CameraResult, bytes);
}
inline std::optional<CameraResult> cameraResult(const Record &record)
{
    if (record.kind != Kind::CameraResult || record.payload.size() > 4096) return {};
    QDataStream s(record.payload);
    CameraResult value;
    s >> value.generation >> value.requestId >> value.error;
    if (s.status() != QDataStream::Ok || !s.atEnd() || !value.generation || !value.requestId || value.error.size() > 1024) return {};
    return value;
}

inline QByteArray frame(const CameraDemand &value)
{
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s << value.generation << value.requestId << quint8(value.capture) << quint8(value.inUse);
    return frame(Kind::CameraDemand, bytes);
}
inline std::optional<CameraDemand> cameraDemand(const Record &record)
{
    if (record.kind != Kind::CameraDemand || record.payload.size() != 18) return {};
    QDataStream s(record.payload);
    CameraDemand value;
    quint8 capture = 0, inUse = 0;
    s >> value.generation >> value.requestId >> capture >> inUse;
    if (s.status() != QDataStream::Ok || !s.atEnd() || !value.generation || !value.requestId || capture > 1 || inUse > 1) return {};
    value.capture = capture;
    value.inUse = inUse;
    return value;
}
}
