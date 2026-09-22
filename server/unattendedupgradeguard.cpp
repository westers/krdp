// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualUnattendedUpgradeGuard.h"
#include <cstdio>

int main(int argc, char **argv) {
    QString error;
    const int result = KRdp::VirtualUnattendedUpgradeGuard::run(argc, argv, &error);
    if (!error.isEmpty()) std::fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
    return result;
}
