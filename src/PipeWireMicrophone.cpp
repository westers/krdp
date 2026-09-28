// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PipeWireMicrophone.h"
#include <QHash>
#include <QMutexLocker>
#include <map>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/utils/result.h>

namespace KRdp
{
namespace { constexpr uint32_t Rate = 48000, Channels = 2, FrameBytes = 4, MaxQueued = Rate * FrameBytes / 2; }

/**
 * Who consumes this source, from the PipeWire registry (N1): every link and the nodes at the
 * other end of the links from this node. A consumer node whose properties mark it as a level
 * meter (pipewire-pulse turns a PulseAudio peak-detect stream into `stream.monitor` and
 * `resample.peaks`) does not count. Registry and node events run on the thread loop; the
 * counts are read from any thread under the mutex.
 */
struct PipeWireMicrophone::Graph {
    struct Link {
        uint32_t output = SPA_ID_INVALID;
        uint32_t input = SPA_ID_INVALID;
    };
    struct Consumer {
        Graph *graph = nullptr;
        uint32_t id = SPA_ID_INVALID;
        pw_proxy *proxy = nullptr;
        spa_hook listener{};
        bool known = false; // its properties arrived
        bool meter = false;
    };

    mutable QMutex mutex;
    // The stream, for its node id: set before the loop runs, destroyed only after it stopped (unlike
    // PipeWireMicrophone::m_stream, which stop() clears while the loop can still deliver events).
    pw_stream *stream = nullptr;
    uint32_t node = SPA_ID_INVALID; // this source's node
    QHash<uint32_t, Link> links; // link global id -> nodes
    std::map<uint32_t, std::unique_ptr<Consumer>> consumers; // input node id -> watch
    pw_registry *registry = nullptr;
    spa_hook registryListener{};

    static bool truthy(const spa_dict *props, const char *key)
    {
        const char *value = props ? spa_dict_lookup(props, key) : nullptr;
        return value && (spa_streq(value, "true") || spa_streq(value, "1"));
    }
    static uint32_t idOf(const spa_dict *props, const char *key)
    {
        const char *value = props ? spa_dict_lookup(props, key) : nullptr;
        uint32_t id = SPA_ID_INVALID;
        return value && spa_atou32(value, &id, 10) ? id : SPA_ID_INVALID;
    }

