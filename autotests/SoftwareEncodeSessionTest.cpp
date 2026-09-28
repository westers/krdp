// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX2: end-to-end, headless checks of the KPipeWire that KRdp links.
//
// F1 (software H.264 fallback): VA-API is made unusable for the whole process
// (LIBVA_DRIVER_NAME names a driver that does not exist), yet a
// PipeWireEncodedStream asked for H264Main must still deliver Annex-B H.264
// with an SPS and a key frame: KPipeWire falls back to libx264/libopenh264 on
// its own. A second case forces libx264 through KPIPEWIRE_FORCE_ENCODER, the
// "software only" knob.
//
// F5 (per-session fd leak): ten encode sessions in a row (create, start,
// first packet, stop, destroy) must leave /proc/self/fd and /proc/self/task
// at the baseline taken after one warm-up session.
//
// WS-E (software HEVC/AV1): with VA-API broken the same way, a stream configured exactly as
// KRdp's session configures it for software HEVC or AV1 (EncoderSelection::apply() with the
// software backend: SoftwareOnly policy, limited colour range) must deliver decodable HEVC/AV1,
// report the software backend, and tag the pictures limited range (krdp-client ignores the tag
// and assumes limited).
//
// Frames come from a pw_stream video source inside this test (BGRx 320x240,
// memfd buffers, driven at 30 fps by a timer), linked by hand to KPipeWire's
// input stream on a private PipeWire daemon (private runtime dir and config,
// no session manager). A marker metadata object that only the private config
// creates proves the test never talks to the desktop's PipeWire. Exits 77
// (skip) when that daemon can't be started or no software H.264 encoder
// exists.

#include "EncoderSelection.h"
#include "SurfaceChain.h"

#include <PipeWireEncodedStream>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <atomic>
#include <cstdio>
#include <functional>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <unistd.h>

using KRdp::VideoCodec;
namespace EncoderSelection = KRdp::EncoderSelection;

namespace
{
constexpr int Width = 320;
constexpr int Height = 240;
constexpr int Stride = Width * 4;
constexpr int Sessions = 10;
constexpr int WaitMs = 10000;
constexpr int SettleMs = 3000;
const char *const PrivateGraphMarker = "krdp-swenc-test-marker";
const char *const SourceNodeName = "krdp-swenc-test-source";

// readdir() rather than QDir: QDir filters the dangling anon_inode:[...]
// links that eventfd/epoll/pidfd descriptors show up as.
int entryCount(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    int count = 0;
    while (const dirent *entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    closedir(dir);
    return count;
}
int fdCount()
{
    return entryCount("/proc/self/fd") - 1; // minus the directory stream itself
}
int threadCount()
{
    return entryCount("/proc/self/task");
}

// No policy, no devices: just what a client-created video stream and a
// client-created link need, plus the marker.
const char *const DaemonConfig = R"(
context.properties = {
    core.daemon = true
    core.name = pipewire-0
    support.dbus = false
}
context.spa-libs = {
    video.convert.* = videoconvert/libspa-videoconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-access }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
]
context.objects = [
    { factory = metadata args = { metadata.name = krdp-swenc-test-marker } }
]
)";

struct Graph {
    std::unique_ptr<QTemporaryDir> runtime;
    QProcess daemon;
    QString skipReason;

