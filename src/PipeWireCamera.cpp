// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PipeWireCamera.h"

#include <QImage>
#include <QDir>
#include <QFile>
#include <QMutexLocker>
#include <QDebug>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/raw-utils.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace KRdp
{
namespace
{
bool hasExternalV4l2Consumer(const QString &device)
{
    struct stat wanted {};
    if (device.isEmpty() || stat(QFile::encodeName(device).constData(), &wanted) != 0) {
        return false;
    }
    const QDir proc(QStringLiteral("/proc"));
    const auto processes = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &process : processes) {
        bool isPid = false;
        const qlonglong pid = process.toLongLong(&isPid);
        if (!isPid || pid == getpid()) {
            continue;
        }
        const QDir fds(proc.filePath(process + QStringLiteral("/fd")));
        for (const QFileInfo &entry : fds.entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot)) {
            struct stat candidate {};
            if (stat(QFile::encodeName(entry.absoluteFilePath()).constData(), &candidate) == 0
                && S_ISCHR(candidate.st_mode) && candidate.st_rdev == wanted.st_rdev) {
                return true;
            }
        }
    }
    return false;
}
}

PipeWireCamera::~PipeWireCamera() { stop(); }

bool PipeWireCamera::start(const QString &id, uint32_t width, uint32_t height, uint32_t fps, const QString &loopbackDevice)
{
    QMutexLocker lock(&m_mutex);
    if (m_stream) return true;
    if (!width || !height) return false;
    pw_init(nullptr, nullptr);
    m_width = width;
    m_height = height;
    m_loop = pw_thread_loop_new("krdp-remote-camera", nullptr);
    if (!m_loop) return false;
    // Like PipeWireMicrophone, PipeWire retains the events pointer beyond this call.
    static const pw_stream_events events = [] {
        pw_stream_events result{};
        result.version = PW_VERSION_STREAM_EVENTS;
        result.process = PipeWireCamera::process;
        return result;
    }();
    const QByteArray nodeName = QByteArrayLiteral("krdp.remote-camera.") + id.toUtf8();
    m_stream = pw_stream_new_simple(pw_thread_loop_get_loop(m_loop), "KRDP Remote Camera",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_ROLE, "Camera", PW_KEY_NODE_NAME, nodeName.constData(), PW_KEY_NODE_DESCRIPTION, "KRDP Remote Camera", nullptr), &events, this);
    uint8_t storage[1024]; spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    spa_video_info_raw format{};
    format.format = SPA_VIDEO_FORMAT_RGBA;
    format.size.width = width;
    format.size.height = height;
    format.framerate.num = fps ? fps : 30;
    format.framerate.denom = 1;
    const spa_pod *params[] = {spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    if (!m_stream || pw_stream_connect(m_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0 || pw_thread_loop_start(m_loop) < 0) {
        if (m_stream) pw_stream_destroy(m_stream);
        m_stream = nullptr;
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
        return false;
    }
    if (!loopbackDevice.isEmpty()) {
        const auto path = QFile::encodeName(loopbackDevice);
        m_loopbackFd = open(path.constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (m_loopbackFd < 0) {
            qWarning() << "KRDP remote camera could not open V4L2 loopback" << loopbackDevice;
        } else {
            v4l2_format format{};
            format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            format.fmt.pix.width = m_width;
            format.fmt.pix.height = m_height;
            format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
            format.fmt.pix.field = V4L2_FIELD_NONE;
            format.fmt.pix.bytesperline = m_width * 2;
            format.fmt.pix.sizeimage = m_width * m_height * 2;
            if (ioctl(m_loopbackFd, VIDIOC_S_FMT, &format) < 0
                || format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV
                || format.fmt.pix.width != m_width || format.fmt.pix.height != m_height) {
                qWarning() << "KRDP remote camera could not configure V4L2 loopback" << loopbackDevice;
                close(m_loopbackFd);
                m_loopbackFd = -1;
            } else {
                // v4l2loopback with exclusive_caps only advertises CAPTURE
                // after it has received a frame. A black frame makes the
                // camera discoverable without opening the redirected camera.
                const QByteArray black(int(m_width * m_height * 2), char(16));
                if (write(m_loopbackFd, black.constData(), size_t(black.size())) < 0) {
                    qWarning() << "KRDP remote camera could not prime V4L2 loopback" << loopbackDevice;
                }
                m_loopbackDevice = loopbackDevice;
                qInfo() << "KRDP remote camera publishing V4L2 loopback" << loopbackDevice << m_width << 'x' << m_height;
            }
        }
    }
    return true;
}

void PipeWireCamera::stop()
{
    pw_stream *stream = nullptr;
    pw_thread_loop *loop = nullptr;
    int loopbackFd = -1;
    {
        QMutexLocker lock(&m_mutex);
        stream = m_stream;
        loop = m_loop;
        m_stream = nullptr;
        m_loop = nullptr;
        loopbackFd = m_loopbackFd;
        m_loopbackFd = -1;
        m_loopbackDevice.clear();
        m_pending.clear();
    }
    if (loop) pw_thread_loop_stop(loop);
    if (stream) { pw_stream_disconnect(stream); pw_stream_destroy(stream); }
    if (loop) pw_thread_loop_destroy(loop);
    if (loopbackFd >= 0) close(loopbackFd);
}

void PipeWireCamera::writeMjpeg(const QByteArray &jpeg)
{
    const QImage image = QImage::fromData(jpeg, "JPEG");
    if (image.isNull()) return;
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QMutexLocker lock(&m_mutex);
    if (!m_stream || uint32_t(rgba.width()) != m_width || uint32_t(rgba.height()) != m_height) return;
    m_pending = QByteArray(reinterpret_cast<const char *>(rgba.constBits()), rgba.sizeInBytes());
    if (m_loopbackFd >= 0) {
        QByteArray yuyv;
        yuyv.resize(int(m_width * m_height * 2));
        const auto *source = reinterpret_cast<const uchar *>(rgba.constBits());
        auto *target = reinterpret_cast<uchar *>(yuyv.data());
        const auto clamp = [](int value) { return uchar(qBound(0, value, 255)); };
        for (uint32_t y = 0; y < m_height; ++y) {
            const auto *row = source + y * rgba.bytesPerLine();
            for (uint32_t x = 0; x < m_width; x += 2) {
                const auto encode = [&clamp](const uchar *pixel, int &luma, int &cb, int &cr) {
                    const int r = pixel[0], g = pixel[1], b = pixel[2];
                    luma = clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                    cb = clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                    cr = clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                };
                int y0, cb0, cr0, y1, cb1, cr1;
                encode(row + x * 4, y0, cb0, cr0);
                encode(row + qMin(x + 1, m_width - 1) * 4, y1, cb1, cr1);
                *target++ = uchar(y0);
                *target++ = clamp((cb0 + cb1) / 2);
                *target++ = uchar(y1);
                *target++ = clamp((cr0 + cr1) / 2);
            }
        }
        const auto written = write(m_loopbackFd, yuyv.constData(), size_t(yuyv.size()));
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            qWarning() << "KRDP remote camera V4L2 loopback write failed; disabling it";
            close(m_loopbackFd);
            m_loopbackFd = -1;
        }
    }
}

bool PipeWireCamera::captureRequested() const
{
    return m_captureRequested.load(std::memory_order_acquire) || hasExternalV4l2Consumer(m_loopbackDevice);
}

void PipeWireCamera::process(void *data) { static_cast<PipeWireCamera *>(data)->process(); }
void PipeWireCamera::process()
{
    // This callback is emitted only once PipeWire schedules our output node.
    // It is deliberately just an atomic edge: RDPECAM and FreeRDP channel I/O
    // remain owned by RdpConnection's session thread.
    m_captureRequested.store(true, std::memory_order_release);
    QMutexLocker lock(&m_mutex);
    if (!m_stream) return;
    pw_buffer *buffer = pw_stream_dequeue_buffer(m_stream);
    if (!buffer || !buffer->buffer || !buffer->buffer->n_datas) return;
    spa_data &data = buffer->buffer->datas[0];
    if (!data.data) { pw_stream_queue_buffer(m_stream, buffer); return; }
    const uint32_t bytes = qMin<uint32_t>(data.maxsize - data.chunk->offset, m_width * m_height * 4);
    if (uint32_t(m_pending.size()) == m_width * m_height * 4) memcpy(static_cast<uint8_t *>(data.data) + data.chunk->offset, m_pending.constData(), bytes);
    else memset(static_cast<uint8_t *>(data.data) + data.chunk->offset, 0, bytes);
    data.chunk->size = bytes;
    data.chunk->stride = m_width * 4;
    pw_stream_queue_buffer(m_stream, buffer);
}
}
