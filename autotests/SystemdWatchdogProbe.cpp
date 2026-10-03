// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-055 K6 (T-K6e helper, not a test and not installed): a minimal daemon that wires SystemdNotify
// and StallDetector the way farside-server and farside-console-host do, then optionally blocks its
// main thread, so a transient systemd unit can show the watchdog firing. Run it only from a
// scratch/transient unit:
//   SystemdWatchdogProbe [--block-after SECONDS --block-for SECONDS]

#include "StallDetector.h"
#include "SystemdNotify.h"
#include "TerminateHandler.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv)
{
    KRdp::installTerminateHandler("SystemdWatchdogProbe");
    KRdp::SystemdNotify::ping();
    QCoreApplication application(argc, argv);
    int blockAfter = -1;
    int blockFor = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--block-after")) {
            blockAfter = std::atoi(argv[i + 1]);
        } else if (!std::strcmp(argv[i], "--block-for")) {
            blockFor = std::atoi(argv[i + 1]);
        }
    }
    KRdp::StallDetector::setPhase("startup");
    KRdp::SystemdNotify watchdog(&application);
    const bool armed = watchdog.start();
    KRdp::StallDetector stallDetector(&application);
    stallDetector.start();
    std::fprintf(stderr, "probe: watchdog %s, period %lld us, blockAfter %d blockFor %d\n", armed ? "armed" : "not armed",
                 static_cast<long long>(KRdp::SystemdNotify::watchdogPeriod().count()), blockAfter, blockFor);
    if (blockAfter >= 0) {
        QTimer::singleShot(blockAfter * 1000, &application, [blockFor] {
            std::fprintf(stderr, "probe: blocking the main thread for %d s\n", blockFor);
            KRdp::StallDetector::Phase phase("probe forced block");
            QThread::sleep(blockFor);
        });
    }
    KRdp::StallDetector::setPhase("event loop");
    return application.exec();
}
