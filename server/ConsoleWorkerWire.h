// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
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

#include "CodecPolicy.h"
#include "VideoFrame.h"

namespace KRdp::ConsoleWorkerWire
{
// Paired broker/worker wire. Reset to 1 on 2026-09-27 (AUD-C-6); older
// versions are not accepted. Broker and worker must always upgrade together.
// 2 (AUD-FIX7): Frame carries the codec that produced it; EncoderCaps,
// EncoderConfig, EncoderReport and EncoderLoad carry the codec policy.
//
// 3 (STATS-S6): EncoderConfig carries statsWanted; EncoderStats (worker -> broker, 1 Hz, only
// while the current EncoderConfig asks for it) carries the encoder's frame counts and encode time.
//
// 4 (FIX-CURSOR): Cursor (worker -> broker, after Ready): the desktop's cursor shape - hidden,
// the default arrow, or a bitmap with its hotspot - whenever it changes, never its position.
//
// 5 (AV1-Q): EncoderConfig's settings carry the AV1 tile count (EncoderSettings::av1Tiles).
//
// Worker -> broker order (AUD-FIX8; ConsoleWorkerOutbox is the worker's side):
//   Hello, [EncoderCaps], Ready, then any record. Before Ready the broker also
//   accepts EncoderReport (held and applied right after Ready), EncoderLoad
//   and Error (the worker's reason for failing); anything else - EncoderStats
//   included - fails it.
// Broker -> worker: nothing but Stop before the worker authenticated, and
//   nothing but Stop/RequestKeyFrame before Ready.
constexpr quint16 ProtocolVersion = 5;
constexpr quint32 MaxRecordBytes = 64 * 1024 * 1024;
constexpr int MaxFrameDimension = 16384;
/// The console launcher passes the per-launch broker socket path here, not in argv (AUD-C-9).
constexpr const char *SocketEnvironment = "KRDP_CONSOLE_WORKER_SOCKET";

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
    Topology,
    TopologyQuery,
    PositionBatch,
    PositionBatchResult,
    ManagedFit,
    ManagedFitResult,
    Primary,
    PrimaryResult,
    Mixed,
    MixedResult,
    MixedCreate,
    MixedCreateResult,
    PhysicalLayout,
    PhysicalLayoutResult,
    PhysicalLeaseReleased,
    EncoderCaps,
    EncoderConfig,
    EncoderReport,
    EncoderLoad,
    EncoderStats,
    Cursor,
};
constexpr Kind LastKind = Kind::Cursor;

/// VideoCodec on the wire: its value + 1, 0 = none/unknown. VideoCodec's last value is Av1 (4).
constexpr quint8 MaxWireCodec = 5;
inline quint8 wireCodec(std::optional<VideoCodec> codec)
{
    return codec ? quint8(int(*codec) + 1) : quint8(0);
}
inline std::optional<VideoCodec> codecFromWire(quint8 value)
{
    return value == 0 || value > MaxWireCodec ? std::nullopt : std::optional<VideoCodec>(VideoCodec(int(value) - 1));
}

/**
 * AUD-FIX7, worker -> broker, right after Hello: the encoders this worker really has
 * (EncoderSupport::probe() in the worker, which runs as the desktop's user on the desktop's
 * render node). The broker's codec policy chooses among these.
 */
struct EncoderCaps {
    CodecPolicy::Encoders encoders;
    bool avc444Hardware = false;
    QString renderNode;
    bool operator==(const EncoderCaps &) const = default;
};

/**
 * AUD-FIX7, broker -> worker: what the controlling connection's codec policy wants the
 * worker's encoders to run (VideoStream::codecForSessions(), encoderSettings() - absent for a
 * client that never asked for a codec, so KPipeWire's defaults stay - and requestedFrameRate(),
 * which also carries the delivery throttle). Applied only for the current control generation.
 */
struct EncoderConfig {
    quint64 generation = 0;
    VideoCodec codec = VideoCodec::Avc420;
    std::optional<CodecPolicy::EncoderSettings> settings;
    quint32 frameRate = CodecPolicy::DefaultFrameRate;
    /// STATS-S6: a KRDPCTL client of this connection is subscribed to stats, so the worker sends
    /// EncoderStats every EncoderStatsIntervalMs (after Ready, never before).
    bool statsWanted = false;
    bool operator==(const EncoderConfig &) const = default;
};

/** AUD-FIX7, worker -> broker: AbstractSession::encoderUnavailable / encoderBackendReported. */
struct EncoderReport {
    enum class Event : quint8 { Unavailable = 1, Backend = 2 };
    Event event = Event::Backend;
    VideoCodec codec = VideoCodec::Avc420;
    bool hardware = false;
    bool operator==(const EncoderReport &) const = default;
};

