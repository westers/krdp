// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

#include "PipeWireAudioPlayback.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    KRdp::PipeWireAudioPlayback audio;
    if (!audio.startIsolated(QStringLiteral("probe"))) {
        QTextStream(stderr) << "Could not create the isolated PipeWire sink" << Qt::endl;
        return 1;
    }
    QTextStream(stdout) << "isolated PipeWire sink active" << Qt::endl;
    QTimer::singleShot(1000, &app, [&]() {
        audio.stop();
        QTextStream(stdout) << "previous PipeWire default restored" << Qt::endl;
        app.quit();
    });
    return app.exec();
}
