// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PipeWireAudioPlayback.h"
#include "PipeWireAudioRouting.h"

#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>

#include <utility>

namespace KRdp
{
namespace
{
constexpr uint32_t Rate = 44100;
constexpr uint32_t Channels = 2;
constexpr uint32_t FrameBytes = 4;
constexpr uint32_t MaxQueued = Rate * FrameBytes / 2;

struct MetadataValue {
    QString value;
    QString type;
    bool present = false;
};

MetadataValue metadataValue(quint32 subject, const QString &key)
{
    QProcess metadata;
    metadata.start(QStringLiteral("pw-metadata"), {QStringLiteral("-n"), QStringLiteral("default"), QString::number(subject), key});
    if (!metadata.waitForFinished(1000) || metadata.exitStatus() != QProcess::NormalExit || metadata.exitCode() != 0) {
        return {};
    }
    const QRegularExpression match(QStringLiteral("key:'%1' value:'([^']*)' type:'([^']*)'").arg(QRegularExpression::escape(key)));
    const auto found = match.match(QString::fromUtf8(metadata.readAllStandardOutput()));
    return found.hasMatch() ? MetadataValue{found.captured(1), found.captured(2), true} : MetadataValue{};
}

bool setMetadataValue(quint32 subject, const QString &key, const QString &value, const QString &type)
{
    QProcess metadata;
    metadata.start(QStringLiteral("pw-metadata"),
                   {QStringLiteral("-n"), QStringLiteral("default"), QString::number(subject), key, value, type});
    return metadata.waitForFinished(1000) && metadata.exitStatus() == QProcess::NormalExit && metadata.exitCode() == 0;
}

void clearMetadataValue(quint32 subject, const QString &key)
{
    QProcess metadata;
    metadata.start(QStringLiteral("pw-metadata"), {QStringLiteral("-n"), QStringLiteral("default"), QStringLiteral("-d"), QString::number(subject), key});
    metadata.waitForFinished(1000);
}

constexpr quint32 DefaultMetadataSubject = 0;
}

PipeWireAudioPlayback::~PipeWireAudioPlayback()
{
    stop();
}

bool PipeWireAudioPlayback::start(const QString &targetSink)
{
    QMutexLocker lock(&m_mutex);
    if (m_stream) {
        return true;
    }
    pw_init(nullptr, nullptr);
    m_loop = pw_thread_loop_new("krdp-remote-audio", nullptr);
    if (!m_loop) {
        return false;
    }
    // PipeWire keeps the supplied event table for the stream lifetime.
    static const pw_stream_events events = [] {
        pw_stream_events result{};
        result.version = PW_VERSION_STREAM_EVENTS;
        result.process = PipeWireAudioPlayback::process;
        return result;
    }();
    const QByteArray target = targetSink.toUtf8();
    const bool isolated = !m_isolatedSinkName.isEmpty();
    m_stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(m_loop), "KRDP Remote Audio",
        isolated ? pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Communication",
                                     PW_KEY_MEDIA_CLASS, "Audio/Sink", PW_KEY_NODE_NAME, m_isolatedSinkName.toUtf8().constData(),
                                     PW_KEY_NODE_DESCRIPTION, "KRDP Remote Audio (client-only)", PW_KEY_NODE_VIRTUAL, "true",
                                     PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr)
                 : pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Communication",
                                     "stream.capture.sink", "true",
                                     PW_KEY_TARGET_OBJECT, target.constData(), nullptr),
        &events, this);
    uint8_t storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_S16_LE;
    format.rate = Rate;
    format.channels = Channels;
    format.position[0] = SPA_AUDIO_CHANNEL_FL;
    format.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    if (!m_stream || pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0 || pw_thread_loop_start(m_loop) < 0) {
        if (m_stream) {
            pw_stream_destroy(m_stream);
        }
        m_stream = nullptr;
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
        return false;
    }
    return true;
}

bool PipeWireAudioPlayback::startIsolated(const QString &id)
{
    if (id.isEmpty()) {
        return false;
    }
    m_isolatedSinkName = QStringLiteral("krdp.remote-audio.%1").arg(id);
    m_previousDefaultSink = metadataValue(DefaultMetadataSubject, QStringLiteral("default.audio.sink")).value;
    m_previousConfiguredSink = metadataValue(DefaultMetadataSubject, QStringLiteral("default.configured.audio.sink")).value;
    if (!start({})) {
        m_isolatedSinkName.clear();
        return false;
    }

    const QString selected = QStringLiteral("{\"name\":\"%1\"}").arg(m_isolatedSinkName);
    if (setMetadataValue(DefaultMetadataSubject, QStringLiteral("default.audio.sink"), selected, QStringLiteral("Spa:String:JSON"))
        && setMetadataValue(DefaultMetadataSubject, QStringLiteral("default.configured.audio.sink"), selected, QStringLiteral("Spa:String:JSON"))) {
        // The default only affects streams created from here on. WirePlumber
        // also honours target.object metadata for already running streams,
        // which is the part that makes an in-progress conference go private.
        moveExistingPlaybackStreams();
        return true;
    }
    stop();
    return false;
}

