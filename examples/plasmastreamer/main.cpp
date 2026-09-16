// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// Test harness: capture the screen through the same pipeline krdpserver uses
// (PlasmaScreencastV1Session -> KPipeWire -> encoder) and dump raw H.264 to a
// file, without any RDP client involved.

#include <algorithm>
#include <csignal>
#include <memory>
#include <vector>

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

namespace
{

// One capture/encode pipeline per physical screen: its own
// PlasmaScreencastV1Session (and therefore its own PipeWireEncodedStream /
// VA-API encoder), writing to its own output file.
struct SessionRun {
    std::unique_ptr<KRdp::PlasmaScreencastV1Session> session;
    QFile file;
    int frames = 0;
    int keyframes = 0;
    qint64 bytes = 0;
};

// Feasibility gate for per-monitor surfaces (OPT-018): run every screen
// through its own session/encoder concurrently instead of the single shared
// session the rest of this harness drives, so we can check whether the
// Radeon 780M can actually sustain two h264_vaapi encoders at once.
int runMulti(QGuiApplication &application, const QCommandLineParser &parser)
{
    signal(SIGINT, [](int) {
        QCoreApplication::exit(0);
    });

    signal(SIGUSR1, [](int) {
        QCoreApplication::exit(0);
    });

    const QString outputPath = parser.value(u"output"_s);
    const auto screens = qGuiApp->screens();

    std::vector<std::unique_ptr<SessionRun>> runs;
    runs.reserve(screens.size());
    for (int i = 0; i < screens.size(); ++i) {
        auto run = std::make_unique<SessionRun>();
        run->file.setFileName(outputPath + u"." + QString::number(i) + u".raw");
        if (!run->file.open(QFile::WriteOnly)) {
            qDebug() << "Failed opening" << run->file.fileName();
            return -1;
        }
        runs.push_back(std::move(run));
    }

    // Wall-clock offsets are measured from the moment every session has
    // started (see the started() handler below), same as the single-session
    // path measures from its one started().
    QElapsedTimer sinceStarted;
    int startedCount = 0;

    QTimer timer;
    timer.setSingleShot(true);
    const bool hasQuitAfter = parser.isSet(u"quit-after"_s);
    const int quitAfter = hasQuitAfter ? parser.value(u"quit-after"_s).toInt() : 0;
    timer.setInterval(quitAfter * 1000);

    QObject::connect(&timer, &QTimer::timeout, &application, [&]() {
        qWarning() << "Run time elapsed at +" << sinceStarted.elapsed() << "ms, disabling streaming to flush encoders";
        for (auto &run : runs) {
            run->session->requestStreamingDisable(&application);
        }
        QTimer::singleShot(3000, &application, &QCoreApplication::quit);
    });

    for (int i = 0; i < screens.size(); ++i) {
        auto *run = runs[i].get();
        run->session = std::make_unique<KRdp::PlasmaScreencastV1Session>();
        run->session->setActiveStream(i);
        run->session->setMonitorIndex(i);
        if (parser.isSet(u"quality"_s)) {
            run->session->setVideoQuality(parser.value(u"quality"_s).toUShort());
        }

        auto *session = run->session.get();
        QObject::connect(session, &KRdp::AbstractSession::frameReceived, session, [run, i, &sinceStarted](const KRdp::VideoFrame &frame) {
            run->file.write(frame.data);
            qWarning() << "Session" << i << "frame" << run->frames << "at +" << sinceStarted.elapsed() << "ms size" << frame.size << "bytes" << frame.data.size()
                       << "keyframe" << frame.isKeyFrame << "monitorIndex" << frame.monitorIndex;
            if (frame.isKeyFrame) {
                run->keyframes++;
            }
            run->frames++;
            run->bytes += frame.data.size();
        });

        // The screencast runs in Screencasting::Metadata cursor mode, so the
        // pointer is NOT painted into the video: moving it over an idle screen
        // produces no damage and no frames. cursorUpdate() carries the pointer
        // position in *this* session's own stream coordinates, which is the
        // direct readout of where KWin actually placed the pointer.
        QObject::connect(session, &KRdp::AbstractSession::cursorUpdate, session, [i, &sinceStarted](const PipeWireCursor &cursor) {
            qWarning() << "Session" << i << "cursor at" << cursor.position << "at +" << sinceStarted.elapsed() << "ms";
        });

        QObject::connect(session, &KRdp::AbstractSession::started, &application, [i, &screens, &startedCount, &sinceStarted, &timer, hasQuitAfter]() {
            qWarning() << "Session" << i << "started";
            startedCount++;
            if (startedCount == screens.size()) {
                sinceStarted.start();
                if (hasQuitAfter) {
                    timer.start();
                }
            }
        });

        QObject::connect(session, &KRdp::AbstractSession::error, &application, [i]() {
            qWarning() << "Session" << i << "error";
            QCoreApplication::exit(2);
        });
    }

    // Each session's sendEvent() normalizes the position against that
    // session's *own* captured size before offsetting by its *own* logical
    // rect (see PlasmaScreencastV1Session::sendEvent()), and owns its own
    // FakeInput object. So injecting through session 0 can only ever land
    // inside session 0's own screen, however large a position is passed in
    // (it clamps). To make a *different* screen busy, inject through that
    // session instead; --wake-pos is local to that session's own capture.
    //
    // --wake-global takes the other route deliberately: sendGlobalEvent()
    // skips the normalization and hands the position to fake input as-is, so
    // --wake-pos is read as a KWin-global workspace coordinate. That is the
    // check for whether org_kde_kwin_fake_input's pointer_motion_absolute
    // really addresses the whole workspace: inject through session 0 at a
    // point inside another screen and see whether that screen's session
    // starts producing frames.
    if (parser.isSet(u"wake-after"_s) && !runs.empty()) {
        const auto wakeOffsets = parser.value(u"wake-after"_s).split(u',', Qt::SkipEmptyParts);
        const auto posParts = parser.value(u"wake-pos"_s).split(u',');
        const QPointF wakePos(posParts.value(0).toDouble(), posParts.value(1).toDouble());
        const int wakeSessionIndex = std::clamp(parser.value(u"wake-session"_s).toInt(), 0, int(runs.size()) - 1);
        const bool wakeGlobal = parser.isSet(u"wake-global"_s);
        auto *wakeSession = runs[wakeSessionIndex]->session.get();
        QObject::connect(wakeSession,
                         &KRdp::AbstractSession::started,
                         &application,
                         [wakeSession, &sinceStarted, wakeOffsets, wakePos, wakeSessionIndex, wakeGlobal]() {
                             for (const auto &offset : wakeOffsets) {
                                 QTimer::singleShot(offset.toInt() * 1000, wakeSession, [wakeSession, &sinceStarted, wakePos, wakeSessionIndex, wakeGlobal]() {
                                     qWarning() << "Injecting mouse move via fake input at" << wakePos << (wakeGlobal ? "(KWin-global)" : "(session-local)")
                                                << "into session" << wakeSessionIndex << "to wake the display at +" << sinceStarted.elapsed() << "ms";
                                     for (const auto &pos : {wakePos, wakePos + QPointF(40, 20), wakePos}) {
                                         auto event =
                                             std::make_shared<QMouseEvent>(QEvent::MouseMove, pos, pos, pos, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                                         if (wakeGlobal) {
                                             wakeSession->sendGlobalEvent(event);
                                         } else {
                                             wakeSession->sendEvent(event);
                                         }
                                     }
                                 });
                             }
                         });
    }

    // Hard fallback so the process cannot hang forever if started() never
    // fires for one of the sessions.
    QTimer::singleShot((quitAfter + 15) * 1000, &application, &QCoreApplication::quit);

    for (auto &run : runs) {
        run->session->requestStreamingEnable(&application);
    }

    auto result = application.exec();

    for (auto &run : runs) {
        run->file.close();
    }

    const double elapsedSeconds = sinceStarted.isValid() ? sinceStarted.elapsed() / 1000.0 : 0.0;
    for (int i = 0; i < int(runs.size()); ++i) {
        auto &run = runs[i];
        const double avgFps = elapsedSeconds > 0.0 ? run->frames / elapsedSeconds : 0.0;
        qWarning().noquote() << QString(u"Session %1: frames %2 keyframes %3 bytes %4 avg_fps %5")
                                     .arg(i)
                                     .arg(run->frames)
                                     .arg(run->keyframes)
                                     .arg(run->bytes)
                                     .arg(avgFps, 0, 'f', 2);
    }

    return result;
}

}

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
        {u"wake-after"_s, u"Inject a small mouse move via the session's fake input at these offsets (seconds, comma separated) after the stream started (mimics an RDP user wiggling the mouse)"_s, u"seconds"_s},
        {u"wake-pos"_s, u"Position \"x,y\" for the --wake-after mouse move, in the target session's own screen pixels; only used with --multi (default 100,100)"_s, u"x,y"_s, u"100,100"_s},
        {u"wake-session"_s,
         u"Index into the --multi session list to inject --wake-after moves through; each session's fake input is scoped to its own screen (sendEvent() clamps the position into that session's own captured size before offsetting by its logical rect), so this picks which screen gets the activity (default 0)"_s,
         u"index"_s,
         u"0"_s},
        {u"refresh-after"_s, u"Call refreshDisplayConfiguration() on the session this many seconds after the stream started"_s, u"seconds"_s},
        {u"keyframe-at"_s, u"Call requestKeyFrame() on the session at these offsets (seconds, comma separated) after the stream started"_s, u"seconds"_s},
        {u"quality-at"_s, u"Set the session video quality at these offsets: seconds:quality, comma separated (e.g. 4:40,8:90)"_s, u"list"_s},
        {u"multi"_s, u"Capture every screen with its own session and encoder; writes <output>.<index>.raw"_s},
        {u"wake-global"_s,
         u"With --wake-after and --multi: inject through AbstractSession::sendGlobalEvent() instead of sendEvent(), treating --wake-pos as a KWin-global workspace coordinate rather than a position on the target session's own screen"_s},
    });
    parser.process(application);

    if (parser.isSet(u"multi"_s)) {
        return runMulti(application, parser);
    }

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
        qWarning() << "Frame" << frameCount << "at +" << sinceStarted.elapsed() << "ms size" << frame.size << "bytes" << frame.data.size() << "keyframe"
                   << frame.isKeyFrame << "monitorIndex" << frame.monitorIndex;
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
        const auto wakeOffsets = parser.value(u"wake-after"_s).split(u',', Qt::SkipEmptyParts);
        QObject::connect(&session, &KRdp::AbstractSession::started, &application, [&session, &sinceStarted, wakeOffsets]() {
            for (const auto &offset : wakeOffsets) {
                QTimer::singleShot(offset.toInt() * 1000, &session, [&session, &sinceStarted]() {
                    qWarning() << "Injecting mouse move via fake input to wake the display at +" << sinceStarted.elapsed() << "ms";
                    for (const auto &pos : {QPointF(100, 100), QPointF(140, 120), QPointF(100, 100)}) {
                        session.sendEvent(std::make_shared<QMouseEvent>(QEvent::MouseMove, pos, pos, pos, Qt::NoButton, Qt::NoButton, Qt::NoModifier));
                    }
                });
            }
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

    // Optionally call requestKeyFrame() directly, exercising the API path when
    // the linked KPipeWire supports it (or the restart fallback otherwise).
    if (parser.isSet(u"keyframe-at"_s)) {
        const auto offsets = parser.value(u"keyframe-at"_s).split(u',', Qt::SkipEmptyParts);
        QObject::connect(&session, &KRdp::AbstractSession::started, &application, [&session, &sinceStarted, offsets]() {
            for (const auto &offset : offsets) {
                QTimer::singleShot(offset.toInt() * 1000, &session, [&session, &sinceStarted]() {
                    qInfo() << "Requesting keyframe at +" << sinceStarted.elapsed() << "ms";
                    session.requestKeyFrame();
                });
            }
        });
    }

    // Optionally change the session's video quality partway through the run,
    // exercising the h264_vaapi reopen path (or the software encoder's live
    // global_quality update).
    if (parser.isSet(u"quality-at"_s)) {
        const auto entries = parser.value(u"quality-at"_s).split(u',', Qt::SkipEmptyParts);
        QObject::connect(&session, &KRdp::AbstractSession::started, &application, [&session, &sinceStarted, entries]() {
            for (const auto &entry : entries) {
                const auto parts = entry.split(u':');
                if (parts.size() != 2) {
                    qWarning() << "Ignoring malformed --quality-at entry" << entry;
                    continue;
                }
                const int offset = parts[0].toInt();
                const int quality = std::clamp(parts[1].toInt(), 0, 100);
                QTimer::singleShot(offset * 1000, &session, [&session, &sinceStarted, quality]() {
                    qWarning() << "Setting video quality to" << quality << "at +" << sinceStarted.elapsed() << "ms";
                    session.setVideoQuality(quint8(quality));
                });
            }
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
