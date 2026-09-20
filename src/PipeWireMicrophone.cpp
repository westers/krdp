// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PipeWireMicrophone.h"
#include <QMutexLocker>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>

namespace KRdp
{
namespace { constexpr uint32_t Rate = 48000, Channels = 2, FrameBytes = 4, MaxQueued = Rate * FrameBytes / 2; }
PipeWireMicrophone::~PipeWireMicrophone() { stop(); }
bool PipeWireMicrophone::start(const QString &id)
{
    QMutexLocker lock(&m_mutex);
    if (m_stream) return true;
    pw_init(nullptr, nullptr);
    m_loop = pw_thread_loop_new("krdp-remote-mic", nullptr);
    if (!m_loop) return false;
    // PipeWire retains this pointer for the life of the stream; a stack-local
    // events table becomes invalid as soon as start() returns and crashes the
    // first graph-process callback.
    static const pw_stream_events events = [] {
        pw_stream_events result{};
        result.version = PW_VERSION_STREAM_EVENTS;
        result.process = PipeWireMicrophone::process;
        return result;
    }();
    const QByteArray nodeName = QByteArrayLiteral("krdp.remote-microphone.") + id.toUtf8();
    m_stream = pw_stream_new_simple(pw_thread_loop_get_loop(m_loop), "KRDP Remote Microphone",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_CLASS, "Audio/Source", PW_KEY_NODE_NAME, nodeName.constData(), PW_KEY_NODE_DESCRIPTION, "KRDP Remote Microphone", nullptr), &events, this);
    uint8_t storage[1024]; spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    spa_audio_info_raw format{}; format.format = SPA_AUDIO_FORMAT_S16_LE; format.rate = Rate; format.channels = Channels; format.position[0] = SPA_AUDIO_CHANNEL_FL; format.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    if (!m_stream || pw_stream_connect(m_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0 || pw_thread_loop_start(m_loop) < 0) {
        if (m_stream) pw_stream_destroy(m_stream);
        m_stream = nullptr;
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
        return false;
    }
    return true;
}
void PipeWireMicrophone::stop()
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
    // process() takes m_mutex. Stop the thread outside it or a pending RT
    // callback can deadlock teardown waiting for this lock.
    if (loop) pw_thread_loop_stop(loop);
    if (stream) { pw_stream_disconnect(stream); pw_stream_destroy(stream); }
    if (loop) pw_thread_loop_destroy(loop);
}
void PipeWireMicrophone::write(const QByteArray &pcm)
{
    QMutexLocker lock(&m_mutex); if (!m_stream) return; m_pending.append(pcm); if (m_pending.size() > int(MaxQueued)) m_pending.remove(0, m_pending.size() - int(MaxQueued));
}
void PipeWireMicrophone::process(void *data) { static_cast<PipeWireMicrophone *>(data)->process(); }
void PipeWireMicrophone::process()
{
    QMutexLocker lock(&m_mutex); if (!m_stream) return; pw_buffer *buffer = pw_stream_dequeue_buffer(m_stream); if (!buffer || !buffer->buffer || !buffer->buffer->n_datas) return;
    spa_data &data = buffer->buffer->datas[0]; if (!data.data) { pw_stream_queue_buffer(m_stream, buffer); return; }
    const uint32_t bytes = data.maxsize - data.chunk->offset; const uint32_t count = qMin(bytes, uint32_t(m_pending.size()));
    if (count) { memcpy(static_cast<uint8_t *>(data.data) + data.chunk->offset, m_pending.constData(), count); m_pending.remove(0, int(count)); }
    if (count < bytes) memset(static_cast<uint8_t *>(data.data) + data.chunk->offset + count, 0, bytes - count);
    data.chunk->size = bytes; data.chunk->stride = FrameBytes; pw_stream_queue_buffer(m_stream, buffer);
}
}
