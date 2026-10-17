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

#include "ClipboardFileDescriptor.h"
#include "ClipboardFileLimits.h"
#include "CodecPolicy.h"
#include "ChromaPolicy.h"
#include "ChromaTimingReport.h"
#include "MonitorCapturePolicy.h"
#include "ConsoleVirtualOutputPolicy.h"
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
//   only Stop/RequestKeyFrame/DisplayPolicy before Ready. DisplayPolicy requires
//   the authenticated Hello and conveys streaming demand, never input authority.
// 6 (WS-D camera): camera policy, format, compressed samples and worker demand.
// 7 (T07): per-desktop display wake/inhibition, also before first capture.
// 8 (T05): complete chroma timing/demand in EncoderConfig; actual chroma
// capability in EncoderReport, distinct from encoder backend.
// 9 (T05): complete per-output AVC444 costs, bound to the control generation.
// 10 (T06): Console monitor capture selection in current-owner EncoderConfig.
// 11 (T06): generation-bound Console temporary-output policy and client display tuple.
// 12 (OPT-054): relative pointer input and physical-device reclaim requests.
// 13 (OPT-054): native pointer capture policy and observed state.
// 14 (OPT-060 M-2): EncoderConfig's Console temporary-output policy carries Layout::Mapped, the client monitors
// (id, physical pixels, scale percent, primary) and the host-screen -> monitor mapping.
// 16 (OPT-067): the text clipboard bridge. ClipboardPolicy (broker -> worker, the planner's decision),
// ClipboardOffer (worker -> broker, a host copy happened, no contents), ClipboardRequest (broker -> worker, the
// client pasted), ClipboardData (worker -> broker, the answer), ClipboardSet (broker -> worker, the client copied)
// and ClipboardSetResult (worker -> broker). Offer/Data/SetResult are valid only after Ready. Contents are UTF-8
// with LF line endings, at most MaxClipboardBytes, and are never logged.
// 17 (OPT-055 wave 2): Heartbeat (worker -> broker, every HeartbeatIntervalMs from Hello on; the broker's watchdog
// treats any record as proof of life) and LocalReclaim (worker -> broker, payload ControlState: the user's reclaim
// shortcut ended Replace, so the broker reports deskInput rather than workerExit).
// 18 (OPT-071): HostPointer (worker -> broker, after Ready, Console only): where the host's pointer is, in the client's
// desktop coordinates, so the broker can show a desk user's pointer to the remote client. No key or button data.
// 19 (OPT-072): the clipboard bridge carries images. ClipboardOffer flags bit 1 = an image is available; ClipboardRequest,
// ClipboardData and ClipboardSet carry a ClipboardKind (text or image). An image is PNG, at most MaxClipboardImageBytes,
// and is never logged. Policy, gates and the one-request-at-a-time rule are the text bridge's.
// 20 (OPT-077): the clipboard bridge carries formatted text. ClipboardOffer flags bit 2 = HTML is available; ClipboardKind gains
// Html. A Html ClipboardData carries the HTML document (UTF-8, at most MaxClipboardHtmlBytes, never the cliprdr wrapper); a
// Html ClipboardSet carries the plain text beside it (may be empty) and the HTML (never empty), so the host application gets
// both in one offer. Policy, gates and the one-request-at-a-time rule are the text bridge's. Contents are never logged.
// 21 (OPT-078 phase 2): host-to-client file copy. ClipboardOffer flags bit 3 = files (text/uri-list of local files) are available;
// ClipboardKind gains Files: a Files ClipboardRequest (options bit 0 = the peer negotiated huge files) makes the worker walk the
// selection and answer with the FileGroupDescriptorW bytes (never file names on any log) and the count of entries it skipped. The
// file bytes travel as FileRead (broker -> worker, 64-bit offset, at most FileRangeBytes) / FileReadResult (worker -> broker);
// FileLock/FileUnlock pin an offer's snapshot while the client holds a clipboard lock, FileCancel drops an offer's queued reads,
// FileProgress (worker -> broker, at most 4/s) says how much of an offer was read. ClipboardPolicy carries the two file flags
// (the planner's per-direction decision for NEW transfers). Pacing needs no record: the broker holds the socket, issues a read
// only inside the request window and writes the response when the pacer allows.
constexpr quint16 ProtocolVersion = 21;
constexpr quint32 MaxRecordBytes = 64 * 1024 * 1024;
constexpr int MaxFrameDimension = 16384;
/// The console launcher passes the per-launch broker socket path here, not in argv (AUD-C-9).
constexpr const char *SocketEnvironment = "FARSIDE_CONSOLE_WORKER_SOCKET";

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
    CameraPolicy,
    CameraFormat,
    CameraFrame,
    CameraResult,
    CameraDemand,
    DisplayPolicy,
    ChromaTiming,
    ReclaimConsole,
    PointerCapture,
    PointerState,
    // Wire 15 (OPT-060 M-8): worker -> broker, payload ControlState. A host screen was plugged in while Replace was
    // live; the worker already restored the host's screens and continues as extend. Never loops: Replace is over.
    HostScreensChanged,
    // Wire 16 (OPT-067): text clipboard bridge, see the version comment above.
    ClipboardPolicy,
    ClipboardOffer,
    ClipboardRequest,
    ClipboardData,
    ClipboardSet,
    ClipboardSetResult,
    // Wire 17 (OPT-055 wave 2): see the version comment above.
    Heartbeat,
    LocalReclaim,
    // Wire 18 (OPT-071): see the version comment above.
    HostPointer,
    // Wire 21 (OPT-078): host-to-client file copy, see the version comment above.
    FileLock,
    FileUnlock,
    FileRead,
    FileReadResult,
    FileCancel,
    FileProgress,
};
constexpr Kind LastKind = Kind::FileProgress;

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
    ChromaPolicy chroma;
    bool chromaEnabled = true;
    MonitorCapturePolicy capture;
    ConsoleVirtualOutputPolicy consoleVirtual;
    bool operator==(const EncoderConfig &) const = default;
};

