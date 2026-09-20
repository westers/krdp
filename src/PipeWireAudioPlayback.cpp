// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PipeWireAudioPlayback.h"

#include <QMutexLocker>
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
    m_stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(m_loop), "KRDP Remote Audio",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_TARGET_OBJECT,
                          target.constData(), nullptr),
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