/** AUD-FIX7, worker -> broker, every EncoderLoadIntervalMs while controlled: its process CPU time (the CPU guard's input). */
constexpr int EncoderLoadIntervalMs = 250;
struct EncoderLoad {
    qint64 cpuNs = 0;
    bool operator==(const EncoderLoad &) const = default;
};

/**
 * STATS-S6, worker -> broker, every EncoderStatsIntervalMs while the current EncoderConfig has
 * statsWanted and this worker is controlled: over the last interval, the frames its encoders
 * produced, how many of them it did not forward (a layout proof, a Fit, a switch), and the
 * encoder's measured time per frame in microseconds (-1 = unknown: KPipeWire reports it only for
 * its AVC444 encoder).
 */
constexpr int EncoderStatsIntervalMs = 1000;
struct EncoderStats {
    quint32 intervalMs = 0;
    quint32 framesEncoded = 0;
    quint32 framesSkipped = 0;
    qint32 encodeUs = -1;
    bool operator==(const EncoderStats &) const = default;
};

/**
 * FIX-CURSOR, worker -> broker, after Ready: the cursor shape the desktop shows now (KWin's
 * screencast cursor metadata, merged over every captured output). Sent only when it changes and
 * at most every CursorShape::MinIntervalMs; the broker turns it into RDP pointer updates.
 * `pixels` is `size` in QImage::Format_ARGB32 (straight alpha, 4 * width bytes per row) and is
 * present only for Image. The broker's clients get no position: RDP clients move their own pointer.
 */
struct CursorShape {
    enum class Type : quint8 { Hidden = 1, Default = 2, Image = 3 };
    /// RDP's largest pointer (TS_LARGE_POINTER); anything bigger is sent as Default.
    static constexpr int MaxDimension = 384;
    static constexpr int MinIntervalMs = 16;
    Type type = Type::Default;
    QPoint hotspot;
    QSize size{0, 0};
    QByteArray pixels;
    bool operator==(const CursorShape &) const = default;
};

struct Record {
    Kind kind = Kind::Error;
    QByteArray payload;
    bool operator==(const Record &) const = default;
};

inline QByteArray frame(Kind kind, const QByteArray &payload = {});

struct PhysicalLeaseReleased {
    quint64 controlGeneration = 0;
    bool verified = false; // Exact physical and/or creator-output lease release readback;
                           // fresh capture comes from a replacement worker.
    bool operator==(const PhysicalLeaseReleased &) const = default;
};

inline QByteArray frame(const PhysicalLeaseReleased &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.controlGeneration << result.verified;
    return frame(Kind::PhysicalLeaseReleased, payload);
}

inline std::optional<PhysicalLeaseReleased> physicalLeaseReleased(const Record &record)
{
    if (record.kind != Kind::PhysicalLeaseReleased || record.payload.size() != 9) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    PhysicalLeaseReleased result;
    stream >> result.controlGeneration >> result.verified;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !result.controlGeneration) return {};
    return result;
}

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

struct PositionTarget {
    QString output;
    QPoint globalLogical;
    bool operator==(const PositionTarget &) const = default;
};

struct PositionBatch {
    quint64 requestId = 0;
    quint64 generation = 0;
    QVector<PositionTarget> targets;
    bool operator==(const PositionBatch &) const = default;
};

struct PositionBatchResult {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString error;
    bool operator==(const PositionBatchResult &) const = default;
};

struct FitRelation {
    QString parent;
    QString child;
    quint8 edge = 0; // 0 left, 1 right, 2 above, 3 below.
    qint32 offset = 0;
    bool operator==(const FitRelation &) const = default;
};

struct ManagedFit {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString output;
    QSize pixels;
    double scale = 1;
    QVector<FitRelation> relations;
    bool operator==(const ManagedFit &) const = default;
};

struct ManagedFitResult {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString error;
    bool operator==(const ManagedFitResult &) const = default;
};

struct Primary {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString output;
    bool operator==(const Primary &) const = default;
};

struct PrimaryResult {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString error;
    bool operator==(const PrimaryResult &) const = default;
};

struct MixedOperation {
    enum class Kind : quint8 { Move = 1, Resize = 2, Primary = 3 };
    Kind kind = Kind::Move;
    QString output;
    QPoint globalLogical;
    QSize pixels;
    double scale = 1;
    bool operator==(const MixedOperation &) const = default;
};

struct Mixed {
    quint64 requestId = 0;
    quint64 generation = 0;
    QVector<MixedOperation> operations;
    bool operator==(const Mixed &) const = default;
};

struct MixedResult {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString error;
    bool operator==(const MixedResult &) const = default;
};

// One creator-owned output plus existing-output changes, applied as one
// broker transaction. The new output is implicit, never a second request.
struct MixedCreate {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString newOutput;
    QSize pixels;
    double scale = 1;
    QPoint globalLogical;
    QVector<MixedOperation> changes;
    bool operator==(const MixedCreate &) const = default;
};