/** AUD-FIX7, worker -> broker: AbstractSession::encoderUnavailable / encoderBackendReported. */
struct EncoderReport {
    enum class Event : quint8 { Unavailable = 1, Backend = 2, ChromaCapability = 3 };
    Event event = Event::Backend;
    VideoCodec codec = VideoCodec::Avc420;
    bool hardware = false;
    bool chromaCapable = false;
    bool operator==(const EncoderReport &) const = default;
};

/** AUD-FIX7, worker -> broker, every EncoderLoadIntervalMs while controlled: its process CPU time (the CPU guard's input). */
constexpr int EncoderLoadIntervalMs = 250;
struct EncoderLoad {
    qint64 cpuNs = 0;
    bool operator==(const EncoderLoad &) const = default;
};

/// After Ready and only under the active encoder/control generation.
struct ChromaTiming {
    quint64 generation = 0;
    VideoCodec codec = VideoCodec::Avc444v2;
    int monitorIndex = 0;
    ChromaTimingReport timing;
    bool operator==(const ChromaTiming &) const = default;
};
// Keep cost reports valid under severe external GPU/CPU stalls; do not reuse
// EncoderStats' ten-second presentation cap for the raw timing measurements.
constexpr qint64 MaxChromaTimingUs = 3600000000LL;

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

/** Wire 17, worker -> broker: how often the worker's event loop says it is alive (the broker's check runs at the same rate). */
constexpr int HeartbeatIntervalMs = 3000;
/** Wire 17: the worker is called hung after this much silence (any record counts as life); 15 missed beats. */
constexpr int HeartbeatHungAfterMs = 45000;

struct Heartbeat {
    quint64 sequence = 0;
    bool operator==(const Heartbeat &) const = default;
};

struct ControlState {
    quint64 generation = 0;
    bool active = false;
    bool operator==(const ControlState &) const = default;
};

/** Broker -> authenticated desktop worker. Revision is endpoint-local and
 * strictly increases; independent from input ownership because viewers also
 * need usable capture. No display mutation/unlock permission is granted. */
struct DisplayPolicy {
    quint64 revision = 0;
    bool active = false;
    bool wakeEnabled = false;
    bool operator==(const DisplayPolicy &) const = default;
};

inline QByteArray frame(const DisplayPolicy &policy)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << policy.revision << quint8(policy.active) << quint8(policy.wakeEnabled);
    return frame(Kind::DisplayPolicy, payload);
}

inline std::optional<DisplayPolicy> displayPolicy(const Record &record)
{
    if (record.kind != Kind::DisplayPolicy || record.payload.size() != 10) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    DisplayPolicy policy;
    quint8 active = 0, wake = 0;
    stream >> policy.revision >> active >> wake;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !policy.revision
        || active > 1 || wake > 1 || (!active && wake)) return std::nullopt;
    policy.active = active;
    policy.wakeEnabled = wake;
    return policy;
}

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
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !quality.generation || quality.quality > 100) {
        return std::nullopt;
    }
    return quality;
}

inline bool mayApplyQuality(const VideoQuality &quality, const ControlState &control)
{
    return control.active && control.generation != 0 && quality.generation == control.generation
        && quality.quality <= 100;
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

inline QByteArray frame(const Heartbeat &beat)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << beat.sequence;
    return frame(Kind::Heartbeat, payload);
}

inline std::optional<Heartbeat> heartbeat(const Record &record)
{
    if (record.kind != Kind::Heartbeat || record.payload.size() != 8) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    Heartbeat beat;
    stream >> beat.sequence;
    return stream.status() == QDataStream::Ok && stream.atEnd() ? std::optional<Heartbeat>(beat) : std::nullopt;
}

/** Wire 18, worker -> broker, after Ready: the host pointer in the client's RDP desktop coordinates (OPT-071). */
struct HostPointer {
    static constexpr qint32 MaxCoordinate = 65535;
    qint32 x = 0;
    qint32 y = 0;
    bool visible = false;
    bool operator==(const HostPointer &) const = default;
};

inline QByteArray frame(const HostPointer &pointer)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << pointer.x << pointer.y << quint8(pointer.visible ? 1 : 0);
    return frame(Kind::HostPointer, payload);
}

inline std::optional<HostPointer> hostPointer(const Record &record)
{
    if (record.kind != Kind::HostPointer || record.payload.size() != 9) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    HostPointer pointer;
    quint8 visible = 0;
    stream >> pointer.x >> pointer.y >> visible;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || visible > 1 || pointer.x < 0 || pointer.y < 0
        || pointer.x > HostPointer::MaxCoordinate || pointer.y > HostPointer::MaxCoordinate) return std::nullopt;
    pointer.visible = visible == 1;
    return pointer;
}

// ---- OPT-067 text clipboard bridge (wire 16) ----
/// Largest text (UTF-8 bytes) either direction carries on the worker wire.
constexpr quint32 MaxClipboardBytes = 1024 * 1024;
/// OPT-072 (wire 19): largest image (encoded PNG bytes) either direction carries on the worker wire.
constexpr quint32 MaxClipboardImageBytes = 16 * 1024 * 1024;

