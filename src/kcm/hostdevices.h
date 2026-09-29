// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// What this computer has, so the page offers only choices that can work
// (REBRAND-PLAN.md §4: hide what the hardware can't do).
namespace HostDevices
{
// The VAAPI drivers the server can pick by name ("radeonsi", "iHD", "i965")
// that are installed in one of `driverDirs` (LIBVA_DRIVERS_PATH and the usual
// dri directories when empty).
QStringList vaapiDrivers(const QStringList &driverDirs = {});

struct VideoDevice {
    QString path; // /dev/videoN
    QString name; // the card label
};

// v4l2loopback devices: virtual video4linux nodes (no hardware parent).
QList<VideoDevice> loopbackCameras(const QString &sysRoot = QStringLiteral("/sys"), const QString &devRoot = QStringLiteral("/dev"));
}
