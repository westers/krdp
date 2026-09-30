// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "PipeWireRuntime.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>
#include <spa/utils/hook.h>

#include <atomic>

struct pw_stream;
struct pw_thread_loop;
struct pw_registry;

namespace KRdp
{
class PipeWireCamera
{
public:
    ~PipeWireCamera();
    bool start(const QString &id, uint32_t width, uint32_t height, uint32_t fps, const QString &loopbackDevice = {});
    void stop();
    // RDPECAM's V4L backend sends MJPEG samples. Decode them outside PipeWire's
    // RT callback and retain only the newest frame to keep conferencing latency bounded.
    void writeMjpeg(const QByteArray &jpeg);
    // A PipeWire process callback means a local application has linked this
    // virtual source. The RDP session thread consumes this flag before asking
    // the client to open its physical camera.
    bool captureRequested() const;
    /** The node exists in the graph (PAUSED or STREAMING). Any thread. */
    bool ready() const { return m_ready.load(); }
    /** True while a PipeWire input is linked or a V4L2 reader holds the loopback. */
    bool consumerActive() const;
private:
    static void process(void *data);
    void process();
    static void registryGlobal(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version, const spa_dict *props);
    static void registryGlobalRemove(void *data, uint32_t id);
    mutable QMutex m_mutex;
    QByteArray m_pending;
    QHash<uint32_t, uint32_t> m_outputLinks; // link id -> output node id
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    int m_loopbackFd = -1;
    QString m_loopbackDevice;
    std::atomic_bool m_captureRequested = false;
    std::atomic_bool m_ready = false;
    std::atomic_bool m_streaming = false;
    std::atomic<uint32_t> m_nodeId = UINT32_MAX;
    pw_thread_loop *m_loop = nullptr;
    pw_stream *m_stream = nullptr;
    pw_registry *m_registry = nullptr;
    spa_hook m_registryListener{};
    PipeWireRuntime::Reference m_runtime;
};
}
