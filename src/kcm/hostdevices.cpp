// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "hostdevices.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace HostDevices
{
QStringList vaapiDrivers(const QStringList &driverDirs)
{
    QStringList dirs = driverDirs;
    if (dirs.isEmpty()) {
        const auto env = qEnvironmentVariable("LIBVA_DRIVERS_PATH");
        if (!env.isEmpty()) {
            dirs << env.split(u':', Qt::SkipEmptyParts);
        }
        dirs << u"/usr/lib/x86_64-linux-gnu/dri"_s << u"/usr/lib/aarch64-linux-gnu/dri"_s << u"/usr/lib64/dri"_s << u"/usr/lib/dri"_s;
    }
    QStringList found;
    for (const auto &driver : {u"radeonsi"_s, u"iHD"_s, u"i965"_s}) {
        for (const auto &dir : std::as_const(dirs)) {
            if (QFileInfo::exists(dir + u'/' + driver + u"_drv_video.so"_s)) {
                found << driver;
                break;
            }
        }
    }
    return found;
}

QList<VideoDevice> loopbackCameras(const QString &sysRoot, const QString &devRoot)
{
    QList<VideoDevice> devices;
    const QDir classDir(sysRoot + u"/class/video4linux"_s);
    const auto entries = classDir.entryList({u"video*"_s}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        // /sys/class/video4linux/videoN links into /sys/devices/virtual/... for
        // loopback devices and under a PCI or USB device for real cameras.
        const QString target = QFileInfo(classDir.filePath(entry)).canonicalFilePath();
        if (!target.contains(u"/devices/virtual/"_s)) {
            continue;
        }
        VideoDevice device;
        device.path = devRoot + u'/' + entry;
        QFile name(classDir.filePath(entry) + u"/name"_s);
        if (name.open(QIODevice::ReadOnly)) {
            device.name = QString::fromUtf8(name.readAll()).trimmed();
        }
        devices << device;
    }
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(devices.begin(), devices.end(), [&collator](const VideoDevice &a, const VideoDevice &b) {
        return collator.compare(a.path, b.path) < 0;
    });
    return devices;
}
}