    bool start()
    {
        const QString pipewire = QStringLiteral(KRDP_PIPEWIRE_EXECUTABLE);
        if (QStandardPaths::findExecutable(pipewire).isEmpty() && !QFile::exists(pipewire)) {
            skipReason = QStringLiteral("no pipewire executable");
            return false;
        }
        runtime = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/krdp-swenc-XXXXXX"));
        if (!runtime->isValid()) {
            skipReason = QStringLiteral("cannot create a private runtime directory");
            return false;
        }
        QFile config(runtime->filePath(QStringLiteral("swenc-pipewire.conf")));
        if (!config.open(QIODevice::WriteOnly) || config.write(DaemonConfig) < 0) {
            skipReason = QStringLiteral("cannot write the private PipeWire config");
            return false;
        }
        config.close();
        // Everything in this process (KPipeWire's PipeWireCore, the source
        // below) resolves PipeWire here only; Qt and Wayland stay away from
        // the desktop too.
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime->path().toUtf8());
        qputenv("XDG_RUNTIME_DIR", runtime->path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "pipewire-0");
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PIPEWIRE_CONFIG_DIR"), runtime->path());
        env.insert(QStringLiteral("PIPEWIRE_CONFIG_NAME"), QStringLiteral("swenc-pipewire.conf"));
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), runtime->filePath(QStringLiteral("config")));
        env.remove(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"));
        daemon.setProcessEnvironment(env);
        daemon.setStandardOutputFile(runtime->filePath(QStringLiteral("daemon.log")));
        daemon.setStandardErrorFile(runtime->filePath(QStringLiteral("daemon.log")), QIODevice::Append);
        daemon.start(pipewire, QStringList{});
        if (!daemon.waitForStarted(3000)) {
            skipReason = QStringLiteral("the private PipeWire daemon did not start");
            return false;
        }
        const QString socket = runtime->filePath(QStringLiteral("pipewire-0"));
        QElapsedTimer timer;
        timer.start();
        while (!QFileInfo::exists(socket) && timer.elapsed() < 5000 && daemon.state() == QProcess::Running) {
            QThread::msleep(20);
        }
        if (!QFileInfo::exists(socket)) {
            skipReason = QStringLiteral("the private PipeWire daemon did not create its socket");
            return false;
        }
        return true;
    }
    void stop()
    {
        if (daemon.state() != QProcess::NotRunning) {
            daemon.terminate();
            if (!daemon.waitForFinished(3000)) {
                daemon.kill();
                daemon.waitForFinished(1000);
            }
        }
    }
};

// A 320x240 BGRx video source on its own pw_thread_loop. It drives the graph
// itself (30 fps timer) and links its output port to every video input
// stream that appears, which stands in for the session manager.
class VideoSource
{
public:
    ~VideoSource()
    {
        stop();
    }

    bool start(QString *error)
    {
        m_loop = pw_thread_loop_new("krdp-swenc-src", nullptr);
        m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
        if (!m_context) {
            *error = QStringLiteral("pw_context_new failed");
            return false;
        }
        if (pw_thread_loop_start(m_loop) < 0) {
            *error = QStringLiteral("pw_thread_loop_start failed");
            return false;
        }
        pw_thread_loop_lock(m_loop);
        m_core = pw_context_connect(m_context, nullptr, 0);
        if (!m_core) {
            pw_thread_loop_unlock(m_loop);
            *error = QStringLiteral("cannot connect to the private PipeWire daemon");
            return false;
        }
        m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
        static const pw_registry_events registryEvents = {
            .version = PW_VERSION_REGISTRY_EVENTS,
            .global = &VideoSource::onGlobal,
            .global_remove = &VideoSource::onGlobalRemove,
        };
        pw_registry_add_listener(m_registry, &m_registryListener, &registryEvents, this);

        m_stream = pw_stream_new(m_core,
                                 SourceNodeName,
                                 pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Source", PW_KEY_MEDIA_CLASS, "Video/Source",
                                                   PW_KEY_NODE_NAME, SourceNodeName, nullptr));
        static const pw_stream_events streamEvents = {
            .version = PW_VERSION_STREAM_EVENTS,
            .state_changed = &VideoSource::onStateChanged,
            .param_changed = &VideoSource::onParamChanged,
            .process = &VideoSource::onProcess,
        };
        pw_stream_add_listener(m_stream, &m_streamListener, &streamEvents, this);

        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_rectangle size = SPA_RECTANGLE(Width, Height);
        const spa_fraction variable = SPA_FRACTION(0, 1);
        const spa_fraction maxRate = SPA_FRACTION(30, 1);
        const spa_fraction minRate = SPA_FRACTION(1, 1);
        const spa_pod *params[1];
        params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b,
                                                                            SPA_TYPE_OBJECT_Format,
                                                                            SPA_PARAM_EnumFormat,
                                                                            SPA_FORMAT_mediaType,
                                                                            SPA_POD_Id(SPA_MEDIA_TYPE_video),
                                                                            SPA_FORMAT_mediaSubtype,
                                                                            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                                                                            SPA_FORMAT_VIDEO_format,
                                                                            SPA_POD_CHOICE_ENUM_Id(4,
                                                                                                   SPA_VIDEO_FORMAT_BGRx,
                                                                                                   SPA_VIDEO_FORMAT_BGRx,
                                                                                                   SPA_VIDEO_FORMAT_BGRA,
                                                                                                   SPA_VIDEO_FORMAT_RGBx),
                                                                            SPA_FORMAT_VIDEO_size,
                                                                            SPA_POD_Rectangle(&size),
                                                                            SPA_FORMAT_VIDEO_framerate,
                                                                            SPA_POD_Fraction(&variable),
                                                                            SPA_FORMAT_VIDEO_maxFramerate,
                                                                            SPA_POD_CHOICE_RANGE_Fraction(&maxRate, &minRate, &maxRate)));
        const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS);
        if (pw_stream_connect(m_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
            pw_thread_loop_unlock(m_loop);
            *error = QStringLiteral("pw_stream_connect failed for the source");
            return false;
        }
        m_timer = pw_loop_add_timer(pw_thread_loop_get_loop(m_loop), &VideoSource::onTimer, this);
        timespec interval{0, 1000000000L / 30};
        pw_loop_update_timer(pw_thread_loop_get_loop(m_loop), m_timer, &interval, &interval, false);
        pw_thread_loop_unlock(m_loop);
        return true;
    }

    void stop()
    {
        if (!m_loop) {
            return;
        }
        pw_thread_loop_lock(m_loop);
        if (m_timer) {
            pw_loop_destroy_source(pw_thread_loop_get_loop(m_loop), m_timer);
        }
        for (auto &link : m_links) {
            spa_hook_remove(&link->listener);
            pw_proxy_destroy(link->proxy);
        }
        m_links.clear();
        if (m_stream) {
            pw_stream_destroy(m_stream);
        }
        if (m_registry) {
            spa_hook_remove(&m_registryListener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy *>(m_registry));
        }
        if (m_core) {
            pw_core_disconnect(m_core);
        }
        pw_thread_loop_unlock(m_loop);
        pw_thread_loop_stop(m_loop);
        if (m_context) {
            pw_context_destroy(m_context);
        }
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
    }

    uint32_t nodeId()
    {
        pw_thread_loop_lock(m_loop);
        const uint32_t id = pw_stream_get_node_id(m_stream);
        pw_thread_loop_unlock(m_loop);
        return id;
    }
    bool sawMarker() const
    {
        return m_sawMarker;
    }
    int framesProduced() const
    {
        return m_frames;
    }
    int linksMade() const
    {
        return m_linksMade;
    }

