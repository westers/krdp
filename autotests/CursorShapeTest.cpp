// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// FIX-CURSOR: the desktop's cursor shape from the capture worker to the broker. CursorTracker
// (the worker's and krdpserver's merge of KWin's per-stream cursor reports, rate limit and hide
// delay), the worker wire's Cursor record, the outbox holding it until Ready, and the broker
// endpoint accepting it (and nothing malformed) from a worker.

#include <QLocalSocket>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>

#include "ConsoleWorkerEndpoint.h"
#include "ConsoleWorkerOutbox.h"
#include "CursorTracker.h"

using namespace KRdp;
using Shape = ConsoleWorkerWire::CursorShape;

namespace
{
QImage arrow(int size = 32, QColor color = Qt::black)
{
    QImage image(size, size, QImage::Format_RGBA8888_Premultiplied); // what KWin's bitmap is
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.fillRect(0, 0, size / 2, size, color);
    return image;
}

PipeWireCursor visible(const QImage &texture = {}, QPoint hotspot = {}, QPoint position = {10, 10})
{
    PipeWireCursor cursor;
    cursor.position = position;
    cursor.hotspot = hotspot;
    cursor.texture = texture;
    return cursor;
}

PipeWireCursor hidden()
{
    PipeWireCursor cursor;
    cursor.visible = false;
    return cursor;
}
}

class CursorShapeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void trackerSendsShapeNotPosition();
    void trackerMergesOutputs();
    void trackerDelaysAHide();
    void trackerRateLimits();
    void trackerResendsToANewReceiver();
    void imageConversion();
    void wireRoundTrip();
    void wireRejectsMalformed();
    void outboxHoldsCursorUntilReady();
    void endpointForwardsCursor();
    void endpointRejectsMalformedCursor();
};

void CursorShapeTest::trackerSendsShapeNotPosition()
{
    CursorTracker tracker;
    tracker.report(0, visible(arrow(), {2, 3}), 0);
    const auto first = tracker.take(0);
    QVERIFY(first);
    QCOMPARE(first->type, Shape::Type::Image);
    QCOMPARE(first->size, QSize(32, 32));
    QCOMPARE(first->hotspot, QPoint(2, 3));
    // Moves only: nothing to send, however many.
    for (int i = 1; i < 20; ++i) {
        tracker.report(0, visible({}, {2, 3}, {10 + i, 10}), 100 * i);
        QVERIFY(!tracker.take(100 * i));
        QCOMPARE(tracker.dueIn(100 * i), -1);
    }
    // A new bitmap (the I-beam over a text field) goes out.
    tracker.report(0, visible(arrow(24, Qt::blue), {12, 12}), 5000);
    const auto second = tracker.take(5000);
    QVERIFY(second);
    QCOMPARE(second->size, QSize(24, 24));
    QCOMPARE(second->hotspot, QPoint(12, 12));
    // The same bitmap again (KWin re-sends it after a hide): nothing new.
    tracker.report(0, visible(arrow(24, Qt::blue), {12, 12}), 6000);
    QVERIFY(!tracker.take(6000));
}

void CursorShapeTest::trackerMergesOutputs()
{
    CursorTracker tracker;
    // Output 1 has the pointer, output 2 does not: visible.
    tracker.report(1, visible(arrow()), 0);
    tracker.report(2, hidden(), 0);
    QCOMPARE(tracker.take(0)->type, Shape::Type::Image);
    // The pointer crosses to output 2: 1 reports it gone first, 2 then has it. No hide in between.
    tracker.report(1, hidden(), 1000);
    QVERIFY(!tracker.take(1000));
    tracker.report(2, visible(), 1040);
    QVERIFY(!tracker.take(1200));
    QCOMPARE(tracker.dueIn(1200), -1);
    // Every output says "absent": hidden (a fullscreen video hid it).
    tracker.report(2, hidden(), 2000);
    QVERIFY(!tracker.take(2000));
    QCOMPARE(tracker.take(2000 + CursorTracker::HideDelayMs)->type, Shape::Type::Hidden);
    // Shown again, with no new bitmap: the last image again.
    tracker.report(2, visible(), 3000);
    const auto again = tracker.take(3000);
    QVERIFY(again);
    QCOMPARE(again->type, Shape::Type::Image);
    QCOMPARE(again->size, QSize(32, 32));
    // An output that stops capturing no longer votes: the last one's "absent" hides.
    tracker.report(1, hidden(), 4000);
    tracker.forget(2, 4000);
    QCOMPARE(tracker.take(4000 + CursorTracker::HideDelayMs)->type, Shape::Type::Hidden);
    // A rebuilt capture: no voter left, the last image is shown.
    tracker.resetSources(5000);
    QCOMPARE(tracker.take(5000)->type, Shape::Type::Image);
}

