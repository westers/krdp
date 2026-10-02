// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "LayoutControl.h"
#include <QFile>
#include <QRegularExpression>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace KRdp::CameraAvailability
{
// Read-only preflight. Opening a producer here would advertise a camera before
// consent. The actual desktop worker must still open/configure the producer.
inline QString reason(const QString &path)
{
    const auto setup = QStringLiteral("An administrator must install v4l2loopback-dkms and v4l2loopback-utils on the remote computer, "
        "configure a loopback device in Farside Host Settings, restart the host, then reconnect.");
    if (path.isEmpty() || path == QStringLiteral("none"))
        return QStringLiteral("Camera sharing needs a camera bridge on the remote computer. ") + setup;
    if (!QRegularExpression(QStringLiteral("^/dev/video[0-9]+$")).match(path).hasMatch())
        return QStringLiteral("The remote camera bridge path is invalid. ") + setup;
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return QStringLiteral("The remote camera bridge is missing or inaccessible. ") + setup;
    struct stat info{};
    v4l2_capability caps{};
    const bool valid = !::fstat(fd, &info) && S_ISCHR(info.st_mode) && major(info.st_rdev) == 81
        && !::ioctl(fd, VIDIOC_QUERYCAP, &caps)
        && QByteArray(reinterpret_cast<const char *>(caps.driver), sizeof(caps.driver)).split('\0').front() == "v4l2 loopback";
    ::close(fd);
    return valid ? QString() : QStringLiteral("The configured camera bridge is not a V4L2 loopback device. ") + setup;
}

inline QString virtualReason()
{
    return QStringLiteral("Camera sharing is unavailable for Virtual desktops because their isolated device access does not yet include a camera bridge. "
        "Use a configured Console connection for camera sharing.");
}

inline LayoutControl::DeviceCapabilities capabilities(LayoutControl::DeviceCapabilities base, const QString &reason)
{
    base.cameraUnavailableReason = reason;
    if (!reason.isEmpty()) base.cameraToggle = base.cameraReselect = false;
    return base;
}
}