/// OPT-077 (wire 20): largest HTML document (UTF-8 bytes) either direction carries on the worker wire.
constexpr quint32 MaxClipboardHtmlBytes = 8 * 1024 * 1024;
/// A Html ClipboardSet is its text length (4 bytes), the text and the HTML.
constexpr quint32 MaxClipboardRichSetBytes = 4 + MaxClipboardBytes + MaxClipboardHtmlBytes;

/// OPT-078 (wire 21): the most a FileGroupDescriptorW is (4 + 100 000 entries x 592); a Files answer is this plus a 4-byte skipped count.
constexpr quint32 MaxClipboardFileListBytes = 4 + ClipboardFileLimits::MaxEntries * 592;
/// OPT-078 (wire 21): the most file bytes one FileRead carries (the validation ceiling; the pacer asks for ClipboardFileLimits::RangeBytes).
constexpr quint32 MaxFileReadBytes = ClipboardFileLimits::MaxRangeBytes;

/// What a clipboard record carries (wire 19; Html since wire 20; Files since wire 21).
enum class ClipboardKind : quint8 { Text, Image, Html, Files };
constexpr quint8 LastClipboardKind = quint8(ClipboardKind::Files);
constexpr quint32 maxClipboardBytes(ClipboardKind kind)
{
    return kind == ClipboardKind::Image ? MaxClipboardImageBytes
        : kind == ClipboardKind::Html   ? MaxClipboardHtmlBytes
        : kind == ClipboardKind::Files  ? MaxClipboardFileListBytes
                                        : MaxClipboardBytes;
}
/// ClipboardOffer flag bits.
constexpr quint8 ClipboardOfferText = 1;
constexpr quint8 ClipboardOfferImage = 2;
constexpr quint8 ClipboardOfferHtml = 4;
/// OPT-078 (wire 21): the host copied files (text/uri-list). Exclusive: a file copy is announced as files only.
constexpr quint8 ClipboardOfferFiles = 8;

/** Broker -> worker: the planner's decision. Revision is endpoint-local and strictly increases. */
struct ClipboardPolicy {
    quint64 revision = 0;
    quint64 controlGeneration = 0;
    bool hostToClient = false;
    bool clientToHost = false;
    bool locked = false;
    bool greeter = false;
    /// OPT-078 (wire 21): file copy for NEW transfers, per direction (a subset of hostToClient / clientToHost).
    bool filesHostToClient = false;
    bool filesClientToHost = false;
    bool operator==(const ClipboardPolicy &) const = default;
};

inline QByteArray frame(const ClipboardPolicy &policy)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << policy.revision << policy.controlGeneration << quint8(policy.hostToClient) << quint8(policy.clientToHost)
           << quint8(policy.locked) << quint8(policy.greeter) << quint8(policy.filesHostToClient) << quint8(policy.filesClientToHost);
    return frame(Kind::ClipboardPolicy, payload);
}

inline std::optional<ClipboardPolicy> clipboardPolicy(const Record &record)
{
    if (record.kind != Kind::ClipboardPolicy || record.payload.size() != 22) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    ClipboardPolicy policy;
    quint8 flags[6] = {};
    stream >> policy.revision >> policy.controlGeneration >> flags[0] >> flags[1] >> flags[2] >> flags[3] >> flags[4] >> flags[5];
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !policy.revision) return std::nullopt;
    for (const auto flag : flags) if (flag > 1) return std::nullopt;
    // A greeter never offers its clipboard to a client; a policy that says otherwise is malformed, not trusted.
    if (flags[3] && flags[0]) return std::nullopt;
    // A file direction is a subset of its clipboard direction, and the login screen shares no files at all.
    if ((flags[4] && !flags[0]) || (flags[5] && !flags[1]) || (flags[3] && (flags[4] || flags[5]))) return std::nullopt;
    policy.hostToClient = flags[0];
    policy.clientToHost = flags[1];
    policy.locked = flags[2];
    policy.greeter = flags[3];
    policy.filesHostToClient = flags[4];
    policy.filesClientToHost = flags[5];
    return policy;
}

/** Worker -> broker: the host's clipboard changed. No contents, no length. */
struct ClipboardOffer {
    quint64 seq = 0;
    quint8 flags = ClipboardOfferText; // bit 0: text available, bit 1 (wire 19): an image, bit 2 (wire 20): HTML, bit 3 (wire 21): files (alone)
    bool operator==(const ClipboardOffer &) const = default;
};

inline QByteArray frame(const ClipboardOffer &offer)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << offer.seq << offer.flags;
    return frame(Kind::ClipboardOffer, payload);
}

inline std::optional<ClipboardOffer> clipboardOffer(const Record &record)
{
    if (record.kind != Kind::ClipboardOffer || record.payload.size() != 9) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    ClipboardOffer offer;
    stream >> offer.seq >> offer.flags;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !offer.seq || !offer.flags
        || offer.flags > (ClipboardOfferText | ClipboardOfferImage | ClipboardOfferHtml | ClipboardOfferFiles)) return std::nullopt;
    if ((offer.flags & ClipboardOfferFiles) && offer.flags != ClipboardOfferFiles) return std::nullopt; // files are announced alone
    return offer;
}

/** Broker -> worker: the client pasted; send the host's text as it is now. */
struct ClipboardRequest {
    quint64 requestId = 0;
    quint32 maxBytes = MaxClipboardBytes;
    ClipboardKind kind = ClipboardKind::Text;
    /// OPT-078 (wire 21), kind Files only: the peer negotiated huge files (offsets and sizes over 4 GiB). Without it the worker skips
    /// files that large and counts them. Zero for every other kind.
    bool hugeFiles = false;
    bool operator==(const ClipboardRequest &) const = default;
};