    void watchConsumer(uint32_t id) // mutex held
    {
        if (consumers.count(id) || !registry) return;
        auto consumer = std::make_unique<Consumer>();
        consumer->graph = this;
        consumer->id = id;
        consumer->proxy = static_cast<pw_proxy *>(pw_registry_bind(registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (!consumer->proxy) return;
        static const pw_node_events events = [] {
            pw_node_events result{};
            result.version = PW_VERSION_NODE_EVENTS;
            result.info = [](void *data, const pw_node_info *info) {
                auto *consumer = static_cast<Consumer *>(data);
                if (!info || !(info->change_mask & PW_NODE_CHANGE_MASK_PROPS)) return;
                QMutexLocker lock(&consumer->graph->mutex);
                consumer->known = true;
                consumer->meter = truthy(info->props, PW_KEY_STREAM_MONITOR) || truthy(info->props, "resample.peaks");
            };
            return result;
        }();
        pw_node_add_listener(reinterpret_cast<pw_node *>(consumer->proxy), &consumer->listener, &events, consumer.get());
        consumers.emplace(id, std::move(consumer));
    }
    void dropConsumer(uint32_t id) // mutex held
    {
        const auto it = consumers.find(id);
        if (it == consumers.end()) return;
        spa_hook_remove(&it->second->listener);
        pw_proxy_destroy(it->second->proxy);
        consumers.erase(it);
    }

    void start(pw_stream *source, pw_core *core)
    {
        stream = source;
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (!registry) return;
        static const pw_registry_events events = [] {
            pw_registry_events result{};
            result.version = PW_VERSION_REGISTRY_EVENTS;
            result.global = [](void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props) {
                auto *graph = static_cast<Graph *>(data);
                if (!spa_streq(type, PW_TYPE_INTERFACE_Link)) return;
                const Link link{idOf(props, PW_KEY_LINK_OUTPUT_NODE), idOf(props, PW_KEY_LINK_INPUT_NODE)};
                QMutexLocker lock(&graph->mutex);
                graph->links.insert(id, link);
                if (link.output == graph->node && link.input != SPA_ID_INVALID) graph->watchConsumer(link.input);
            };
            result.global_remove = [](void *data, uint32_t id) {
                auto *graph = static_cast<Graph *>(data);
                QMutexLocker lock(&graph->mutex);
                graph->links.remove(id);
                graph->dropConsumer(id); // a node that went away
            };
            return result;
        }();
        pw_registry_add_listener(registry, &registryListener, &events, this);
    }
    /// The source's node id is known (the stream reached PAUSED): watch the links already there.
    void updateNode()
    {
        const uint32_t id = stream ? pw_stream_get_node_id(stream) : SPA_ID_INVALID;
        QMutexLocker lock(&mutex);
        if (node == id) return;
        node = id;
        for (const Link &link : std::as_const(links)) {
            if (link.output == node && link.input != SPA_ID_INVALID) watchConsumer(link.input);
        }
    }
    bool recording() const
    {
        QMutexLocker lock(&mutex);
        if (node == SPA_ID_INVALID) return false;
        for (const Link &link : links) {
            if (link.output != node) continue;
            const auto it = consumers.find(link.input);
            // No watch (the bind failed): count it. A watch whose properties have not arrived
            // (one round trip) waits, so opening a level meter never flashes "in use".
            if (it == consumers.end() || (it->second->known && !it->second->meter)) return true;
        }
        return false;
    }
    /// With the thread loop stopped (no callback can run).
    void stop()
    {
        QMutexLocker lock(&mutex);
        while (!consumers.empty()) dropConsumer(consumers.begin()->first);
        if (registry) {
            spa_hook_remove(&registryListener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
            registry = nullptr;
        }
        links.clear();
        node = SPA_ID_INVALID;
        stream = nullptr;
    }
};

PipeWireMicrophone::PipeWireMicrophone()
    : m_graph(std::make_unique<Graph>()) // for the object's lifetime: consumerActive() reads it from any thread
{
}
PipeWireMicrophone::~PipeWireMicrophone() { stop(); }
bool PipeWireMicrophone::consumerActive() const { return m_graph->recording(); }
bool PipeWireMicrophone::start(const QString &id)
{
    QMutexLocker lock(&m_mutex);
    if (m_stream) return true;
    m_state = State::Starting;
    m_runtime.acquire();
    m_loop = pw_thread_loop_new("krdp-remote-mic", nullptr);
    if (!m_loop) { m_runtime.release(); m_state = State::Failed; return false; }
    // PipeWire retains this pointer for the life of the stream; a stack-local
    // events table becomes invalid as soon as start() returns and crashes the
    // first graph-process callback.
    static const pw_stream_events events = [] {
        pw_stream_events result{};
        result.version = PW_VERSION_STREAM_EVENTS;
        result.process = PipeWireMicrophone::process;
        result.state_changed = [](void *data, pw_stream_state, pw_stream_state state, const char *) {
            auto *self = static_cast<PipeWireMicrophone *>(data);
            self->m_streaming = state == PW_STREAM_STATE_STREAMING;
            if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
                self->m_graph->updateNode();
            }
            // PAUSED is a registered source with no consumer yet. Do not wait
            // for STREAMING: applications choose when to open the microphone.
            if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
                self->m_state = State::Ready;
            } else if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
                self->m_state = State::Failed;
            } else {
                self->m_state = State::Starting;
            }
        };
        return result;
    }();
    const QByteArray nodeName = QByteArrayLiteral("krdp.remote-microphone.") + id.toUtf8();
    m_stream = pw_stream_new_simple(pw_thread_loop_get_loop(m_loop), "KRDP Remote Microphone",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_CLASS, "Audio/Source", PW_KEY_NODE_NAME, nodeName.constData(), PW_KEY_NODE_DESCRIPTION, "KRDP Remote Microphone", nullptr), &events, this);
    uint8_t storage[1024]; spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    spa_audio_info_raw format{}; format.format = SPA_AUDIO_FORMAT_S16_LE; format.rate = Rate; format.channels = Channels; format.position[0] = SPA_AUDIO_CHANNEL_FL; format.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    const auto connected = [&] {
        if (!m_stream || pw_stream_connect(m_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) return false;
        // The simple stream's core exists from pw_stream_connect() on. Before the loop runs, so
        // no registry event can race this.
        if (pw_core *core = pw_stream_get_core(m_stream)) m_graph->start(m_stream, core);
        return true;
    };
    if (!connected() || pw_thread_loop_start(m_loop) < 0) {
        m_graph->stop();
        if (m_stream) pw_stream_destroy(m_stream);
        m_stream = nullptr;
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
        m_runtime.release();
        m_state = State::Failed;
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
    // The registry and node proxies belong to the stream's core: release them first.
    m_graph->stop();
    if (stream) { pw_stream_disconnect(stream); pw_stream_destroy(stream); }
    if (loop) pw_thread_loop_destroy(loop);
    m_runtime.release();
    m_streaming = false;
    m_state = State::Stopped;
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