private:
    struct Port {
        uint32_t node;
        bool output;
    };
    struct Link {
        VideoSource *self;
        pw_proxy *proxy;
        spa_hook listener;
    };

    static void onGlobal(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props)
    {
        auto *self = static_cast<VideoSource *>(data);
        if (!props) {
            return;
        }
        if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
            const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
            if (name && std::strcmp(name, PrivateGraphMarker) == 0) {
                self->m_sawMarker = true;
            }
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
            if (mediaClass && std::strcmp(mediaClass, "Stream/Input/Video") == 0) {
                self->m_sinkNodes.insert(id);
                self->tryLink();
            }
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
            const char *node = spa_dict_lookup(props, PW_KEY_NODE_ID);
            const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
            if (node && direction) {
                self->m_ports[id] = Port{uint32_t(std::strtoul(node, nullptr, 10)), std::strcmp(direction, "out") == 0};
                self->tryLink();
            }
        }
    }
    static void onGlobalRemove(void *data, uint32_t id)
    {
        auto *self = static_cast<VideoSource *>(data);
        self->m_sinkNodes.erase(id);
        self->m_ports.erase(id);
        self->m_linkedInputs.erase(id);
    }

    // Registry events run on the loop thread with the loop lock held.
    void tryLink()
    {
        const uint32_t source = pw_stream_get_node_id(m_stream);
        if (source == SPA_ID_INVALID) {
            return;
        }
        uint32_t outPort = SPA_ID_INVALID;
        for (const auto &[id, port] : m_ports) {
            if (port.node == source && port.output) {
                outPort = id;
                break;
            }
        }
        if (outPort == SPA_ID_INVALID) {
            return;
        }
        for (const auto &[id, port] : m_ports) {
            if (port.output || !m_sinkNodes.contains(port.node) || m_linkedInputs.contains(id)) {
                continue;
            }
            const QByteArray outNode = QByteArray::number(source);
            const QByteArray outPortId = QByteArray::number(outPort);
            const QByteArray inNode = QByteArray::number(port.node);
            const QByteArray inPortId = QByteArray::number(id);
            const spa_dict_item items[] = {
                SPA_DICT_ITEM_INIT(PW_KEY_LINK_OUTPUT_NODE, outNode.constData()),
                SPA_DICT_ITEM_INIT(PW_KEY_LINK_OUTPUT_PORT, outPortId.constData()),
                SPA_DICT_ITEM_INIT(PW_KEY_LINK_INPUT_NODE, inNode.constData()),
                SPA_DICT_ITEM_INIT(PW_KEY_LINK_INPUT_PORT, inPortId.constData()),
                SPA_DICT_ITEM_INIT(PW_KEY_OBJECT_LINGER, "false"),
            };
            const spa_dict dict = SPA_DICT_INIT_ARRAY(items);
            auto *proxy = static_cast<pw_proxy *>(pw_core_create_object(m_core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &dict, 0));
            if (!proxy) {
                continue;
            }
            auto link = std::make_unique<Link>(Link{this, proxy, {}});
            static const pw_proxy_events proxyEvents = {
                .version = PW_VERSION_PROXY_EVENTS,
                .removed = &VideoSource::onLinkRemoved,
            };
            pw_proxy_add_listener(proxy, &link->listener, &proxyEvents, link.get());
            m_links.push_back(std::move(link));
            m_linkedInputs.insert(id);
            ++m_linksMade;
        }
    }
    // The link went away with KPipeWire's stream: free our proxy for it.
    static void onLinkRemoved(void *data)
    {
        auto *link = static_cast<Link *>(data);
        VideoSource *self = link->self;
        spa_hook_remove(&link->listener);
        pw_proxy_destroy(link->proxy);
        std::erase_if(self->m_links, [link](const std::unique_ptr<Link> &l) {
            return l.get() == link;
        });
    }

    static void onStateChanged(void *data, pw_stream_state, pw_stream_state state, const char *)
    {
        auto *self = static_cast<VideoSource *>(data);
        if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
            self->tryLink();
        }
    }

    static void onParamChanged(void *data, uint32_t id, const spa_pod *param)
    {
        auto *self = static_cast<VideoSource *>(data);
        if (!param || id != SPA_PARAM_Format) {
            return;
        }
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) {
            return;
        }
        self->m_format = info.format;
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod *params[3];
        params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b,
                                                                            SPA_TYPE_OBJECT_ParamBuffers,
                                                                            SPA_PARAM_Buffers,
                                                                            SPA_PARAM_BUFFERS_buffers,
                                                                            SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
                                                                            SPA_PARAM_BUFFERS_blocks,
                                                                            SPA_POD_Int(1),
                                                                            SPA_PARAM_BUFFERS_size,
                                                                            SPA_POD_Int(Stride * Height),
                                                                            SPA_PARAM_BUFFERS_stride,
                                                                            SPA_POD_Int(Stride),
                                                                            SPA_PARAM_BUFFERS_dataType,
                                                                            SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)));
        params[1] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b,
                                                                            SPA_TYPE_OBJECT_ParamMeta,
                                                                            SPA_PARAM_Meta,
                                                                            SPA_PARAM_META_type,
                                                                            SPA_POD_Id(SPA_META_Header),
                                                                            SPA_PARAM_META_size,
                                                                            SPA_POD_Int(sizeof(spa_meta_header))));
        params[2] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b,
                                                                            SPA_TYPE_OBJECT_ParamMeta,
                                                                            SPA_PARAM_Meta,
                                                                            SPA_PARAM_META_type,
                                                                            SPA_POD_Id(SPA_META_VideoDamage),
                                                                            SPA_PARAM_META_size,
                                                                            SPA_POD_CHOICE_RANGE_Int(sizeof(spa_meta_region) * 16,
                                                                                                     sizeof(spa_meta_region) * 1,
                                                                                                     sizeof(spa_meta_region) * 16)));
        pw_stream_update_params(self->m_stream, params, 3);
    }

    static void onTimer(void *data, uint64_t)
    {
        auto *self = static_cast<VideoSource *>(data);
        if (pw_stream_get_state(self->m_stream, nullptr) == PW_STREAM_STATE_STREAMING) {
            pw_stream_trigger_process(self->m_stream);
        }
    }

    static void onProcess(void *data)
    {
        auto *self = static_cast<VideoSource *>(data);
        pw_buffer *buffer = pw_stream_dequeue_buffer(self->m_stream);
        if (!buffer) {
            return;
        }
        spa_buffer *spa = buffer->buffer;
        spa_data &plane = spa->datas[0];
        if (!plane.data || plane.maxsize < uint32_t(Stride * Height)) {
            pw_stream_queue_buffer(self->m_stream, buffer);
            return;
        }
        const int frame = self->m_frames++;
        auto *pixels = static_cast<uint8_t *>(plane.data);
        // A diagonal gradient that scrolls plus a moving white bar: every
        // frame differs from the last, so the encoder always has work.
        for (int y = 0; y < Height; ++y) {
            auto *row = reinterpret_cast<uint32_t *>(pixels + y * Stride);
            for (int x = 0; x < Width; ++x) {
                const uint8_t r = uint8_t(x + frame * 3);
                const uint8_t g = uint8_t(y + frame * 2);
                const uint8_t b = uint8_t((x ^ y) + frame);
                const bool bar = ((x + frame * 4) % Width) < 16;
                row[x] = bar ? 0xffffffffu : (0xffu << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
            }
        }
        plane.chunk->offset = 0;
        plane.chunk->size = Stride * Height;
        plane.chunk->stride = Stride;
        plane.chunk->flags = SPA_CHUNK_FLAG_NONE;
        if (auto *header = static_cast<spa_meta_header *>(spa_buffer_find_meta_data(spa, SPA_META_Header, sizeof(spa_meta_header)))) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            header->flags = 0;
            header->offset = 0;
            header->pts = SPA_TIMESPEC_TO_NSEC(&now);
            header->dts_offset = 0;
            header->seq = frame;
        }
        if (spa_meta *damage = spa_buffer_find_meta(spa, SPA_META_VideoDamage)) {
            auto *regions = static_cast<spa_meta_region *>(damage->data);
            const size_t count = damage->size / sizeof(spa_meta_region);
            if (count > 0) {
                regions[0].region = SPA_REGION(0, 0, Width, Height);
            }
            if (count > 1) {
                regions[1].region = SPA_REGION(0, 0, 0, 0); // terminator
            }
        }
        pw_stream_queue_buffer(self->m_stream, buffer);
    }

    pw_thread_loop *m_loop = nullptr;
    pw_context *m_context = nullptr;
    pw_core *m_core = nullptr;
    pw_registry *m_registry = nullptr;
    pw_stream *m_stream = nullptr;
    spa_source *m_timer = nullptr;
    spa_hook m_registryListener{};
    spa_hook m_streamListener{};
    std::map<uint32_t, Port> m_ports;
    std::set<uint32_t> m_sinkNodes;
    std::set<uint32_t> m_linkedInputs;
    std::vector<std::unique_ptr<Link>> m_links;
    uint32_t m_format = SPA_VIDEO_FORMAT_UNKNOWN;
    std::atomic<bool> m_sawMarker = false;
    std::atomic<int> m_frames = 0;
    std::atomic<int> m_linksMade = 0;
};