inline QByteArray frame(const ClipboardRequest &request)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.requestId << request.maxBytes << quint8(request.kind) << quint8(request.hugeFiles);
    return frame(Kind::ClipboardRequest, payload);
}

inline std::optional<ClipboardRequest> clipboardRequest(const Record &record)
{
    if (record.kind != Kind::ClipboardRequest || record.payload.size() != 14) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    ClipboardRequest request;
    quint8 kind = 0;
    quint8 options = 0;
    stream >> request.requestId >> request.maxBytes >> kind >> options;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !request.requestId || kind > LastClipboardKind || options > 1) return std::nullopt;
    request.kind = ClipboardKind(kind);
    if (options && request.kind != ClipboardKind::Files) return std::nullopt;
    request.hugeFiles = options == 1;
    if (!request.maxBytes || request.maxBytes > maxClipboardBytes(request.kind)) return std::nullopt;
    return request;
}

enum class ClipboardStatus : quint8 { Ok, Empty, Gone, TooLarge, Denied };
constexpr quint8 LastClipboardStatus = quint8(ClipboardStatus::Denied);

/** Worker -> broker: the answer to a ClipboardRequest. Only an Ok answer carries text. */
struct ClipboardData {
    quint64 requestId = 0;
    quint64 seq = 0;
    ClipboardStatus status = ClipboardStatus::Gone;
    QString text; // LF line endings (kind Text)
    ClipboardKind kind = ClipboardKind::Text;
    QByteArray image; // PNG (kind Image); only with status Ok
    QByteArray html; // the HTML document, UTF-8 (kind Html); only with status Ok, never empty
    QByteArray files; // OPT-078 (wire 21): the FileGroupDescriptorW bytes (kind Files); only with status Ok, at least one entry
    quint32 skipped = 0; // OPT-078: entries left out of \a files (symlinks, special files, names too long, over 4 GiB without huge support)
    bool operator==(const ClipboardData &) const = default;
};

namespace detail
{
constexpr qsizetype ClipboardTextHeader = 8 + 8 + 1 + 1;
}

inline QByteArray frame(const ClipboardData &data)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << data.requestId << data.seq << quint8(data.status) << quint8(data.kind);
    if (data.status == ClipboardStatus::Ok) {
        if (data.kind == ClipboardKind::Files) {
            QByteArray skipped;
            QDataStream skippedStream(&skipped, QIODevice::WriteOnly);
            skippedStream.setByteOrder(QDataStream::BigEndian);
            skippedStream << data.skipped;
            payload.append(skipped);
            payload.append(data.files);
        } else {
            payload.append(data.kind == ClipboardKind::Image ? data.image : data.kind == ClipboardKind::Html ? data.html : data.text.toUtf8());
        }
    }
    return frame(Kind::ClipboardData, payload);
}

inline std::optional<ClipboardData> clipboardData(const Record &record)
{
    using detail::ClipboardTextHeader;
    if (record.kind != Kind::ClipboardData || record.payload.size() < ClipboardTextHeader
        || record.payload.size() > ClipboardTextHeader + 4 + qsizetype(MaxClipboardFileListBytes)) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    ClipboardData data;
    quint8 status = 0;
    quint8 kind = 0;
    stream >> data.requestId >> data.seq >> status >> kind;
    if (stream.status() != QDataStream::Ok || !data.requestId || status > LastClipboardStatus || kind > LastClipboardKind) return std::nullopt;
    data.status = ClipboardStatus(status);
    data.kind = ClipboardKind(kind);
    auto body = record.payload.mid(ClipboardTextHeader);
    if (data.status == ClipboardStatus::Ok && data.kind == ClipboardKind::Files) {
        // The skipped count, then the descriptor: at least one entry (4 + 592 bytes) and exactly 4 + n x 592.
        if (body.size() < 4 + 4 + ClipboardFileDescriptor::DescriptorBytes) return std::nullopt;
        QDataStream skippedStream(body.left(4));
        skippedStream.setByteOrder(QDataStream::BigEndian);
        skippedStream >> data.skipped;
        body.remove(0, 4);
        if (qint64(body.size()) > qint64(MaxClipboardFileListBytes)
            || (body.size() - 4) % ClipboardFileDescriptor::DescriptorBytes != 0) return std::nullopt;
        data.files = body;
        return data;
    }
    if (qint64(body.size()) > qint64(maxClipboardBytes(data.kind))) return std::nullopt;
    if (data.status != ClipboardStatus::Ok) return body.isEmpty() ? std::optional<ClipboardData>(data) : std::nullopt;
    if (data.kind == ClipboardKind::Image) {
        if (body.isEmpty()) return std::nullopt; // an Ok image has bytes
        data.image = body;
    } else if (data.kind == ClipboardKind::Html) {
        if (body.isEmpty() || !QByteArrayView(body).isValidUtf8()) return std::nullopt; // an Ok html has a document
        data.html = body;
    } else {
        data.text = QString::fromUtf8(body);
    }
    return data;
}

/** Broker -> worker: the client copied this text. */
struct ClipboardSet {
    quint64 setId = 0;
    quint64 controlGeneration = 0;
    QString text; // LF line endings; empty clears the host clipboard (kind Text)
    ClipboardKind kind = ClipboardKind::Text;
    QByteArray image; // PNG (kind Image); never empty
    QByteArray html; // the HTML document, UTF-8 (kind Html); never empty. `text` is then the plain text beside it (may be empty)
    bool operator==(const ClipboardSet &) const = default;
};