struct MixedCreateResult {
    quint64 requestId = 0;
    quint64 generation = 0;
    QString error;
    bool operator==(const MixedCreateResult &) const = default;
};

struct PhysicalOutput {
    QString name;
    QSize pixels;
    QRect logical;
    double scale = 1;
    bool primary = false;
    quint8 priority = 0;
    bool operator==(const PhysicalOutput &) const = default;
};

// The broker supplies its entire capture-verified physical snapshot and the
// exact draft. The selected unprivileged worker must compare ALL outputs with
// fresh KScreen before any mutation. The consent bit is explicit, not inferred
// from a physical-looking output name. This record alone grants no write path.
struct PhysicalLayout {
    quint64 requestId = 0;
    quint64 controlGeneration = 0;
    QString catalogGeneration;
    quint64 expectedRevision = 0;
    bool allowPhysicalChange = false;
    QVector<PhysicalOutput> before;
    QVector<MixedOperation> operations;
    bool operator==(const PhysicalLayout &) const = default;
};

struct PhysicalLayoutResult {
    quint64 requestId = 0;
    quint64 controlGeneration = 0;
    QString error;
    bool operator==(const PhysicalLayoutResult &) const = default;
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

inline QByteArray frame(const PositionBatch &request)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.requestId << request.generation << quint8(request.targets.size());
    for (const auto &target : request.targets) stream << target.output << target.globalLogical;
    return frame(Kind::PositionBatch, payload);
}

inline std::optional<PositionBatch> positionBatch(const Record &record)
{
    if (record.kind != Kind::PositionBatch || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    PositionBatch request;
    quint8 count = 0;
    stream >> request.requestId >> request.generation >> count;
    if (stream.status() != QDataStream::Ok || !request.requestId || !request.generation || count < 2 || count > 16) return {};
    QSet<QString> names;
    for (quint8 i = 0; i < count; ++i) {
        PositionTarget target;
        stream >> target.output >> target.globalLogical;
        if (stream.status() != QDataStream::Ok || target.output.isEmpty() || target.output.size() > 128
            || names.contains(target.output)
            || target.globalLogical.x() < -32768 || target.globalLogical.x() > 32768
            || target.globalLogical.y() < -32768 || target.globalLogical.y() > 32768) return {};
        for (const auto character : target.output) {
            if (!character.isLetterOrNumber() && character != QLatin1Char('-') && character != QLatin1Char('_')) return {};
        }
        names.insert(target.output);
        request.targets.append(target);
    }
    if (!stream.atEnd()) return {};
    return request;
}

inline QByteArray frame(const PositionBatchResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.requestId << result.generation << result.error;
    return frame(Kind::PositionBatchResult, payload);
}

inline std::optional<PositionBatchResult> positionBatchResult(const Record &record)
{
    if (record.kind != Kind::PositionBatchResult || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    PositionBatchResult result;
    stream >> result.requestId >> result.generation >> result.error;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !result.requestId || !result.generation
        || result.error.size() > 1024) return {};
    return result;
}

inline QByteArray frame(const ManagedFit &request)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.requestId << request.generation << request.output << request.pixels << request.scale
           << quint8(request.relations.size());
    for (const auto &relation : request.relations)
        stream << relation.parent << relation.child << relation.edge << relation.offset;
    return frame(Kind::ManagedFit, payload);
}

inline std::optional<ManagedFit> managedFit(const Record &record)
{
    if (record.kind != Kind::ManagedFit || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    ManagedFit request;
    quint8 count = 0;
    stream >> request.requestId >> request.generation >> request.output >> request.pixels >> request.scale >> count;
    if (stream.status() != QDataStream::Ok || !request.requestId || !request.generation || count > 16
        || request.pixels.width() < 320 || request.pixels.height() < 200
        || request.pixels.width() > 4096 || request.pixels.height() > 4096
        || request.pixels.width() % 2 || request.pixels.height() % 2
        || !std::isfinite(request.scale) || request.scale < 1.0 || request.scale > 4.0
        || request.output.isEmpty() || request.output.size() > 128) return {};
    const auto validName = [](const QString &name) {
        if (name.isEmpty() || name.size() > 128) return false;
        for (const auto character : name)
            if (!character.isLetterOrNumber() && character != QLatin1Char('-') && character != QLatin1Char('_')) return false;
        return true;
    };
    if (!validName(request.output)) return {};
    QSet<QString> children;
    for (quint8 i = 0; i < count; ++i) {
        FitRelation relation;
        stream >> relation.parent >> relation.child >> relation.edge >> relation.offset;
        if (stream.status() != QDataStream::Ok || !validName(relation.parent) || !validName(relation.child)
            || relation.parent == relation.child || relation.edge > 3 || children.contains(relation.child)
            || relation.offset < -32768 || relation.offset > 32768) return {};
        children.insert(relation.child);
        request.relations.append(relation);
    }
    if (!stream.atEnd()) return {};
    return request;
}

inline QByteArray frame(const ManagedFitResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.requestId << result.generation << result.error;
    return frame(Kind::ManagedFitResult, payload);
}

inline std::optional<ManagedFitResult> managedFitResult(const Record &record)
{
    if (record.kind != Kind::ManagedFitResult || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    ManagedFitResult result;
    stream >> result.requestId >> result.generation >> result.error;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !result.requestId || !result.generation
        || result.error.size() > 1024) return {};
    return result;
}

inline QByteArray frame(const Primary &request)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.requestId << request.generation << request.output;
    return frame(Kind::Primary, payload);
}

