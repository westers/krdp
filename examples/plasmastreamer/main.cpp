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
        {u"codec"_s, u"Codec the session encodes for: avc420 (default, one stream), avc444 or avc444v2 (main + aux chroma stream; writes <output>.main.raw and <output>.aux.raw)"_s, u"codec"_s, u"avc420"_s},
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
        {u"chroma"_s,
         u"AVC444 chroma policy motionGapMs,restMs,maxGapMs (KRDP OPT-045b), applied via AbstractSession::setChromaPolicy() before start() - the same call path krdpserver uses for its configured default, unlike KPIPEWIRE_AVC444_* which only sets the encoder's own construction-time fallback and is overridden by this"_s,
         u"motion,rest,max"_s},
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

    KRdp::VideoCodec codec = KRdp::VideoCodec::Avc420;
    const QString codecArg = parser.value(u"codec"_s).toLower();
    if (codecArg == u"avc444v2"_s) {
        codec = KRdp::VideoCodec::Avc444v2;
    } else if (codecArg == u"avc444"_s) {
        codec = KRdp::VideoCodec::Avc444;
    } else if (codecArg != u"avc420"_s) {
        qWarning() << "--codec must be avc420, avc444 or avc444v2";
        return 2;
    }
    session.setVideoCodec(codec);
    const bool avc444 = codec != KRdp::VideoCodec::Avc420;

    if (parser.isSet(u"chroma"_s)) {
        const auto parts = parser.value(u"chroma"_s).split(u',');
        if (parts.size() != 3) {
            qWarning() << "--chroma needs motion,rest,max";
            return 2;
        }
        bool ok1 = false, ok2 = false, ok3 = false;
        const KRdp::ChromaPolicy policy{parts[0].toInt(&ok1), parts[1].toInt(&ok2), parts[2].toInt(&ok3)};
        if (!ok1 || !ok2 || !ok3) {
            qWarning() << "--chroma needs three integers";
            return 2;
        }
        // Same call path SessionController uses for the configured server default (before the
        // session's stream() is ever created), so this exercises exactly what a live
        // PipeWireBaseEncodedStream::setChromaPolicy() startup push does - unlike
        // KPIPEWIRE_AVC444_*, which only seeds the encoder's own construction-time fallback and
        // is unconditionally overridden by AbstractSession::stream()'s own applyChromaPolicyIfSupported() call.
        session.setChromaPolicy(policy);
    }

    signal(SIGINT, [](int) {
        QCoreApplication::exit(0);
    });

    signal(SIGUSR1, [](int) {
        QCoreApplication::exit(0);
    });

    // AVC420 keeps writing exactly <output>, as before; AVC444/v2 splits main and aux pictures
    // into <output>.main.raw / <output>.aux.raw. The .frames index (per-picture byte counts) is
    // written in both modes -- the probe needs it for AVC420 too.
    const QString outputPath = parser.value(u"output"_s);
    QFile file{avc444 ? outputPath + u".main.raw"_s : outputPath};
    QFile auxFile{outputPath + u".aux.raw"_s};
    QFile indexFile{outputPath + u".frames"_s};
    if (!file.open(QFile::WriteOnly)) {
        qDebug() << "Failed opening" << file.fileName();
        return -1;
    }
    if (avc444 && !auxFile.open(QFile::WriteOnly)) {
        qDebug() << "Failed opening" << auxFile.fileName();
        return -1;
    }
    if (!indexFile.open(QFile::WriteOnly)) {
        qDebug() << "Failed opening" << indexFile.fileName();
        return -1;
    }

    // Wall-clock offsets are measured from started().
    QElapsedTimer sinceStarted;
    qint64 frameCount = 0;
    qint64 byteCount = 0;
    qint64 keyFrameCount = 0;
    qint64 keyFrameBytes = 0;
    qint64 mainWithAuxCount = 0;
    qint64 mainWithAuxBytes = 0;
    qint64 lumaOnlyCount = 0;
    qint64 lumaOnlyBytes = 0;
    qint64 auxCount = 0;
    qint64 auxBytes = 0;
    qint64 auxOnlyCount = 0;
    QList<qint64> firstFrameSizes;
    QSize firstFrameSize;

    QObject::connect(&session, &KRdp::AbstractSession::frameReceived, &session, [&](const KRdp::VideoFrame &frame) {
        file.write(frame.data);
        if (avc444) {
            auxFile.write(frame.aux);
        }
        indexFile.write(QStringLiteral("%1 %2 %3 %4 %5\n")
                             .arg(frameCount)
                             .arg(frame.data.size())
                             .arg(frame.aux.size())
                             .arg(frame.isKeyFrame ? 1 : 0)
                             .arg(sinceStarted.elapsed())
                             .toUtf8());
        qWarning() << "Frame" << frameCount << "at +" << sinceStarted.elapsed() << "ms size" << frame.size << "bytes" << frame.data.size() << "keyframe"
                   << frame.isKeyFrame << "monitorIndex" << frame.monitorIndex << "aux" << frame.aux.size();
        if (frameCount == 0) {
            firstFrameSize = frame.size;
        }
        if (firstFrameSizes.size() < 5) {
            firstFrameSizes.append(frame.data.size());
        }
        if (frame.isKeyFrame) {
            keyFrameCount++;
            keyFrameBytes += frame.data.size();
        } else if (!frame.data.isEmpty() && !frame.aux.isEmpty()) {
            mainWithAuxCount++;
            mainWithAuxBytes += frame.data.size();
        } else if (!frame.data.isEmpty()) {
            lumaOnlyCount++;
            lumaOnlyBytes += frame.data.size();
        } else if (!frame.aux.isEmpty()) {
            // Aux-only refresh: the encoder shipped a chroma update with no new luma (LC=2).
            auxOnlyCount++;
        }
        if (!frame.aux.isEmpty()) {
            auxCount++;
            auxBytes += frame.aux.size();
        }
        frameCount++;
        byteCount += frame.data.size();
    });

    // AVC444 per-frame CPU cost and the encoder's aux-refresh policy counters, reported once a
    // second by the private KPipeWire (no-op signal on stock KPipeWire / AVC420 sessions).
    qint64 dlSum = 0, dlMax = 0, spSum = 0, spMax = 0, upSum = 0, upMax = 0, emSum = 0, emMax = 0, eaSum = 0, eaMax = 0;
    qint64 timedFrames = 0;
    int timingReports = 0;
    QString splitVariant;
    qint64 auxSentTotal = 0, auxSkippedTotal = 0, auxRestTotal = 0, rewriteFailTotal = 0;

    QObject::connect(&session, &KRdp::AbstractSession::chromaTimingReported, &session, [&](const KRdp::ChromaTimingReport &r) {
        dlSum += r.downloadAvg * r.frames;
        dlMax = std::max(dlMax, r.downloadMax);
        spSum += r.splitAvg * r.frames;
        spMax = std::max(spMax, r.splitMax);
        upSum += r.uploadAvg * r.frames;
        upMax = std::max(upMax, r.uploadMax);
        emSum += r.encodeMainAvg * r.frames;
        emMax = std::max(emMax, r.encodeMainMax);
        eaSum += r.encodeAuxAvg * r.frames;
        eaMax = std::max(eaMax, r.encodeAuxMax);
        timedFrames += r.frames;
        splitVariant = r.splitVariant;
        auxSentTotal += r.auxSent;
        auxSkippedTotal += r.auxSkippedMotion;
        auxRestTotal += r.auxRestRefresh;
        rewriteFailTotal += r.rewriteFailures;
        timingReports++;
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

    const double seconds = sinceStarted.isValid() ? sinceStarted.elapsed() / 1000.0 : 0.0;
    qWarning().noquote() << QStringLiteral(
                                 "Pictures: frames %1 (%2 PDU/s) key %3 avg %4 B | main P with aux %5 avg %6 B | luma-only %7 avg %8 B | aux %9 avg %10 B "
                                 "(aux-only refreshes %11)")
                                 .arg(frameCount)
                                 .arg(seconds > 0 ? frameCount / seconds : 0.0, 0, 'f', 1)
                                 .arg(keyFrameCount)
                                 .arg(keyFrameCount ? keyFrameBytes / keyFrameCount : 0)
                                 .arg(mainWithAuxCount)
                                 .arg(mainWithAuxCount ? mainWithAuxBytes / mainWithAuxCount : 0)
                                 .arg(lumaOnlyCount)
                                 .arg(lumaOnlyCount ? lumaOnlyBytes / lumaOnlyCount : 0)
                                 .arg(auxCount)
                                 .arg(auxCount ? auxBytes / auxCount : 0)
                                 .arg(auxOnlyCount);
    if (timingReports > 0 && timedFrames > 0) {
        const qint64 dlAvg = dlSum / timedFrames;
        const qint64 spAvg = spSum / timedFrames;
        const qint64 upAvg = upSum / timedFrames;
        const qint64 emAvg = emSum / timedFrames;
        const qint64 eaAvg = eaSum / timedFrames;
        qWarning().noquote() << QStringLiteral("AVC444 cost (per frame, us, avg over %1 frames / max): download %2/%3 split %4/%5 (%6) upload %7/%8 main "
                                                "queue->packet %9/%10 aux %11/%12 download+split avg %13 | policy: aux sent %14 skipped-motion %15 "
                                                "rest-refresh %16 rewrite-failures %17")
                                     .arg(timedFrames)
                                     .arg(dlAvg)
                                     .arg(dlMax)
                                     .arg(spAvg)
                                     .arg(spMax)
                                     .arg(splitVariant)
                                     .arg(upAvg)
                                     .arg(upMax)
                                     .arg(emAvg)
                                     .arg(emMax)
                                     .arg(eaAvg)
                                     .arg(eaMax)
                                     .arg(dlAvg + spAvg)
                                     .arg(auxSentTotal)
                                     .arg(auxSkippedTotal)
                                     .arg(auxRestTotal)
                                     .arg(rewriteFailTotal);
    }

    return result;
}