inline QByteArray frame(const ClipboardSet &set)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << set.setId << set.controlGeneration << quint8(set.kind);
    if (set.kind == ClipboardKind::Html) {
        const QByteArray text = set.text.toUtf8();
        stream << quint32(text.size());
        payload.append(text);
        payload.append(set.html);
    } else {
        payload.append(set.kind == ClipboardKind::Image ? set.image : set.text.toUtf8());
    }
    return frame(Kind::ClipboardSet, payload);
}

inline std::optional<ClipboardSet> clipboardSet(const Record &record)
{
    constexpr qsizetype header = 17;
    if (record.kind != Kind::ClipboardSet || record.payload.size() < header
        || record.payload.size() > header + qsizetype(MaxClipboardImageBytes)) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    ClipboardSet set;
    quint8 kind = 0;
    stream >> set.setId >> set.controlGeneration >> kind;
    if (stream.status() != QDataStream::Ok || !set.setId || !set.controlGeneration || kind > LastClipboardKind
        || kind == quint8(ClipboardKind::Files)) return std::nullopt; // files travel client to host in a later wire, never as a set
    set.kind = ClipboardKind(kind);
    const auto body = record.payload.mid(header);
    if (set.kind == ClipboardKind::Html) {
        if (body.size() < 4 || qint64(body.size()) > qint64(MaxClipboardRichSetBytes)) return std::nullopt;
        QDataStream lengthStream(body.left(4));
        lengthStream.setByteOrder(QDataStream::BigEndian);
        quint32 textLength = 0;
        lengthStream >> textLength;
        if (textLength > MaxClipboardBytes || qint64(textLength) > qint64(body.size()) - 4) return std::nullopt;
        const auto text = body.mid(4, textLength);
        set.html = body.mid(4 + textLength);
        if (set.html.isEmpty() || set.html.size() > qsizetype(MaxClipboardHtmlBytes) || !QByteArrayView(set.html).isValidUtf8()
            || !QByteArrayView(text).isValidUtf8()) return std::nullopt;
        set.text = QString::fromUtf8(text);
        return set;
    }
    if (qint64(body.size()) > qint64(maxClipboardBytes(set.kind))) return std::nullopt;
    if (set.kind == ClipboardKind::Image) {
        if (body.isEmpty()) return std::nullopt; // an image copy has bytes; clearing is a text set
        set.image = body;
    } else {
        set.text = QString::fromUtf8(body);
    }
    return set;
}

enum class ClipboardSetStatus : quint8 { Written, Denied, Failed };

/** Worker -> broker: what became of a ClipboardSet. Counted and logged by the broker; never retried. */
struct ClipboardSetResult {
    quint64 setId = 0;
    ClipboardSetStatus status = ClipboardSetStatus::Failed;
    bool operator==(const ClipboardSetResult &) const = default;
};

inline QByteArray frame(const ClipboardSetResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.setId << quint8(result.status);
    return frame(Kind::ClipboardSetResult, payload);
}

inline std::optional<ClipboardSetResult> clipboardSetResult(const Record &record)
{
    if (record.kind != Kind::ClipboardSetResult || record.payload.size() != 9) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    ClipboardSetResult result;
    quint8 status = 0;
    stream >> result.setId >> status;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !result.setId
        || status > quint8(ClipboardSetStatus::Failed)) return std::nullopt;
    result.status = ClipboardSetStatus(status);
    return result;
}

/**
 * The gates the worker applies before it touches the host clipboard, as pure functions (the style of
 * mayApplyQuality). \a desktop is the worker's own knowledge: a greeter worker (not an authenticated desktop)
 * additionally needs policy.greeter, a desktop worker needs it false, so a broker mistake cannot make a
 * desktop write look like a greeter write or the reverse.
 */
inline bool mayApplyClipboardSet(const ClipboardSet &set, const ControlState &control, const ClipboardPolicy &policy,
                                 quint64 lastSetId, bool desktop)
{
    return control.active && control.generation != 0 && set.controlGeneration == control.generation
        && policy.controlGeneration == control.generation && policy.clientToHost && policy.greeter == !desktop
        && (set.kind == ClipboardKind::Image ? !set.image.isEmpty() && quint64(set.image.size()) <= MaxClipboardImageBytes
            : set.kind == ClipboardKind::Html ? !set.html.isEmpty() && quint64(set.html.size()) <= MaxClipboardHtmlBytes
                    && quint64(set.text.toUtf8().size()) <= MaxClipboardBytes
                                              : quint64(set.text.toUtf8().size()) <= MaxClipboardBytes)
        && set.setId > lastSetId;
}

inline bool mayAnswerClipboardRequest(const ClipboardRequest &request, const ControlState &control, const ClipboardPolicy &policy,
                                      quint64 lastRequestId, bool desktop)
{
    return control.active && policy.controlGeneration == control.generation && policy.hostToClient && !policy.greeter && desktop
        && request.requestId > lastRequestId && (request.kind != ClipboardKind::Files || policy.filesHostToClient);
}

inline bool mayOfferClipboard(const ClipboardPolicy &policy, bool desktop)
{
    return policy.hostToClient && !policy.greeter && desktop;
}

// ---- OPT-078 host-to-client file copy (wire 21) ----
// Worker side: the user's files stay where they are; nothing is copied. The broker (which holds the socket and the pacer) asks for
// ranges inside the client's request window and writes each response when the pacer allows. File names never appear in these
// records (the descriptor travels in a Files ClipboardData) and are never logged.