void CursorShapeTest::trackerDelaysAHide()
{
    CursorTracker tracker;
    tracker.report(0, visible(arrow()), 0);
    QVERIFY(tracker.take(0));
    tracker.report(0, hidden(), 500);
    QCOMPARE(tracker.dueIn(500), CursorTracker::HideDelayMs);
    QVERIFY(!tracker.take(599));
    QCOMPARE(tracker.dueIn(599), 1);
    QCOMPARE(tracker.take(600)->type, Shape::Type::Hidden);
    // Nothing reported yet: the default arrow, not hidden.
    CursorTracker fresh;
    QCOMPARE(fresh.take(0)->type, Shape::Type::Default);
}

void CursorShapeTest::trackerRateLimits()
{
    CursorTracker tracker;
    int sent = 0;
    // An animated cursor at 1000 fps for 1 s: at most one per MinIntervalMs, the latest wins.
    for (int ms = 0; ms < 1000; ++ms) {
        tracker.report(0, visible(arrow(32, QColor::fromHsv(ms % 360, 255, 255))), ms);
        if (tracker.take(ms)) ++sent;
    }
    QVERIFY2(sent <= 1000 / Shape::MinIntervalMs + 1, qPrintable(QString::number(sent)));
    QVERIFY(sent >= 1000 / Shape::MinIntervalMs - 1);
    // The last frame is still owed and due within the interval.
    const qint64 due = tracker.dueIn(999);
    QVERIFY(due > 0 && due <= Shape::MinIntervalMs);
    const auto last = tracker.take(999 + due);
    QVERIFY(last);
    QCOMPARE(last->pixels, CursorTracker::fromImage(arrow(32, QColor::fromHsv(999 % 360, 255, 255)), {}).pixels);
}

void CursorShapeTest::trackerResendsToANewReceiver()
{
    CursorTracker tracker;
    tracker.report(0, visible(arrow()), 0);
    QVERIFY(tracker.take(0));
    QVERIFY(!tracker.take(1000));
    tracker.resend();
    QCOMPARE(tracker.take(1000)->type, Shape::Type::Image);
}

void CursorShapeTest::imageConversion()
{
    QImage image(2, 1, QImage::Format_RGBA8888);
    image.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    image.setPixelColor(1, 0, QColor(0, 0, 255, 128));
    const Shape shape = CursorTracker::fromImage(image, {5, -3});
    QCOMPARE(shape.type, Shape::Type::Image);
    QCOMPARE(shape.hotspot, QPoint(1, 0)); // clamped into the image
    QCOMPARE(shape.pixels.size(), 8);
    // ARGB32 in memory (little endian): B, G, R, A.
    QCOMPARE(quint8(shape.pixels[0]), quint8(0));
    QCOMPARE(quint8(shape.pixels[2]), quint8(255));
    QCOMPARE(quint8(shape.pixels[3]), quint8(255));
    QCOMPARE(quint8(shape.pixels[4]), quint8(255));
    QCOMPARE(quint8(shape.pixels[7]), quint8(128));
    // Bigger than RDP allows: the default pointer.
    QCOMPARE(CursorTracker::fromImage(arrow(Shape::MaxDimension + 1), {}).type, Shape::Type::Default);
    QCOMPARE(CursorTracker::fromImage(arrow(Shape::MaxDimension), {}).type, Shape::Type::Image);
}

void CursorShapeTest::wireRoundTrip()
{
    const auto roundTrip = [](const Shape &shape) {
        ConsoleWorkerWire::Deframer deframer;
        deframer.feed(ConsoleWorkerWire::frame(shape));
        const auto record = deframer.next();
        return record ? ConsoleWorkerWire::cursorShape(*record) : std::nullopt;
    };
    const Shape image = CursorTracker::fromImage(arrow(48), {7, 9});
    QCOMPARE(roundTrip(image), std::optional(image));
    const Shape largest = CursorTracker::fromImage(arrow(Shape::MaxDimension), {383, 383});
    QCOMPARE(roundTrip(largest), std::optional(largest));
    Shape hide;
    hide.type = Shape::Type::Hidden;
    QCOMPARE(roundTrip(hide), std::optional(hide));
    QCOMPARE(roundTrip(Shape{}), std::optional(Shape{}));
    QCOMPARE(ConsoleWorkerWire::LastKind, ConsoleWorkerWire::Kind::ChromaTiming);
    QCOMPARE(ConsoleWorkerWire::ProtocolVersion, quint16(11)); // paired protocol, including Console output policy
}

void CursorShapeTest::wireRejectsMalformed()
{
    const auto parse = [](const Shape &shape) {
        ConsoleWorkerWire::Deframer deframer;
        deframer.feed(ConsoleWorkerWire::frame(shape));
        const auto record = deframer.next();
        return record ? ConsoleWorkerWire::cursorShape(*record) : std::nullopt;
    };
    Shape image = CursorTracker::fromImage(arrow(16), {1, 1});
    Shape bad = image;
    bad.pixels.chop(4);
    QVERIFY(!parse(bad)); // too few bytes for its size
    bad = image;
    bad.hotspot = QPoint(16, 0);
    QVERIFY(!parse(bad)); // hotspot outside
    bad = image;
    bad.size = QSize(0, 16);
    QVERIFY(!parse(bad));
    bad = image;
    bad.type = Shape::Type::Hidden;
    QVERIFY(!parse(bad)); // a hidden cursor carries no bitmap
    bad = CursorTracker::fromImage(arrow(Shape::MaxDimension), {});
    bad.size = QSize(Shape::MaxDimension + 1, Shape::MaxDimension);
    bad.pixels.append(QByteArray(Shape::MaxDimension * 4, '\0'));
    QVERIFY(!parse(bad)); // bigger than RDP allows
    ConsoleWorkerWire::Record record{ConsoleWorkerWire::Kind::Cursor, QByteArray(3, '\x03')};
    QVERIFY(!ConsoleWorkerWire::cursorShape(record));
    record.kind = ConsoleWorkerWire::Kind::EncoderStats;
    QVERIFY(!ConsoleWorkerWire::cursorShape(record));
}