inline std::optional<Primary> primary(const Record &record)
{
    if (record.kind != Kind::Primary || record.payload.size() > 512) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    Primary request;
    stream >> request.requestId >> request.generation >> request.output;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !request.requestId || !request.generation
        || request.output.isEmpty() || request.output.size() > 128) return {};
    for (const auto character : request.output)
        if (!character.isLetterOrNumber() && character != QLatin1Char('-') && character != QLatin1Char('_')) return {};
    return request;
}

inline QByteArray frame(const PrimaryResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.requestId << result.generation << result.error;
    return frame(Kind::PrimaryResult, payload);
}

inline std::optional<PrimaryResult> primaryResult(const Record &record)
{
    if (record.kind != Kind::PrimaryResult || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    PrimaryResult result;
    stream >> result.requestId >> result.generation >> result.error;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !result.requestId || !result.generation
        || result.error.size() > 1024) return {};
    return result;
}

inline QByteArray frame(const Mixed &request)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.requestId << request.generation << quint8(request.operations.size());
    for (const auto &operation : request.operations)
        stream << quint8(operation.kind) << operation.output << operation.globalLogical
               << operation.pixels << operation.scale;
    return frame(Kind::Mixed, payload);
}

inline std::optional<Mixed> mixed(const Record &record)
{
    if (record.kind != Kind::Mixed || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    Mixed request;
    quint8 count = 0;
    stream >> request.requestId >> request.generation >> count;
    if (stream.status() != QDataStream::Ok || !request.requestId || !request.generation
        || count < 2 || count > 16) return {};
    for (quint8 i = 0; i < count; ++i) {
        MixedOperation operation;
        quint8 kind = 0;
        stream >> kind >> operation.output >> operation.globalLogical >> operation.pixels >> operation.scale;
        if (stream.status() != QDataStream::Ok || kind < 1 || kind > 3
            || operation.output.isEmpty() || operation.output.size() > 128
            || !std::isfinite(operation.scale)) return {};
        for (const auto character : operation.output)
            if (!character.isLetterOrNumber() && character != QLatin1Char('-') && character != QLatin1Char('_')) return {};
        operation.kind = MixedOperation::Kind(kind);
        if (operation.kind == MixedOperation::Kind::Move) {
            if (operation.globalLogical.x() < 0 || operation.globalLogical.x() > 32768
                || operation.globalLogical.y() < 0 || operation.globalLogical.y() > 32768
                || operation.pixels != QSize() || operation.scale != 1) return {};
        } else if (operation.kind == MixedOperation::Kind::Resize) {
            if (operation.globalLogical != QPoint() || operation.pixels.width() < 320
                || operation.pixels.width() > 4096 || operation.pixels.height() < 200
                || operation.pixels.height() > 4096 || operation.pixels.width() % 2
                || operation.pixels.height() % 2 || operation.scale < 1 || operation.scale > 4) return {};
        } else if (operation.globalLogical != QPoint() || operation.pixels != QSize()
            || operation.scale != 1) return {};
        request.operations.append(operation);
    }
    if (!stream.atEnd()) return {};
    return request;
}

inline QByteArray frame(const MixedResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.requestId << result.generation << result.error;
    return frame(Kind::MixedResult, payload);
}

inline std::optional<MixedResult> mixedResult(const Record &record)
{
    if (record.kind != Kind::MixedResult || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    MixedResult result;
    stream >> result.requestId >> result.generation >> result.error;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !result.requestId || !result.generation
        || result.error.size() > 1024) return {};
    return result;
}

inline QByteArray frame(const PhysicalLayout &request)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.requestId << request.controlGeneration << request.catalogGeneration
           << request.expectedRevision << quint8(request.allowPhysicalChange ? 1 : 0) << quint8(request.before.size());
    for (const auto &output : request.before)
        stream << output.name << output.pixels << output.logical << output.scale << output.primary << output.priority;
    stream << quint8(request.operations.size());
    for (const auto &operation : request.operations)
        stream << quint8(operation.kind) << operation.output << operation.globalLogical
               << operation.pixels << operation.scale;
    return frame(Kind::PhysicalLayout, payload);
}