// NAL unit types in an Annex-B byte stream; empty when it does not start with
// a start code.
QList<int> annexBNalTypes(const QByteArray &data)
{
    QList<int> types;
    const auto *bytes = reinterpret_cast<const uint8_t *>(data.constData());
    const qsizetype size = data.size();
    const bool startsWithCode = (size >= 4 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1)
        || (size >= 3 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 1);
    if (!startsWithCode) {
        return types;
    }
    for (qsizetype i = 0; i + 3 < size; ++i) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
            types.append(bytes[i + 3] & 0x1f);
            i += 2;
        }
    }
    return types;
}

Graph *g_graph = nullptr;
}

class SoftwareEncodeSessionTest : public QObject
{
    Q_OBJECT

    VideoSource m_source;
    uint32_t m_sourceNode = SPA_ID_INVALID;

    struct SessionResult {
        int packets = 0;
        int keyFrames = 0;
        QByteArray firstPacket;
        QList<QByteArray> data; ///< the first packets, for decoding
        QList<QByteArray> keyFrameData; ///< the first keyframes (AUD-FIX11: headers in band)
        QList<PipeWireBaseEncodedStream::EncoderBackend> backends; ///< activeEncoderBackendChanged
        QString error;
    };

    // One KRdp-style encode session: create, start, wait for packets, stop,
    // wait for the produce thread to finish, destroy. \a configure picks the
    // encoder (default: H264Main, the stock-client path).
    SessionResult runSession(int wantPackets, const std::function<void(PipeWireEncodedStream *)> &configure = {})
    {
        SessionResult result;
        auto *stream = new PipeWireEncodedStream;
        QPointer<PipeWireEncodedStream> guard(stream);
        connect(stream, &PipeWireEncodedStream::newPacket, stream, [&result](const PipeWireEncodedStream::Packet &packet) {
            if (result.packets == 0) {
                result.firstPacket = packet.data();
            }
            if (result.data.size() < 16) {
                result.data.append(packet.data());
            }
            ++result.packets;
            if (packet.isKeyFrame()) {
                ++result.keyFrames;
                if (result.keyFrameData.size() < 4) result.keyFrameData.append(packet.data());
            }
        });
        connect(stream, &PipeWireBaseEncodedStream::errorFound, stream, [&result](const QString &error) {
            result.error = error;
        });
        connect(stream, &PipeWireBaseEncodedStream::activeEncoderBackendChanged, stream, [&result](PipeWireBaseEncodedStream::EncoderBackend backend) {
            result.backends.append(backend);
        });
        stream->setNodeId(m_sourceNode);
        if (configure) {
            configure(stream);
        } else {
            stream->setEncoder(PipeWireBaseEncodedStream::H264Main);
        }
        stream->setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed); // as KRdp's sessions do
        stream->setMaxFramerate(30, 1);
        stream->start();

