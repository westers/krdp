// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// FIX-CURSOR, for WorkerEndToEndTest: a full-screen Wayland window whose cursor follows the word in
// the file given as the argument (polled): `arrow`, `ibeam` (a text field), `sizehor` (a window
// edge) or `blank` (hidden, like a full-screen video). KWin then changes (or hides) the cursor the
// capture worker reports.

#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QRasterWindow>
#include <QTimer>

class Window : public QRasterWindow
{
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter(this).fillRect(QRect(QPoint(0, 0), size()), QColor(40, 60, 90));
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 2) return 2;
    const QString control = QString::fromLocal8Bit(argv[1]);
    Window window;
    window.showFullScreen();
    QByteArray current;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &window, [&] {
        QFile file(control);
        if (!file.open(QIODevice::ReadOnly)) return;
        const QByteArray word = file.readAll().trimmed();
        if (word == current) return;
        current = word;
        if (word == "ibeam") window.setCursor(Qt::IBeamCursor);
        else if (word == "sizehor") window.setCursor(Qt::SizeHorCursor);
        else if (word == "blank") window.setCursor(Qt::BlankCursor);
        else window.setCursor(Qt::ArrowCursor);
        qInfo("cursor-shape-client: %s", word.constData());
    });
    poll.start(50);
    return app.exec();
}