inline std::optional<PhysicalLayout> physicalLayout(const Record &record)
{
    if (record.kind != Kind::PhysicalLayout || record.payload.size() > 16384) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    PhysicalLayout request;
    quint8 consent = 0;
    quint8 count = 0;
    stream >> request.requestId >> request.controlGeneration >> request.catalogGeneration
           >> request.expectedRevision >> consent >> count;
    request.allowPhysicalChange = consent == 1;
    const auto safeName = [](const QString &name) {
        if (name.isEmpty() || name.size() > 128) return false;
        for (const auto character : name)
            if (!character.isLetterOrNumber() && character != QLatin1Char('-') && character != QLatin1Char('_')) return false;
        return true;
    };
    if (stream.status() != QDataStream::Ok || !request.requestId || !request.controlGeneration
        || !safeName(request.catalogGeneration) || !request.expectedRevision
        || consent != 1 || count < 1 || count > 16) return {};
    QSet<QString> names;
    QSet<quint8> priorities;
    for (quint8 i = 0; i < count; ++i) {
        PhysicalOutput output;
        stream >> output.name >> output.pixels >> output.logical >> output.scale >> output.primary >> output.priority;
        if (stream.status() != QDataStream::Ok || !safeName(output.name)
            || output.name.startsWith(QStringLiteral("Virtual-")) || names.contains(output.name)
            || output.pixels.width() < 320 || output.pixels.width() > 4096
            || output.pixels.height() < 200 || output.pixels.height() > 4096
            || !output.logical.isValid() || output.logical.left() < -32768 || output.logical.top() < -32768
            || output.logical.right() > 32768 || output.logical.bottom() > 32768
            || !std::isfinite(output.scale) || output.scale < 1 || output.scale > 4
            || output.logical.size() != QSize(int(std::ceil(output.pixels.width() / output.scale)),
                int(std::ceil(output.pixels.height() / output.scale)))
            || output.priority < 1 || output.priority > 16 || priorities.contains(output.priority)
            || output.primary != (output.priority == 1)) return {};
        names.insert(output.name);
        priorities.insert(output.priority);
        request.before.append(output);
    }
    if (!priorities.contains(1)) return {};
    quint8 operations = 0;
    stream >> operations;
    if (stream.status() != QDataStream::Ok || operations < 1 || operations > 16) return {};
    QSet<QString> seen;
    int primaryOperations = 0;
    for (quint8 i = 0; i < operations; ++i) {
        MixedOperation operation;
        quint8 kind = 0;
        stream >> kind >> operation.output >> operation.globalLogical >> operation.pixels >> operation.scale;
        if (stream.status() != QDataStream::Ok || kind < 1 || kind > 3 || !names.contains(operation.output)
            || !std::isfinite(operation.scale)) return {};
        operation.kind = MixedOperation::Kind(kind);
        const QString key = QString::number(kind) + QLatin1Char(':') + operation.output;
        if (seen.contains(key)) return {};
        seen.insert(key);
        if (operation.kind == MixedOperation::Kind::Move) {
            if (operation.globalLogical.x() < -32768 || operation.globalLogical.x() > 32768
                || operation.globalLogical.y() < -32768 || operation.globalLogical.y() > 32768
                || !operation.pixels.isEmpty() || operation.scale != 1) return {};
        } else if (operation.kind == MixedOperation::Kind::Resize) {
            if (operation.globalLogical != QPoint() || operation.pixels.width() < 320
                || operation.pixels.width() > 4096 || operation.pixels.height() < 200
                || operation.pixels.height() > 4096 || operation.pixels.width() % 2
                || operation.pixels.height() % 2 || operation.scale < 1 || operation.scale > 4) return {};
        } else {
            if (operation.globalLogical != QPoint() || !operation.pixels.isEmpty() || operation.scale != 1
                || ++primaryOperations > 1) return {};
        }
        request.operations.append(operation);
    }
    return stream.atEnd() ? std::optional(request) : std::nullopt;
}

inline QByteArray frame(const PhysicalLayoutResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.requestId << result.controlGeneration << result.error;
    return frame(Kind::PhysicalLayoutResult, payload);
}

inline std::optional<PhysicalLayoutResult> physicalLayoutResult(const Record &record)
{
    if (record.kind != Kind::PhysicalLayoutResult || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    PhysicalLayoutResult result;
    stream >> result.requestId >> result.controlGeneration >> result.error;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !result.requestId
        || !result.controlGeneration || result.error.size() > 1024) return {};
    return result;
}

