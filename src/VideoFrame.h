// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <chrono>
#include <memory>

#include <QByteArray>
#include <QRegion>
#include <QSize>
#include <QVector>

#include "SurfaceLayout.h"

namespace KRdp
{

/**
 * A frame of compressed video data.
 *
 * This is the single definition; VideoStream.h includes it. It used to be
 * duplicated there, which quietly gave PortalSession.cpp (the only direct
 * includer of this header) a VideoFrame without monitorIndex.
 */
struct VideoFrame {
    /**
     * The size of the frame, in pixels.
     */
    QSize size;
    /**
     * h264 compressed data in YUV420 color space.
     */
    QByteArray data;
    /**
     * AVC444: the auxiliary chroma picture of the same frame; empty for a luma-only frame and in AVC420.
     */
    QByteArray aux;
    /**
     * Informational (the aux is the next picture of the same H.264 stream; `isKeyFrame` is the one that matters).
     */
    bool auxIsKeyFrame = false;
    /**
     * Area of the frame that was actually damaged.
     * TODO: Actually use this information.
     */
    QRegion damage;
    /**
     * Whether the packet contains all the information
     */
    bool isKeyFrame;
    /**
     * Logical monitor layout mapped into this frame's coordinate space.
     */
    QVector<VideoMonitor> monitors;
    /**
     * Index of the surface this frame belongs to (0 unless MonitorMode=multi).
     */
    int monitorIndex = 0;
    /**
     * When was this frame presented.
     */
    std::chrono::system_clock::time_point presentationTimeStamp;
};

}
