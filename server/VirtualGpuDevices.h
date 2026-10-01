// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <QString>
#include <optional>
#include <vector>
#include <sys/types.h>
namespace KRdp::VirtualGpuDevices {
struct Device { QByteArray path; dev_t number; };
struct Selection { QString pci; QString render; QString driver; std::vector<Device> nodes; };
// Read-only resolution used by both host administration and the actual device
// namespace authority. Never opens a GPU or changes grants/ACLs/accounts.
std::optional<Selection> resolve(const QString &pci);
}