inline QByteArray frame(const MixedCreate &request)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.requestId << request.generation << request.newOutput << request.pixels
           << request.scale << request.globalLogical << quint8(request.changes.size());
    for (const auto &change : request.changes)
        stream << quint8(change.kind) << change.output << change.globalLogical << change.pixels << change.scale;
    return frame(Kind::MixedCreate, payload);
}

inline std::optional<MixedCreate> mixedCreate(const Record &record)
{
    if (record.kind != Kind::MixedCreate || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    MixedCreate request;
    quint8 count = 0;
    stream >> request.requestId >> request.generation >> request.newOutput >> request.pixels
           >> request.scale >> request.globalLogical >> count;
    if (stream.status() != QDataStream::Ok || !request.requestId || !request.generation
        || !request.newOutput.startsWith(QStringLiteral("Virtual-krdp-added-"))
        || request.newOutput.size() > 128 || request.pixels.width() < 320 || request.pixels.width() > 4096
        || request.pixels.height() < 200 || request.pixels.height() > 4096
        || request.pixels.width() % 2 || request.pixels.height() % 2
        || !std::isfinite(request.scale) || request.scale < 1 || request.scale > 4
        || request.globalLogical.x() < 0 || request.globalLogical.x() > 32768
        || request.globalLogical.y() < 0 || request.globalLogical.y() > 32768
        || count < 1 || count > 15) return {};
    for (const auto character : request.newOutput)
        if (!character.isLetterOrNumber() && character != QLatin1Char('-') && character != QLatin1Char('_')) return {};
    for (quint8 i = 0; i < count; ++i) {
        MixedOperation change;
        quint8 kind = 0;
        stream >> kind >> change.output >> change.globalLogical >> change.pixels >> change.scale;
        if (stream.status() != QDataStream::Ok || kind < 1 || kind > 3
            || change.output.isEmpty() || change.output.size() > 128
            || !std::isfinite(change.scale)) return {};
        for (const auto character : change.output)
            if (!character.isLetterOrNumber() && character != QLatin1Char('-') && character != QLatin1Char('_')) return {};
        change.kind = MixedOperation::Kind(kind);
        if (change.kind == MixedOperation::Kind::Move) {
            if (change.globalLogical.x() < 0 || change.globalLogical.x() > 32768
                || change.globalLogical.y() < 0 || change.globalLogical.y() > 32768
                || change.pixels != QSize() || change.scale != 1) return {};
        } else if (change.kind == MixedOperation::Kind::Resize) {
            if (change.globalLogical != QPoint() || change.pixels.width() < 320
                || change.pixels.width() > 4096 || change.pixels.height() < 200
                || change.pixels.height() > 4096 || change.pixels.width() % 2
                || change.pixels.height() % 2 || change.scale < 1 || change.scale > 4) return {};
        } else if (change.globalLogical != QPoint() || change.pixels != QSize()
            || change.scale != 1) return {};
        request.changes.append(change);
    }
    if (!stream.atEnd()) return {};
    return request;
}

inline QByteArray frame(const MixedCreateResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.requestId << result.generation << result.error;
    return frame(Kind::MixedCreateResult, payload);
}

inline std::optional<MixedCreateResult> mixedCreateResult(const Record &record)
{
    if (record.kind != Kind::MixedCreateResult || record.payload.size() > 4096) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    MixedCreateResult result;
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

// Independently KScreen-confirmed inventory accompanying a decoded console
// keyframe. Empty means the worker could not validate the current capture;
// the broker must retire its cached topology instead of serving stale data.
struct TopologyOutput {
    QString name;
    QSize pixels;
    QRect logical;
    double scale = 1;
    bool primary = false;
    quint8 priority = 0;
    bool physical = true; // False only for an output created and owned by this worker.
    bool operator==(const TopologyOutput &) const = default;
};

struct Topology {
    QVector<TopologyOutput> outputs;
    bool operator==(const Topology &) const = default;
};

inline QByteArray frame(const Topology &topology)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(topology.outputs.size());
    for (const auto &output : topology.outputs)
        stream << output.name << output.pixels << output.logical << output.scale << output.primary << output.priority << output.physical;
    return frame(Kind::Topology, payload);
}

inline std::optional<Topology> topology(const Record &record)
{
    if (record.kind != Kind::Topology || record.payload.size() > 32768) return {};
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 count = 0;
    stream >> count;
    if (count > 16) return {};
    Topology result;
    QSet<QString> names;
    QSet<int> priorities;
    int primaries = 0;
    for (quint32 i = 0; i < count; ++i) {
        TopologyOutput output;
        stream >> output.name >> output.pixels >> output.logical >> output.scale >> output.primary >> output.priority >> output.physical;
        if (stream.status() != QDataStream::Ok || output.name.isEmpty() || output.name.size() > 128
            || names.contains(output.name) || output.pixels.width() < 1 || output.pixels.width() > 16384
            || output.pixels.height() < 1 || output.pixels.height() > 16384
            || output.logical.isEmpty() || output.logical.width() > 32768 || output.logical.height() > 32768
            || output.logical.x() < -32768 || output.logical.x() > 32768
            || output.logical.y() < -32768 || output.logical.y() > 32768
            || !std::isfinite(output.scale) || output.scale < 1 || output.scale > 4
            || output.priority < 1 || output.priority > 16 || priorities.contains(output.priority)
            || output.primary != (output.priority == 1)) return {};
        names.insert(output.name);
        priorities.insert(output.priority);
        primaries += output.primary;
        result.outputs.append(output);
    }
    return stream.status() == QDataStream::Ok && stream.atEnd() && (count == 0 || (primaries == 1 && priorities.size() == count))
        ? std::optional<Topology>(result) : std::nullopt;
}

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

namespace detail
{
inline void writeBackends(QDataStream &stream, const CodecPolicy::Backends &b)
{
    stream << b.hardware << b.software << b.liveBitrate;
}
inline void readBackends(QDataStream &stream, CodecPolicy::Backends &b)
{
    stream >> b.hardware >> b.software >> b.liveBitrate;
}
}

inline QByteArray frame(const EncoderCaps &caps)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    detail::writeBackends(stream, caps.encoders.avc);
    detail::writeBackends(stream, caps.encoders.hevc);
    detail::writeBackends(stream, caps.encoders.av1);
    stream << caps.avc444Hardware << caps.renderNode.left(256);
    return frame(Kind::EncoderCaps, payload);
}