/** Broker -> worker: the client locked the offer \a seq (CB_LOCK_CLIPDATA): keep its snapshot until the matching FileUnlock. */
struct FileLock {
    quint64 seq = 0;
    quint32 clipDataId = 0;
    bool operator==(const FileLock &) const = default;
};
/** Broker -> worker: the client unlocked \a clipDataId (or the broker dropped its lock). */
struct FileUnlock {
    quint32 clipDataId = 0;
    bool operator==(const FileUnlock &) const = default;
};
/** Broker -> worker: read \a length bytes at \a offset of entry \a index of offer \a seq (the broker has validated the range). */
struct FileRead {
    quint64 readId = 0;
    quint64 seq = 0;
    quint32 index = 0;
    quint64 offset = 0;
    quint32 length = 0;
    bool operator==(const FileRead &) const = default;
};
enum class FileReadStatus : quint8 { Ok, Changed, Gone, Denied, IoError, NoSuchOffer };
constexpr quint8 LastFileReadStatus = quint8(FileReadStatus::NoSuchOffer);
/** Worker -> broker: the answer to a FileRead. Only Ok carries bytes, exactly as many as asked. */
struct FileReadResult {
    quint64 readId = 0;
    FileReadStatus status = FileReadStatus::Gone;
    QByteArray data;
    bool operator==(const FileReadResult &) const = default;
};
/** Broker -> worker: the offer \a seq is over for the client (cancel, a new copy, the client left); drop its queued reads. 0 = every offer. */
struct FileCancel {
    quint64 seq = 0;
    bool operator==(const FileCancel &) const = default;
};
enum class FileProgressState : quint8 { Reading, Done, Failed };
constexpr quint8 LastFileProgressState = quint8(FileProgressState::Failed);
/** Worker -> broker, at most 4 a second while reading: bytes of the offer read from disk so far (counts only). */
struct FileProgress {
    quint64 seq = 0;
    quint64 doneBytes = 0;
    quint64 totalBytes = 0;
    FileProgressState state = FileProgressState::Reading;
    bool operator==(const FileProgress &) const = default;
};
constexpr int FileProgressMaxPerSecond = 4;

inline QByteArray frame(const FileLock &lock)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << lock.seq << lock.clipDataId;
    return frame(Kind::FileLock, payload);
}
inline std::optional<FileLock> fileLock(const Record &record)
{
    if (record.kind != Kind::FileLock || record.payload.size() != 12) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    FileLock lock;
    stream >> lock.seq >> lock.clipDataId;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !lock.seq) return std::nullopt;
    return lock;
}

inline QByteArray frame(const FileUnlock &unlock)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << unlock.clipDataId;
    return frame(Kind::FileUnlock, payload);
}
inline std::optional<FileUnlock> fileUnlock(const Record &record)
{
    if (record.kind != Kind::FileUnlock || record.payload.size() != 4) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    FileUnlock unlock;
    stream >> unlock.clipDataId;
    if (stream.status() != QDataStream::Ok || !stream.atEnd()) return std::nullopt;
    return unlock;
}

inline QByteArray frame(const FileRead &read)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << read.readId << read.seq << read.index << read.offset << read.length;
    return frame(Kind::FileRead, payload);
}
inline std::optional<FileRead> fileRead(const Record &record)
{
    if (record.kind != Kind::FileRead || record.payload.size() != 32) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    FileRead read;
    stream >> read.readId >> read.seq >> read.index >> read.offset >> read.length;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !read.readId || !read.seq || !read.length || read.length > MaxFileReadBytes
        || read.offset > ~quint64(0) - read.length) return std::nullopt;
    return read;
}

inline QByteArray frame(const FileReadResult &result)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << result.readId << quint8(result.status);
    if (result.status == FileReadStatus::Ok) payload.append(result.data);
    return frame(Kind::FileReadResult, payload);
}
inline std::optional<FileReadResult> fileReadResult(const Record &record)
{
    constexpr qsizetype Header = 8 + 1;
    if (record.kind != Kind::FileReadResult || record.payload.size() < Header || record.payload.size() > Header + qsizetype(MaxFileReadBytes)) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    FileReadResult result;
    quint8 status = 0;
    stream >> result.readId >> status;
    if (stream.status() != QDataStream::Ok || !result.readId || status > LastFileReadStatus) return std::nullopt;
    result.status = FileReadStatus(status);
    const auto body = record.payload.mid(Header);
    if (result.status != FileReadStatus::Ok) return body.isEmpty() ? std::optional<FileReadResult>(result) : std::nullopt;
    if (body.isEmpty()) return std::nullopt; // an Ok read has bytes
    result.data = body;
    return result;
}

inline QByteArray frame(const FileCancel &cancel)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << cancel.seq;
    return frame(Kind::FileCancel, payload);
}
inline std::optional<FileCancel> fileCancel(const Record &record)
{
    if (record.kind != Kind::FileCancel || record.payload.size() != 8) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    FileCancel cancel;
    stream >> cancel.seq;
    if (stream.status() != QDataStream::Ok || !stream.atEnd()) return std::nullopt;
    return cancel;
}

inline QByteArray frame(const FileProgress &progress)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << progress.seq << progress.doneBytes << progress.totalBytes << quint8(progress.state);
    return frame(Kind::FileProgress, payload);
}
inline std::optional<FileProgress> fileProgress(const Record &record)
{
    if (record.kind != Kind::FileProgress || record.payload.size() != 25) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    FileProgress progress;
    quint8 state = 0;
    stream >> progress.seq >> progress.doneBytes >> progress.totalBytes >> state;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !progress.seq || state > LastFileProgressState || progress.doneBytes > progress.totalBytes) return std::nullopt;
    progress.state = FileProgressState(state);
    return progress;
}

