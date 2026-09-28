// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX2 F5: every software-encoded video session owns a KPipeWire
// DmaBufHandler (the encoder downloads DMA-BUF frames through EGL). Before the
// fix the handler never destroyed its EGL context, left it current on the
// encoder thread and never released its GBM device and EGL display, so each
// session left file descriptors behind (on Sol's NVIDIA driver one sync_file
// per session; on Mesa the display's duplicated render-node fd).
//
// Here: import a real GBM buffer on the first render node, download it on a
// worker thread (as the encoder does), destroy the handler on the main thread,
// many times, and check the fd count returns to its baseline. Runs on
// KPipeWire's headless GBM path (KPIPEWIRE_DMABUF_RENDER_NODE; Qt is offscreen
// and XDG_RUNTIME_DIR an empty directory) so it never talks to the desktop.
// Skips when there is no usable render node.

#include <DmaBufHandler>
#include <PipeWireSourceStream>

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <gbm.h>
#include <thread>
#include <unistd.h>

namespace
{
int fdCount()
{
    DIR *dir = opendir("/proc/self/fd");
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
    return count - 1; // the directory stream itself
}

struct Buffer {
    int drmFd = -1;
    gbm_device *device = nullptr;
    gbm_bo *bo = nullptr;
    int dmabufFd = -1;
    ~Buffer()
    {
        if (dmabufFd >= 0) {
            close(dmabufFd);
        }
        if (bo) {
            gbm_bo_destroy(bo);
        }
        if (device) {
            gbm_device_destroy(device);
        }
        if (drmFd >= 0) {
            close(drmFd);
        }
    }
};
}

class DmaBufHandlerLeakTest : public QObject
{
    Q_OBJECT

    static bool download(const Buffer &buffer)
    {
        PipeWireFrame frame;
        frame.format = SPA_VIDEO_FORMAT_BGRx;
        DmaBufAttributes attributes;
        attributes.width = 64;
        attributes.height = 64;
        attributes.format = GBM_FORMAT_XRGB8888;
        attributes.modifier = gbm_bo_get_modifier(buffer.bo);
        attributes.planes.append({buffer.dmabufFd, 0, gbm_bo_get_stride(buffer.bo)});
        frame.dmabuf = attributes;

        auto handler = std::make_unique<DmaBufHandler>();
        bool ok = false;
        // The encoder thread downloads; the handler dies with the encoder, later,
        // on another thread.
        std::thread worker([&] {
            QImage image(64, 64, QImage::Format_RGB32);
            ok = handler->downloadFrame(image, frame);
        });
        worker.join();
        handler.reset();
        return ok;
    }

private Q_SLOTS:
    void handlersReleaseEverything()
    {
        Buffer buffer;
        const auto nodes = QDir(QStringLiteral("/dev/dri")).entryList({QStringLiteral("renderD*")}, QDir::System, QDir::Name);
        if (nodes.isEmpty()) {
            QSKIP("no DRM render node");
        }
        const QByteArray node = QByteArrayLiteral("/dev/dri/") + nodes.first().toLatin1();
        buffer.drmFd = open(node.constData(), O_RDWR | O_CLOEXEC);
        if (buffer.drmFd < 0) {
            QSKIP("cannot open the first render node");
        }
        buffer.device = gbm_create_device(buffer.drmFd);
        if (!buffer.device) {
            QSKIP("no GBM device");
        }
        buffer.bo = gbm_bo_create(buffer.device, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!buffer.bo) {
            QSKIP("cannot allocate a GBM buffer");
        }
        buffer.dmabufFd = gbm_bo_get_fd(buffer.bo);
        QVERIFY(buffer.dmabufFd >= 0);
        // The handler's own GBM display on the same node (KPipeWire's headless path).
        qputenv("KPIPEWIRE_DMABUF_RENDER_NODE", node);

        // Warm up: the EGL/driver libraries keep some process-wide state of their own.
        for (int i = 0; i < 3; ++i) {
            if (!download(buffer)) {
                QSKIP("EGL cannot import a DMA-BUF here");
            }
        }
        const int baseline = fdCount();
        constexpr int Sessions = 25;
        for (int i = 0; i < Sessions; ++i) {
            QVERIFY(download(buffer));
        }
        const int after = fdCount();
        qInfo() << "fds: baseline" << baseline << "after" << Sessions << "handlers" << after;
        QCOMPARE(after, baseline);
    }
};

int main(int argc, char **argv)
{
    // GBM path only: no compositor, and nothing reaches the desktop's Wayland socket.
    QTemporaryDir runtime;
    qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
    qunsetenv("WAYLAND_DISPLAY");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    DmaBufHandlerLeakTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "DmaBufHandlerLeakTest.moc"