inline std::optional<EncoderCaps> encoderCaps(const Record &record)
{
    if (record.kind != Kind::EncoderCaps || record.payload.size() > 4096) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    EncoderCaps caps;
    detail::readBackends(stream, caps.encoders.avc);
    detail::readBackends(stream, caps.encoders.hevc);
    detail::readBackends(stream, caps.encoders.av1);
    stream >> caps.avc444Hardware >> caps.renderNode;
    return stream.status() == QDataStream::Ok && stream.atEnd() && caps.renderNode.size() <= 256 ? std::optional<EncoderCaps>(caps) : std::nullopt;
}

inline QByteArray frame(const EncoderConfig &config)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << config.generation << wireCodec(config.codec) << config.settings.has_value();
    const auto settings = config.settings.value_or(CodecPolicy::EncoderSettings{});
    stream << settings.hardware << quint8(settings.preset) << settings.targetKbps << qint32(settings.maxFrameRate) << config.frameRate << config.statsWanted
           << quint8(std::clamp(settings.av1Tiles, 0, 64));
    return frame(Kind::EncoderConfig, payload);
}

inline std::optional<EncoderConfig> encoderConfig(const Record &record)
{
    if (record.kind != Kind::EncoderConfig) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    EncoderConfig config;
    quint8 codec = 0;
    bool hasSettings = false;
    CodecPolicy::EncoderSettings settings;
    quint8 preset = 0;
    qint32 maxFrameRate = 0;
    quint8 av1Tiles = 0;
    stream >> config.generation >> codec >> hasSettings >> settings.hardware >> preset >> settings.targetKbps >> maxFrameRate >> config.frameRate >> config.statsWanted
        >> av1Tiles;
    const auto decoded = codecFromWire(codec);
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !config.generation || !decoded || preset > quint8(CodecPolicy::Preset::Fastest)
        || maxFrameRate < 0 || maxFrameRate > 240 || config.frameRate < 1 || config.frameRate > 240 || settings.targetKbps > 1000000 || av1Tiles > 64) {
        return std::nullopt;
    }
    config.codec = *decoded;
    settings.preset = CodecPolicy::Preset(preset);
    settings.maxFrameRate = maxFrameRate;
    settings.av1Tiles = av1Tiles;
    if (hasSettings) {
        config.settings = settings;
    }
    return config;
}

inline QByteArray frame(const EncoderReport &report)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint8(report.event) << wireCodec(report.codec) << report.hardware;
    return frame(Kind::EncoderReport, payload);
}

inline std::optional<EncoderReport> encoderReport(const Record &record)
{
    if (record.kind != Kind::EncoderReport) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    quint8 event = 0;
    quint8 codec = 0;
    EncoderReport report;
    stream >> event >> codec >> report.hardware;
    const auto decoded = codecFromWire(codec);
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !decoded
        || (event != quint8(EncoderReport::Event::Unavailable) && event != quint8(EncoderReport::Event::Backend))) {
        return std::nullopt;
    }
    report.event = EncoderReport::Event(event);
    report.codec = *decoded;
    return report;
}

inline QByteArray frame(const EncoderLoad &load)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << load.cpuNs;
    return frame(Kind::EncoderLoad, payload);
}