/** The worker's gate for FileLock / FileRead: an active controller, a current policy that allows files host to client, a desktop worker. */
inline bool mayServeFiles(const ControlState &control, const ClipboardPolicy &policy, bool desktop)
{
    return control.active && control.generation != 0 && policy.controlGeneration == control.generation && policy.hostToClient
        && policy.filesHostToClient && !policy.greeter && desktop;
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
    bool complete = true; // False for a capture projection; never grants full-layout mutations.
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
    stream << quint8(topology.complete ? 1 : 0);
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
    quint8 complete = 0;
    stream >> complete;
    result.complete = complete == 1;
    return stream.status() == QDataStream::Ok && stream.atEnd() && complete <= 1 && (count == 0 || (primaries == 1 && priorities.size() == count))
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
        RelativePointer,
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
           << quint8(std::clamp(settings.av1Tiles, 0, 64))
           << qint32(config.chroma.motionGapMs) << qint32(config.chroma.restMs) << qint32(config.chroma.maxGapMs) << config.chromaEnabled
           << quint8(config.capture.mode) << qint32(config.capture.index)
           << quint8(config.consoleVirtual.enabled) << quint8(config.consoleVirtual.policy) << quint8(config.consoleVirtual.layout)
           << config.consoleVirtual.fallback << config.consoleVirtual.client.desktopSize << quint32(config.consoleVirtual.client.monitors.size());
    for (const auto &monitor : config.consoleVirtual.client.monitors)
        stream << qint32(monitor.geometry.x()) << qint32(monitor.geometry.y())
               << qint32(monitor.geometry.width()) << qint32(monitor.geometry.height()) << quint8(monitor.primary);
    // Wire 14: Layout::Mapped monitors and mapping (both empty for every other layout).
    stream << quint32(config.consoleVirtual.mappedMonitors.size());
    for (const auto &monitor : config.consoleVirtual.mappedMonitors)
        stream << monitor.id << qint32(monitor.geometry.x()) << qint32(monitor.geometry.y())
               << qint32(monitor.geometry.width()) << qint32(monitor.geometry.height()) << quint16(monitor.scalePercent)
               << quint8(monitor.primary);
    stream << quint32(config.consoleVirtual.mapping.size());
    for (const auto &entry : config.consoleVirtual.mapping) stream << entry.host << entry.monitor;
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
    qint32 motionGap = 0, rest = 0, maxGap = 0, monitorIndex = 0;
    quint8 captureMode = 0;
    stream >> config.generation >> codec >> hasSettings >> settings.hardware >> preset >> settings.targetKbps >> maxFrameRate >> config.frameRate >> config.statsWanted
        >> av1Tiles >> motionGap >> rest >> maxGap >> config.chromaEnabled >> captureMode >> monitorIndex;
    config.chroma = {motionGap, rest, maxGap};
    config.capture = {MonitorCapturePolicy::Mode(captureMode), monitorIndex};
    quint8 virtualEnabled = 0, virtualPolicy = 0, virtualLayout = 0;
    quint32 monitorCount = 0;
    stream >> virtualEnabled >> virtualPolicy >> virtualLayout >> config.consoleVirtual.fallback
        >> config.consoleVirtual.client.desktopSize >> monitorCount;
    if (stream.status() != QDataStream::Ok || virtualEnabled > 1
        || virtualPolicy > quint8(ConsoleVirtualOutputPolicy::Policy::Extend)
        || virtualLayout > quint8(ConsoleVirtualOutputPolicy::Layout::Mapped)
        || monitorCount > ClientDisplay::MaxMonitors) return {};
    config.consoleVirtual.enabled = virtualEnabled;
    config.consoleVirtual.policy = ConsoleVirtualOutputPolicy::Policy(virtualPolicy);
    config.consoleVirtual.layout = ConsoleVirtualOutputPolicy::Layout(virtualLayout);
    for (quint32 i = 0; i < monitorCount; ++i) {
        qint32 x = 0, y = 0, width = 0, height = 0;
        quint8 primary = 0;
        stream >> x >> y >> width >> height >> primary;
        if (stream.status() != QDataStream::Ok || x < -ClientDisplay::MaxCoordinate || x > ClientDisplay::MaxCoordinate
            || y < -ClientDisplay::MaxCoordinate || y > ClientDisplay::MaxCoordinate || primary > 1
            || !ClientDisplay::usable(QSize(width, height))) return {};
        config.consoleVirtual.client.monitors.append({QRect(x, y, width, height), bool(primary)});
    }
    quint32 mappedCount = 0;
    stream >> mappedCount;
    if (stream.status() != QDataStream::Ok || mappedCount > ConsoleVirtualOutputPolicy::MaxMappedMonitors) return {};
    for (quint32 i = 0; i < mappedCount; ++i) {
        ConsoleVirtualOutputPolicy::MappedMonitor monitor;
        qint32 x = 0, y = 0, width = 0, height = 0;
        quint16 scalePercent = 0;
        quint8 primary = 0;
        stream >> monitor.id >> x >> y >> width >> height >> scalePercent >> primary;
        if (stream.status() != QDataStream::Ok || primary > 1 || monitor.id.size() > 64) return {};
        monitor.geometry = QRect(x, y, width, height);
        monitor.scalePercent = scalePercent;
        monitor.primary = primary;
        config.consoleVirtual.mappedMonitors.append(monitor);
    }
    quint32 entryCount = 0;
    stream >> entryCount;
    if (stream.status() != QDataStream::Ok || entryCount > ConsoleVirtualOutputPolicy::MaxMappedMonitors) return {};
    for (quint32 i = 0; i < entryCount; ++i) {
        ConsoleVirtualOutputPolicy::MapEntry entry;
        stream >> entry.host >> entry.monitor;
        if (stream.status() != QDataStream::Ok || entry.host.size() > ConsoleVirtualOutputPolicy::MaxMappedName || entry.monitor.size() > 64) return {};
        config.consoleVirtual.mapping.append(entry);
    }
    const auto decoded = codecFromWire(codec);
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !config.generation || !decoded || preset > quint8(CodecPolicy::Preset::Fastest)
        || maxFrameRate < 0 || maxFrameRate > 240 || config.frameRate < 1 || config.frameRate > 240 || settings.targetKbps > 1000000 || av1Tiles > 64
        || !config.chroma.isValid() || !config.capture.isValid() || !config.consoleVirtual.isValid()
        || (config.consoleVirtual.enabled && config.capture != MonitorCapturePolicy{})) {
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
    stream << quint8(report.event) << wireCodec(report.codec) << report.hardware << report.chromaCapable;
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
    stream >> event >> codec >> report.hardware >> report.chromaCapable;
    const auto decoded = codecFromWire(codec);
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !decoded
        || (event != quint8(EncoderReport::Event::Unavailable) && event != quint8(EncoderReport::Event::Backend)
            && event != quint8(EncoderReport::Event::ChromaCapability))) {
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

inline QByteArray frame(const ChromaTiming &report)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    const auto &r = report.timing;
    stream << report.generation << wireCodec(report.codec) << qint32(report.monitorIndex)
           << qint32(r.frames) << qint32(r.auxSent) << qint32(r.auxSkippedMotion)
           << qint32(r.auxRestRefresh) << qint32(r.auxMaxGap) << qint32(r.rewriteFailures)
           << r.splitVariant << r.downloadAvg << r.downloadMax << r.splitAvg << r.splitMax
           << r.uploadAvg << r.uploadMax << r.encodeMainAvg << r.encodeMainMax
           << r.encodeAuxAvg << r.encodeAuxMax << r.downloadMin << r.splitMin << r.uploadMin
           << r.encodeMainMin << r.encodeAuxMin;
    return frame(Kind::ChromaTiming, payload);
}

