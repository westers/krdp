// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QMutex>
#include <QString>

struct pw_stream;
struct pw_thread_loop;

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
private:
    static void process(void *data);
    void process();
    QMutex m_mutex;
    QByteArray m_pending;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    int m_loopbackFd = -1;
    pw_thread_loop *m_loop = nullptr;
    pw_stream *m_stream = nullptr;
};
}