inline std::optional<EncoderLoad> encoderLoad(const Record &record)
{
    if (record.kind != Kind::EncoderLoad || record.payload.size() != 8) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    EncoderLoad load;
    stream >> load.cpuNs;
    return stream.status() == QDataStream::Ok && load.cpuNs >= 0 ? std::optional<EncoderLoad>(load) : std::nullopt;
}

inline QByteArray frame(const EncoderStats &stats)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << stats.intervalMs << stats.framesEncoded << stats.framesSkipped << stats.encodeUs;
    return frame(Kind::EncoderStats, payload);
}

inline QByteArray frame(const CursorShape &cursor)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint8(cursor.type) << qint16(cursor.hotspot.x()) << qint16(cursor.hotspot.y()) << quint16(cursor.size.width())
           << quint16(cursor.size.height()) << cursor.pixels;
    return frame(Kind::Cursor, payload);
}

inline std::optional<CursorShape> cursorShape(const Record &record)
{
    constexpr qsizetype MaxPayload = 16 + qsizetype(CursorShape::MaxDimension) * CursorShape::MaxDimension * 4;
    if (record.kind != Kind::Cursor || record.payload.size() > MaxPayload) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    quint8 type = 0;
    qint16 x = 0;
    qint16 y = 0;
    quint16 width = 0;
    quint16 height = 0;
    CursorShape cursor;
    stream >> type >> x >> y >> width >> height >> cursor.pixels;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || type < quint8(CursorShape::Type::Hidden) || type > quint8(CursorShape::Type::Image)) {
        return std::nullopt;
    }
    cursor.type = CursorShape::Type(type);
    cursor.hotspot = QPoint(x, y);
    cursor.size = QSize(width, height);
    if (cursor.type == CursorShape::Type::Image) {
        // The worker runs as another user: the broker sizes RDP pointer PDUs from this.
        if (width < 1 || height < 1 || width > CursorShape::MaxDimension || height > CursorShape::MaxDimension
            || cursor.pixels.size() != qsizetype(width) * height * 4 || x < 0 || y < 0 || x >= width || y >= height) {
            return std::nullopt;
        }
    } else if (width != 0 || height != 0 || x != 0 || y != 0 || !cursor.pixels.isEmpty()) {
        return std::nullopt;
    }
    return cursor;
}

inline std::optional<EncoderStats> encoderStats(const Record &record)
{
    if (record.kind != Kind::EncoderStats || record.payload.size() != 16) {
        return std::nullopt;
    }
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    EncoderStats stats;
    stream >> stats.intervalMs >> stats.framesEncoded >> stats.framesSkipped >> stats.encodeUs;
    if (stream.status() != QDataStream::Ok || stats.intervalMs == 0 || stats.intervalMs > 60000 || stats.framesSkipped > stats.framesEncoded
        || stats.framesEncoded > 100000 || stats.encodeUs < -1 || stats.encodeUs > 10000000) {
        return std::nullopt;
    }
    return stats;
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
    stream << wireCodec(video.codec); // v2
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
    // The worker runs as another user; its sizes and indices index broker
    // arrays and size encoder surfaces (AUD-C-10). Reject anything outside
    // the limits a capture can legitimately produce.
    if (stream.status() != QDataStream::Ok || count > 16 || video.size.width() < 1 || video.size.height() < 1
        || video.size.width() > MaxFrameDimension || video.size.height() > MaxFrameDimension || monitorIndex < 0
        || (count == 0 ? monitorIndex != 0 : monitorIndex >= qint32(count))) {
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
    quint8 codec = 0;
    stream >> codec;
    if (stream.status() != QDataStream::Ok || codec > MaxWireCodec) {
        return std::nullopt;
    }
    video.codec = codecFromWire(codec);
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
        if (stream.status() == QDataStream::Ok && version != ProtocolVersion) {
            // A different paired build, not a corrupt stream. Callers back
            // off instead of relaunching the same mismatched worker at once.
            m_mismatchedVersion = version;
            ++m_invalid;
            return std::nullopt;
        }
        if (stream.status() != QDataStream::Ok || !stream.atEnd() || type < quint8(Kind::Hello) || type > quint8(LastKind)) {
            ++m_invalid;
            return std::nullopt;
        }
        return Record{Kind(type), payload};
    }

    bool overflowed() const { return m_overflowed; }
    int takeInvalidCount() { return std::exchange(m_invalid, 0); }
    /** Peer wire version of the last well-formed record with a different version, if any. */
    std::optional<quint16> mismatchedVersion() const { return m_mismatchedVersion; }
    /** Bytes buffered but not yet returned as a record. */
    qsizetype pending() const { return m_buffer.size(); }

private:
    std::optional<quint16> m_mismatchedVersion;
    QByteArray m_buffer;
    bool m_overflowed = false;
    int m_invalid = 0;
};
}
