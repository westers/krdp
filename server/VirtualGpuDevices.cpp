// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualGpuDevices.h"
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <sys/stat.h>
#include <sys/sysmacros.h>
using namespace Qt::StringLiterals;
namespace KRdp::VirtualGpuDevices {
namespace {
bool snapshot(const QByteArray &path, std::vector<Device> &devices)
{
    struct stat info{};
    if (lstat(path.constData(), &info) || !S_ISCHR(info.st_mode)) return false;
    devices.push_back({path, info.st_rdev}); return true;
}
bool driverNode(const QByteArray &path, const QString &driver, uint minorNumber, std::vector<Device> &devices)
{
    QFile registrations(u"/proc/devices"_s);
    if (!registrations.open(QIODevice::ReadOnly)) return false;
    const auto characters = QString::fromUtf8(registrations.read(65536)).section(u"Block devices:"_s, 0, 0);
    const auto match = QRegularExpression(u"^\\s*([0-9]+)\\s+%1\\s*$"_s.arg(QRegularExpression::escape(driver)),
        QRegularExpression::MultilineOption).match(characters);
    bool ok = false;
    const uint majorNumber = match.captured(1).toUInt(&ok);
    struct stat info{};
    if (!match.hasMatch() || !ok || lstat(path.constData(), &info) || !S_ISCHR(info.st_mode)
        || major(info.st_rdev) != majorNumber || minor(info.st_rdev) != minorNumber) return false;
    devices.push_back({path, info.st_rdev}); return true;
}
}
std::optional<Selection> resolve(const QString &bdf)
{
    if (!QRegularExpression(u"^[0-9a-f]{4}:[0-9a-f]{2}:[01][0-9a-f]\\.[0-7]$"_s).match(bdf).hasMatch()) return {};
    const auto render = QFileInfo(u"/dev/dri/by-path/pci-%1-render"_s.arg(bdf)).canonicalFilePath();
    if (!QRegularExpression(u"^/dev/dri/renderD[0-9]+$"_s).match(render).hasMatch()) return {};
    const auto device = QFileInfo(u"/sys/class/drm/%1/device"_s.arg(QFileInfo(render).fileName())).canonicalFilePath();
    if (device.isEmpty() || QFileInfo(device).fileName() != bdf) return {};
    const auto driver = QFileInfo(QFileInfo(device + u"/driver"_s).canonicalFilePath()).fileName();
    Selection selection{bdf, render, driver, {}};
    if (!snapshot(QFile::encodeName(render), selection.nodes)) return {};
    const auto number = selection.nodes.front().number;
    const auto deviceNumber = u"%1:%2"_s.arg(major(number)).arg(minor(number));
    QFile sysfsNumber(u"/sys/class/drm/%1/dev"_s.arg(QFileInfo(render).fileName()));
    if (!sysfsNumber.open(QIODevice::ReadOnly) || sysfsNumber.read(128).trimmed() != deviceNumber.toLatin1()
        || QFileInfo(u"/sys/dev/char/%1/device"_s.arg(deviceNumber)).canonicalFilePath() != device) return {};
    if (driver == u"nvidia") {
        QFile information(u"/proc/driver/nvidia/gpus/%1/information"_s.arg(bdf));
        if (!information.open(QIODevice::ReadOnly)) return {};
        const auto match = QRegularExpression(u"^Device Minor:\\s+([0-9]+)\\s*$"_s, QRegularExpression::MultilineOption)
            .match(QString::fromUtf8(information.read(65536)));
        bool minorOk = false;
        const auto minorNumber = match.captured(1).toUInt(&minorOk);
        if (!match.hasMatch() || !minorOk || minorNumber > 255
            || !driverNode("/dev/nvidia" + QByteArray::number(minorNumber), u"nvidia"_s, minorNumber, selection.nodes)
            || !driverNode("/dev/nvidiactl", u"nvidiactl"_s, 255, selection.nodes)
            || !driverNode("/dev/nvidia-uvm", u"nvidia-uvm"_s, 0, selection.nodes)) return {};
    } else if (driver != u"amdgpu" && driver != u"i915" && driver != u"xe") return {};
    return selection;
}
}
