// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// Test harness: capture the screen through the same pipeline krdpserver uses
// (PlasmaScreencastV1Session -> KPipeWire -> encoder) and dump raw H.264 to a
// file, without any RDP client involved.

#include <csignal>

#include <QCommandLineParser>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QUrl>

#include "PlasmaScreencastV1Session.h"
#include "VideoStream.h"

using namespace Qt::StringLiterals;

int main(int argc, char **argv)
{
    QGuiApplication application{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {u"quit-after"_s, u"Quit after running for this amount of seconds"_s, u"seconds"_s},
        {u"monitor"_s, u"Index of the monitor to display."_s, u"monitor"_s, u"-1"_s},
        {u"quality"_s, u"Encoding quality of the stream, from 0 (lowest) to 100 (highest)"_s, u"quality"_s},
        {u"output"_s, u"Path of the file to write the raw h264 stream to"_s, u"file"_s, u"stream.raw"_s},
        {u"wake-after"_s, u"Inject a small mouse move via the session's fake input this many seconds after the stream started (mimics an RDP user wiggling the mouse)"_s, u"seconds"_s},
        {u"refresh-after"_s, u"Call refreshDisplayConfiguration() on the session this many seconds after the stream started"_s, u"seconds"_s},
    });
    parser.process(application);

    // Match the order krdpserver's SessionController uses: configure the
    // stream target and quality first, then enable streaming (which for the
    // Plasma session synchronously calls start()).
    KRdp::PlasmaScreencastV1Session session;
    session.setActiveStream(parser.value(u"monitor"_s).toInt());
    if (parser.isSet(u"quality"_s)) {
        session.setVideoQuality(parser.value(u"quality"_s).toUShort());
    }

    signal(SIGINT, [](int) {
        QCoreApplication::exit(0);
    });

    signal(SIGUSR1, [](int) {
        QCoreApplication::exit(0);
    });

    const QString outputPath = parser.value(u"output"_s);
    QFile file{outputPath};
    if (!file.open(QFile::WriteOnly)) {
        qDebug() << "Failed opening" << outputPath;
        return -1;
    }

    // Wall-clock offsets are measured from started().
    QElapsedTimer sinceStarted;
    qint64 frameCount = 0;
    qint64 byteCount = 0;
    qint64 keyFrameCount = 0;
    QList<qint64> firstFrameSizes;
    QSize firstFrameSize;

    QObject::connect(&session, &KRdp::AbstractSession::frameReceived, &session, [&](const KRdp::VideoFrame &frame) {
        file.write(frame.data);
        qWarning() << "Frame" << frameCount << "at +" << sinceStarted.elapsed() << "ms size" << frame.size << "bytes" << frame.data.size() << "keyframe" << frame.isKeyFrame;
        if (frameCount == 0) {
            firstFrameSize = frame.size;
        }
        if (firstFrameSizes.size() < 5) {
            firstFrameSizes.append(frame.data.size());
        }
        if (frame.isKeyFrame) {
            keyFrameCount++;
        }
        frameCount++;
        byteCount += frame.data.size();
    });

    QObject::connect(&session, &KRdp::AbstractSession::started, &application, [&sinceStarted]() {
        sinceStarted.start();
        qWarning() << "Session started";
    });

    QObject::connect(&session, &KRdp::AbstractSession::error, &application, []() {
        qWarning() << "session error";
        QCoreApplication::exit(2);
    });

    // When the run time elapses, disable streaming first so the encoder is
    // flushed (h264_vaapi holds frames until async_depth is reached or it is
    // flushed), keep the event loop alive briefly so the flushed packets are
    // delivered, then quit.
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &application, [&]() {
        qWarning() << "Run time elapsed at +" << sinceStarted.elapsed() << "ms, disabling streaming to flush encoder";
        session.requestStreamingDisable(&application);
        QTimer::singleShot(3000, &application, &QCoreApplication::quit);
    });
    int quitAfter = 0;
    if (parser.isSet(u"quit-after"_s)) {
        quitAfter = parser.value(u"quit-after"_s).toInt();
        QObject::connect(&session, &KRdp::AbstractSession::started, &timer, qOverload<>(&QTimer::start));
        timer.setInterval(quitAfter * 1000);
    }
    // Optionally mimic the RDP user moving the mouse after connecting, which is
    // what wakes DPMS-off outputs in the real scenario. Mouse motion only.
    if (parser.isSet(u"wake-after"_s)) {
        const int wakeAfter = parser.value(u"wake-after"_s).toInt();
        QObject::connect(&session, &KRdp::AbstractSession::started, &application, [&session, &sinceStarted, wakeAfter]() {
            QTimer::singleShot(wakeAfter * 1000, &session, [&session, &sinceStarted]() {
                qWarning() << "Injecting mouse move via fake input to wake the display at +" << sinceStarted.elapsed() << "ms";
                for (const auto &pos : {QPointF(100, 100), QPointF(140, 120), QPointF(100, 100)}) {
                    session.sendEvent(std::make_shared<QMouseEvent>(QEvent::MouseMove, pos, pos, pos, Qt::NoButton, Qt::NoButton, Qt::NoModifier));
                }
            });
        });
    }

    // Optionally exercise the session's own screencast re-creation path.
    if (parser.isSet(u"refresh-after"_s)) {
        const int refreshAfter = parser.value(u"refresh-after"_s).toInt();
        QObject::connect(&session, &KRdp::AbstractSession::started, &application, [&session, &sinceStarted, refreshAfter]() {
            QTimer::singleShot(refreshAfter * 1000, &session, [&session, &sinceStarted]() {
                qWarning() << "Calling refreshDisplayConfiguration() at +" << sinceStarted.elapsed() << "ms";
                session.refreshDisplayConfiguration();
            });
        });
    }

    // Hard fallback so the process cannot hang forever if started() never fires.
    QTimer::singleShot((quitAfter + 15) * 1000, &application, &QCoreApplication::quit);

    session.requestStreamingEnable(&application);
    session.start();
    auto result = application.exec();

    file.close();

    qWarning() << "Total frames:" << frameCount;
    qWarning() << "Key frames:" << keyFrameCount;
    qWarning() << "Total bytes:" << byteCount;
    qWarning() << "First frame size (pixels):" << firstFrameSize;
    qWarning() << "First 5 frame data sizes (bytes):" << firstFrameSizes;
    qWarning() << "Output written to:" << outputPath;

    return result;
}