void CursorShapeTest::outboxHoldsCursorUntilReady()
{
    QVector<ConsoleWorkerWire::Record> written;
    ConsoleWorkerOutbox outbox([&written](const QByteArray &bytes) {
        ConsoleWorkerWire::Deframer deframer;
        deframer.feed(bytes);
        while (const auto record = deframer.next()) written.append(*record);
    });
    const Shape first = CursorTracker::fromImage(arrow(16), {});
    const Shape second = CursorTracker::fromImage(arrow(20), {});
    outbox.cursor(first); // before Hello: dropped
    QVERIFY(written.isEmpty());
    outbox.hello({QStringLiteral("s"), 1000, QByteArray(24, 't')});
    outbox.cursor(first);
    outbox.cursor(second);
    QCOMPARE(written.size(), 1); // Hello only
    outbox.ready();
    QCOMPARE(written.size(), 3);
    QCOMPARE(written[1].kind, ConsoleWorkerWire::Kind::Ready);
    QCOMPARE(ConsoleWorkerWire::cursorShape(written[2]), std::optional(second)); // only the latest
    outbox.cursor(first);
    QCOMPARE(ConsoleWorkerWire::cursorShape(written.value(3)), std::optional(first));
}

void CursorShapeTest::endpointForwardsCursor()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ConsoleWorkerEndpoint endpoint;
    const QByteArray token(24, 't');
    QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), {ConsoleSeat::Adapter::VirtualUser, QStringLiteral("v1"), 1000}, token));
    QVector<Shape> received;
    connect(&endpoint, &ConsoleWorkerEndpoint::cursorShapeReceived, this, [&received](const Shape &shape) {
        received.append(shape);
    });
    QStringList errors;
    connect(&endpoint, &ConsoleWorkerEndpoint::protocolError, this, [&errors](const QString &message) {
        errors << message;
    });
    QLocalSocket worker;
    worker.connectToServer(endpoint.socketName());
    QVERIFY(worker.waitForConnected(1000));
    // As the real worker's outbox sends it: Hello, Ready, then the held cursor.
    ConsoleWorkerOutbox outbox([&worker](const QByteArray &bytes) {
        worker.write(bytes);
    });
    outbox.hello({QStringLiteral("v1"), 1000, token});
    const Shape ibeam = CursorTracker::fromImage(arrow(24, Qt::blue), {12, 12});
    outbox.cursor(ibeam);
    outbox.ready();
    QTRY_COMPARE(received.size(), 1);
    QCOMPARE(received[0], ibeam);
    QCOMPARE(endpoint.cursorShape(), std::optional(ibeam));
    Shape hide;
    hide.type = Shape::Type::Hidden;
    outbox.cursor(hide);
    QTRY_COMPARE(received.size(), 2);
    QCOMPARE(endpoint.cursorShape(), std::optional(hide));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    // A worker that goes away takes its cursor with it.
    worker.disconnectFromServer();
    QTRY_VERIFY(!endpoint.cursorShape());
}

void CursorShapeTest::endpointRejectsMalformedCursor()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ConsoleWorkerEndpoint endpoint;
    const QByteArray token(24, 't');
    QVERIFY(endpoint.listen(directory.filePath(QStringLiteral("worker.sock")), {ConsoleSeat::Adapter::VirtualUser, QStringLiteral("v1"), 1000}, token));
    QStringList errors;
    connect(&endpoint, &ConsoleWorkerEndpoint::protocolError, this, [&errors](const QString &message) {
        errors << message;
    });
    int received = 0;
    connect(&endpoint, &ConsoleWorkerEndpoint::cursorShapeReceived, this, [&received] {
        ++received;
    });
    QLocalSocket worker;
    worker.connectToServer(endpoint.socketName());
    QVERIFY(worker.waitForConnected(1000));
    worker.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{QStringLiteral("v1"), 1000, token}) + ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
    QTRY_VERIFY(endpoint.ready());
    Shape bad = CursorTracker::fromImage(arrow(16), {});
    bad.size = QSize(64, 64); // claims more than it carries
    worker.write(ConsoleWorkerWire::frame(bad));
    QTRY_VERIFY(!errors.isEmpty());
    QCOMPARE(received, 0);
    QVERIFY(!endpoint.cursorShape());
}

QTEST_MAIN(CursorShapeTest)

#include "CursorShapeTest.moc"