        QElapsedTimer timer;
        timer.start();
        while ((result.packets < wantPackets || result.keyFrames < 1) && result.error.isEmpty() && timer.elapsed() < WaitMs) {
            QTest::qWait(10);
        }

        stream->stop();
        timer.restart();
        while (stream->state() != PipeWireBaseEncodedStream::Idle && timer.elapsed() < WaitMs) {
            QTest::qWait(10);
        }
        if (stream->state() != PipeWireBaseEncodedStream::Idle && result.error.isEmpty()) {
            result.error = QStringLiteral("stream did not return to Idle after stop()");
        }
        stream->deleteLater();
        timer.restart();
        while (guard && timer.elapsed() < WaitMs) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QTest::qWait(10);
        }
        if (guard && result.error.isEmpty()) {
            result.error = QStringLiteral("stream object was not destroyed");
        }
        return result;
    }

    void verifyH264(const SessionResult &result)
    {
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY2(result.packets >= 1,
                 qPrintable(QStringLiteral("no encoded packet within %1 ms (%2 source frames, %3 links)")
                                .arg(WaitMs)
                                .arg(m_source.framesProduced())
                                .arg(m_source.linksMade())));
        QVERIFY2(result.keyFrames >= 1, "no key frame arrived");
        const QList<int> nals = annexBNalTypes(result.firstPacket);
        QVERIFY2(!nals.isEmpty(), qPrintable(QStringLiteral("first packet is not Annex-B: %1").arg(QString::fromLatin1(result.firstPacket.left(8).toHex(' ')))));
        QStringList types;
        for (int type : nals) {
            types << QString::number(type);
        }
        QVERIFY2(nals.contains(7), qPrintable(QStringLiteral("no SPS in the first packet; NAL types %1").arg(types.join(QLatin1Char(',')))));
        // AUD-FIX11: every keyframe carries its SPS/PPS in band (a client never gets extradata).
        for (const QByteArray &keyFrame : result.keyFrameData) QVERIFY(KRdp::keyframeCarriesHeaders(VideoCodec::Avc420, keyFrame));
        qInfo("first packet: %lld bytes, NAL types %s; %d packets, %d key frames", static_cast<long long>(result.firstPacket.size()),
              qPrintable(types.join(QLatin1Char(','))), result.packets, result.keyFrames);
    }

    // A software HEVC/AV1 session configured the way KRdp's session does it for the software
    // backend; the packets must decode to limited-range 4:2:0 pictures of the source's size.
    void verifyPrivateCodecInSoftware(VideoCodec codec)
    {
        const bool hevc = codec == VideoCodec::Hevc;
        const auto encoder = hevc ? PipeWireBaseEncodedStream::HEVCMain : PipeWireBaseEncodedStream::AV1Main;
        if (!(PipeWireBaseEncodedStream::availableEncoderBackends(encoder) & PipeWireBaseEncodedStream::EncoderBackend::Software)) {
            QSKIP(hevc ? "no libx265 in libavcodec" : "no libsvtav1/libaom-av1 in libavcodec");
        }
        // Hardware is off for the whole process (LIBVA_DRIVER_NAME), as with KRDP_FORCE_SOFTWARE_ENCODING.
        QVERIFY(!(PipeWireBaseEncodedStream::availableEncoderBackends(encoder) & PipeWireBaseEncodedStream::EncoderBackend::Hardware));
        bool matched = false;
        const SessionResult result = runSession(5, [&](PipeWireEncodedStream *stream) {
            matched = EncoderSelection::apply(stream, codec, false);
            QCOMPARE(stream->encoderBackendPolicy(), PipeWireBaseEncodedStream::EncoderBackendPolicy::SoftwareOnly);
        });
        QVERIFY2(matched, "EncoderSelection::apply() did not get the private codec's encoder");
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY2(result.packets >= 5, qPrintable(QStringLiteral("%1 packets within %2 ms").arg(result.packets).arg(WaitMs)));
        QVERIFY(result.keyFrames >= 1);
        // AUD-FIX11: every keyframe carries its VPS/SPS/PPS or sequence header in band.
        for (const QByteArray &keyFrame : result.keyFrameData) QVERIFY(KRdp::keyframeCarriesHeaders(codec, keyFrame));
        QVERIFY2(result.backends.contains(PipeWireBaseEncodedStream::EncoderBackend::Software),
                 qPrintable(QStringLiteral("backend reports: %1").arg(result.backends.size())));
        QVERIFY(!result.backends.contains(PipeWireBaseEncodedStream::EncoderBackend::Hardware));

        const AVCodec *decoder = hevc ? avcodec_find_decoder(AV_CODEC_ID_HEVC) : avcodec_find_decoder_by_name("libdav1d");
        if (!decoder && !hevc) {
            decoder = avcodec_find_decoder_by_name("libaom-av1");
        }
        if (!decoder) {
            QSKIP("no software decoder to check the stream with");
        }
        AVCodecContext *context = avcodec_alloc_context3(decoder);
        QVERIFY(context && avcodec_open2(context, decoder, nullptr) >= 0);
        AVFrame *frame = av_frame_alloc();
        AVPacket *packet = av_packet_alloc();
        int decoded = 0;
        AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
        QSize size;
        for (const QByteArray &data : result.data) {
            av_packet_unref(packet);
            QVERIFY(av_new_packet(packet, int(data.size())) >= 0);
            std::memcpy(packet->data, data.constData(), size_t(data.size()));
            if (avcodec_send_packet(context, packet) < 0) {
                continue;
            }
            while (avcodec_receive_frame(context, frame) >= 0) {
                if (decoded++ == 0) {
                    range = frame->color_range;
                    size = QSize(frame->width, frame->height);
                }
                av_frame_unref(frame);
            }
        }
        avcodec_send_packet(context, nullptr);
        while (avcodec_receive_frame(context, frame) >= 0) {
            ++decoded;
            av_frame_unref(frame);
        }
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
        qInfo("%s: %d packets, %d key frames, %d decoded, first picture %dx%d range %s", hevc ? "HEVC" : "AV1", result.packets, result.keyFrames, decoded,
              size.width(), size.height(), av_color_range_name(range));
        QVERIFY2(decoded >= 1, "the software stream does not decode");
        QVERIFY(size.width() >= Width && size.height() >= Height);
        QCOMPARE(range, AVCOL_RANGE_MPEG); // limited
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(g_graph);
        QString error;
        QVERIFY2(m_source.start(&error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(m_source.nodeId() != SPA_ID_INVALID, WaitMs);
        m_sourceNode = m_source.nodeId();
        QTRY_VERIFY_WITH_TIMEOUT(m_source.sawMarker(), WaitMs);
        QVERIFY2(m_source.sawMarker(), "the private graph marker is missing; refusing to go on");
        qInfo("private graph ready, source node %u", m_sourceNode);
    }

    void cleanupTestCase()
    {
        m_source.stop();
    }

    void cleanup()
    {
        qunsetenv("KPIPEWIRE_FORCE_ENCODER");
    }

    // F1: VA-API is broken for the whole process; H.264 still arrives.
    void h264WithoutHardware()
    {
        QCOMPARE(qgetenv("LIBVA_DRIVER_NAME"), QByteArray("krdp-test-none"));
        verifyH264(runSession(3));
    }

    // F1: the explicit "software only" knob picks libx264 and it works.
    void h264ForcedLibx264()
    {
        if (!avcodec_find_encoder_by_name("libx264")) {
            QSKIP("libavcodec has no libx264");
        }
        qputenv("KPIPEWIRE_FORCE_ENCODER", "libx264");
        verifyH264(runSession(3));
    }

    // WS-E: HEVC and AV1 with hardware off, software backend, as the codec policy asks for them.
    void hevcInSoftware()
    {
        verifyPrivateCodecInSoftware(VideoCodec::Hevc);
    }
    void av1InSoftware()
    {
        verifyPrivateCodecInSoftware(VideoCodec::Av1);
    }

    // F5: sequential sessions leave no fd or thread behind.
    void sessionsDoNotLeak()
    {
        const SessionResult warmUp = runSession(1);
        QVERIFY2(warmUp.error.isEmpty() && warmUp.packets >= 1, qPrintable(QStringLiteral("warm-up session failed: %1").arg(warmUp.error)));
        QTest::qWait(1000);
        const int baseFds = fdCount();
        const int baseThreads = threadCount();

        QElapsedTimer total;
        total.start();
        for (int i = 0; i < Sessions; ++i) {
            const SessionResult result = runSession(1);
            QVERIFY2(result.error.isEmpty() && result.packets >= 1,
                     qPrintable(QStringLiteral("session %1: %2 packets, error '%3'").arg(i).arg(result.packets).arg(result.error)));
        }
        const qint64 sessionsMs = total.elapsed();

        QElapsedTimer settle;
        settle.start();
        int fds = fdCount();
        int threads = threadCount();
        while ((fds > baseFds || threads > baseThreads) && settle.elapsed() < SettleMs) {
            QTest::qWait(50);
            fds = fdCount();
            threads = threadCount();
        }
        qInfo("%d sessions in %lld ms; fds %d -> %d, threads %d -> %d (settled after %lld ms)", Sessions, static_cast<long long>(sessionsMs), baseFds, fds,
              baseThreads, threads, static_cast<long long>(settle.elapsed()));
        QVERIFY2(fds <= baseFds, qPrintable(QStringLiteral("fd leak: %1 -> %2 after %3 sessions").arg(baseFds).arg(fds).arg(Sessions)));
        QVERIFY2(threads <= baseThreads, qPrintable(QStringLiteral("thread leak: %1 -> %2 after %3 sessions").arg(baseThreads).arg(threads).arg(Sessions)));
    }
};

int main(int argc, char **argv)
{
    // Before anything can load libva: no VA-API driver will initialise.
    qputenv("LIBVA_DRIVER_NAME", "krdp-test-none");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("WAYLAND_DISPLAY");
    qunsetenv("DISPLAY");
    qunsetenv("KPIPEWIRE_FORCE_ENCODER");
    qunsetenv("KPIPEWIRE_CHROMA_MODE");

    Graph graph;
    if (!graph.start()) {
        std::fprintf(stderr, "SKIP: %s\n", qPrintable(graph.skipReason));
        graph.stop();
        return 77;
    }
    if (!avcodec_find_encoder_by_name("libx264") && !avcodec_find_encoder_by_name("libopenh264")) {
        std::fprintf(stderr, "SKIP: libavcodec has neither libx264 nor libopenh264\n");
        graph.stop();
        return 77;
    }
    g_graph = &graph;

    int status = 0;
    {
        QGuiApplication app(argc, argv);
        SoftwareEncodeSessionTest test;
        status = QTest::qExec(&test, argc, argv);
    }
    graph.stop();
    return status;
}

#include "SoftwareEncodeSessionTest.moc"
