// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionMaintenanceCoordinator.h"
#include <QCoreApplication>
#include <cstdio>
#include <cstring>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 1) return 64;
    uid_t real, effective, saved;
    if (getresuid(&real, &effective, &saved) || real || effective || saved) return 1;
    // The service uses env -i. Do not accept caller-provided bus addresses,
    // library paths, plugin paths or invocation claims as coordinator authority.
    for (char **entry = environ; entry && *entry; ++entry)
        if (std::strcmp(*entry, "PATH=/usr/bin:/bin") && std::strcmp(*entry, "LANG=C.UTF-8")) return 1;
    QCoreApplication app(argc, argv);
    QString error;
    const auto result = KRdp::VirtualSessionMaintenanceCoordinator::bootstrap(&error);
    if (result == KRdp::VirtualSessionMaintenanceCoordinator::Result::Clean) return 0;
    std::fprintf(stderr, "Maintenance bootstrap refused: %s\n", error.toUtf8().constData());
    return result == KRdp::VirtualSessionMaintenanceCoordinator::Result::Uncertain ? 2 : 1;
}
