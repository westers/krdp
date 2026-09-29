// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// FIX-CURSOR, for WorkerEndToEndTest: a full-screen Wayland window whose cursor follows the word in
// the file given as the argument (polled): `arrow`, `ibeam` (a text field), `sizehor` (a window
// edge) or `blank` (hidden, like a full-screen video). KWin then changes (or hides) the cursor the
// capture worker reports.
//
// AUD-FIX14: `regions` makes the cursor follow the pointer instead, as Konsole's does (an I-beam over
// its text, an arrow over its scroll bar): the window sets it in its mouse move handler, i.e. only
// after KWin moved the pointer and told this client. Left third: bitmap cursor A (20x14, hotspot
// 5,9, opaque magenta), a cursor surface of this client (wl_pointer.set_cursor with an shm buffer);
// middle third: Qt::IBeamCursor (wp_cursor_shape_device_v1 `text` where KWin offers it, which
// KWin 6.6 does); right third: bitmap cursor B (32x24, hotspot 30,2, opaque cyan).

#include <algorithm>

#include <QFile>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QRasterWindow>
#include <QTimer>

static QCursor bitmapCursor(QSize size, QPoint hotspot, QColor color)
{
    QPixmap pixmap(size);
    pixmap.fill(color);
    return QCursor(pixmap, hotspot.x(), hotspot.y());
}

class Window : public QRasterWindow
{
public:
    bool regions = false;

    void followPointer(QPointF position)
    {
        if (!regions) return;
        const int region = std::clamp(int(position.x() * 3 / std::max(1, width())), 0, 2);
        if (region == m_region) return;
        m_region = region;
        if (region == 0) setCursor(bitmapCursor(QSize(20, 14), QPoint(5, 9), QColor(255, 0, 255)));
        else if (region == 1) setCursor(Qt::IBeamCursor);
        else setCursor(bitmapCursor(QSize(32, 24), QPoint(30, 2), QColor(0, 255, 255)));
        qInfo("cursor-shape-client: region %d at %.1f,%.1f", region, position.x(), position.y());
    }
    void resetRegion()
    {
        m_region = -1;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter(this).fillRect(QRect(QPoint(0, 0), size()), QColor(40, 60, 90));
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        followPointer(event->position());
    }
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Enter) followPointer(static_cast<QEnterEvent *>(event)->position());
        return QRasterWindow::event(event);
    }

private:
    int m_region = -1;
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
        window.regions = word == "regions";
        window.resetRegion();
        if (window.regions) {
            window.setCursor(Qt::ArrowCursor); // until the pointer moves
            qInfo("cursor-shape-client: regions");
            return;
        }
        if (word == "ibeam") window.setCursor(Qt::IBeamCursor);
        else if (word == "sizehor") window.setCursor(Qt::SizeHorCursor);
        else if (word == "blank") window.setCursor(Qt::BlankCursor);
        else window.setCursor(Qt::ArrowCursor);
        qInfo("cursor-shape-client: %s", word.constData());
    });
    poll.start(50);
    return app.exec();
}