void PipeWireAudioPlayback::stop()
{
    pw_stream *stream = nullptr;
    pw_thread_loop *loop = nullptr;
    {
        QMutexLocker lock(&m_mutex);
        stream = m_stream;
        loop = m_loop;
        m_stream = nullptr;
        m_loop = nullptr;
        m_pending.clear();
    }
    if (!m_isolatedSinkName.isEmpty()) {
        const QString selected = QStringLiteral("{\"name\":\"%1\"}").arg(m_isolatedSinkName);
        // Never overwrite a choice the local user made while this RDP session
        // was active.  We only undo the values that still name our sink.
        if (metadataValue(DefaultMetadataSubject, QStringLiteral("default.audio.sink")).value == selected) {
            if (m_previousDefaultSink.isEmpty()) {
                clearMetadataValue(DefaultMetadataSubject, QStringLiteral("default.audio.sink"));
            } else {
                setMetadataValue(DefaultMetadataSubject, QStringLiteral("default.audio.sink"), m_previousDefaultSink, QStringLiteral("Spa:String:JSON"));
            }
        }
        if (metadataValue(DefaultMetadataSubject, QStringLiteral("default.configured.audio.sink")).value == selected) {
            if (m_previousConfiguredSink.isEmpty()) {
                clearMetadataValue(DefaultMetadataSubject, QStringLiteral("default.configured.audio.sink"));
            } else {
                setMetadataValue(DefaultMetadataSubject, QStringLiteral("default.configured.audio.sink"), m_previousConfiguredSink, QStringLiteral("Spa:String:JSON"));
            }
        }
        restoreMovedPlaybackStreams();
        m_isolatedSinkName.clear();
        m_previousDefaultSink.clear();
        m_previousConfiguredSink.clear();
    }
    if (loop) {
        pw_thread_loop_stop(loop);
    }
    if (stream) {
        pw_stream_disconnect(stream);
        pw_stream_destroy(stream);
    }
    if (loop) {
        pw_thread_loop_destroy(loop);
    }
}

void PipeWireAudioPlayback::moveExistingPlaybackStreams()
{
    QProcess dump;
    dump.start(QStringLiteral("pw-dump"));
    if (!dump.waitForFinished(2000) || dump.exitStatus() != QProcess::NormalExit || dump.exitCode() != 0) {
        qWarning() << "Could not inspect PipeWire playback streams for isolated audio";
        return;
    }
    const auto streams = PipeWireAudioRouting::movablePlaybackStreams(dump.readAllStandardOutput());
    for (const PipeWireAudioRouting::PlaybackStream &stream : streams) {
        const quint32 streamId = stream.id;
        const MetadataValue previous = metadataValue(streamId, QStringLiteral("target.object"));
        // A node-name target is supported by WirePlumber's defined-target
        // policy and avoids confusing a PipeWire global id with object.serial.
        if (setMetadataValue(streamId, QStringLiteral("target.object"), m_isolatedSinkName, QStringLiteral("Spa:String"))) {
            m_movedStreams.append({streamId, previous.value, previous.type, previous.present});
        }
    }
    if (!m_movedStreams.isEmpty()) {
        qInfo() << "Moved" << m_movedStreams.size() << "existing PipeWire playback stream(s) to the remote-only sink";
    }
}

void PipeWireAudioPlayback::restoreMovedPlaybackStreams()
{
    for (const MovedStream &stream : std::as_const(m_movedStreams)) {
        const MetadataValue current = metadataValue(stream.id, QStringLiteral("target.object"));
        if (!current.present || current.value != m_isolatedSinkName) {
            continue; // The local user or application chose a new target.
        }
        if (stream.hadPreviousTarget) {
            setMetadataValue(stream.id, QStringLiteral("target.object"), stream.previousTarget, stream.previousType);
        } else {
            clearMetadataValue(stream.id, QStringLiteral("target.object"));
        }
    }
    m_movedStreams.clear();
}

QByteArray PipeWireAudioPlayback::take()
{
    QMutexLocker lock(&m_mutex);
    // RDPSND is a realtime channel too: send 20-ms PCM packets rather than
    // letting a delayed session loop turn the bounded capture queue into a
    // single half-second burst.
    const int bytes = qMin(m_pending.size(), int(Rate * FrameBytes / 50));
    QByteArray result = m_pending.left(bytes);
    m_pending.remove(0, bytes);
    return result;
}

void PipeWireAudioPlayback::process(void *data)
{
    static_cast<PipeWireAudioPlayback *>(data)->process();
}

void PipeWireAudioPlayback::process()
{
    QMutexLocker lock(&m_mutex);
    if (!m_stream) {
        return;
    }
    pw_buffer *buffer = pw_stream_dequeue_buffer(m_stream);
    if (!buffer || !buffer->buffer || !buffer->buffer->n_datas) {
        return;
    }
    spa_data &data = buffer->buffer->datas[0];
    if (data.data && data.chunk && data.chunk->size) {
        const auto *input = static_cast<const char *>(data.data) + data.chunk->offset;
        m_pending.append(input, int(data.chunk->size));
        if (m_pending.size() > int(MaxQueued)) {
            m_pending.remove(0, m_pending.size() - int(MaxQueued));
        }
    }
    pw_stream_queue_buffer(m_stream, buffer);
}
}