inline std::optional<ChromaTiming> chromaTiming(const Record &record)
{
    if (record.kind != Kind::ChromaTiming || record.payload.size() > 256) return std::nullopt;
    QDataStream stream(record.payload);
    stream.setByteOrder(QDataStream::BigEndian);
    ChromaTiming report;
    quint8 codec = 0;
    qint32 monitor = 0, frames = 0, sent = 0, skipped = 0, rest = 0, gap = 0, failures = 0;
    auto &r = report.timing;
    stream >> report.generation >> codec >> monitor >> frames >> sent >> skipped >> rest >> gap >> failures
           >> r.splitVariant >> r.downloadAvg >> r.downloadMax >> r.splitAvg >> r.splitMax
           >> r.uploadAvg >> r.uploadMax >> r.encodeMainAvg >> r.encodeMainMax
           >> r.encodeAuxAvg >> r.encodeAuxMax >> r.downloadMin >> r.splitMin >> r.uploadMin
           >> r.encodeMainMin >> r.encodeAuxMin;
    const auto parsedCodec = codecFromWire(codec);
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || !report.generation
        || !parsedCodec || (*parsedCodec != VideoCodec::Avc444 && *parsedCodec != VideoCodec::Avc444v2)
        || monitor < 0 || monitor >= 16 || r.splitVariant.isEmpty() || r.splitVariant.size() > 32) return std::nullopt;
    for (const auto ch : r.splitVariant) {
        const auto c = ch.unicode();
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return std::nullopt;
    }
    // At-rest auxiliary refreshes are independent of main-frame counters.
    for (const auto count : {frames, sent, skipped, rest, gap, failures})
        if (count < 0 || count > 100000) return std::nullopt;
    for (const auto pair : {std::pair{r.downloadAvg, r.downloadMax}, std::pair{r.splitAvg, r.splitMax},
                           std::pair{r.uploadAvg, r.uploadMax}, std::pair{r.encodeMainAvg, r.encodeMainMax},
                           std::pair{r.encodeAuxAvg, r.encodeAuxMax}})
        if (pair.first < 0 || pair.first > pair.second || pair.second > MaxChromaTimingUs) return std::nullopt;
    for (const auto pair : {std::pair{r.downloadMin, r.downloadAvg}, std::pair{r.splitMin, r.splitAvg},
                           std::pair{r.uploadMin, r.uploadAvg}, std::pair{r.encodeMainMin, r.encodeMainAvg},
                           std::pair{r.encodeAuxMin, r.encodeAuxAvg}})
        if (pair.first < 0 || pair.first > pair.second) return std::nullopt;
    report.codec = *parsedCodec;
    report.monitorIndex = monitor;
    r.frames = frames; r.auxSent = sent; r.auxSkippedMotion = skipped;
    r.auxRestRefresh = rest; r.auxMaxGap = gap; r.rewriteFailures = failures;
    return report;
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
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || type > quint8(Input::Type::RelativePointer)
        || eventType < int(QEvent::None) || eventType > int(QEvent::User)) {
        return std::nullopt;
    }
    if (Input::Type(type) == Input::Type::RelativePointer
        && ((!std::isfinite(result.position.x()) || !std::isfinite(result.position.y())
             || std::abs(result.position.x()) > 32768 || std::abs(result.position.y()) > 32768)
            || (eventType != QEvent::MouseMove && eventType != QEvent::MouseButtonPress
                && eventType != QEvent::MouseButtonRelease && eventType != QEvent::Wheel))) return std::nullopt;
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
