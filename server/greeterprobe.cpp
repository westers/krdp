// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QGuiApplication>
#include <QScreen>
#include <QTextStream>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    QTextStream out(stdout);
    out << "platform=" << QGuiApplication::platformName()
        << " wayland_display=" << qEnvironmentVariable("WAYLAND_DISPLAY")
        << " runtime_dir=" << qEnvironmentVariable("XDG_RUNTIME_DIR") << Qt::endl;

    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        out << "screen=" << screen->name() << " geometry=" << screen->geometry().x() << ',' << screen->geometry().y() << ' '
            << screen->geometry().width() << 'x' << screen->geometry().height() << Qt::endl;
    }
    return screens.isEmpty() ? 2 : 0;
}
