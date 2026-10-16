// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QBuffer>
#include <QImage>
#include <QTest>

#include "ClipboardImage.h"
#include "PngMimeData.h"

using namespace KRdp;

namespace
{
QByteArray makePng(int w, int h)
{
    QImage image(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) image.setPixelColor(x, y, QColor(x * 255 / w, y * 255 / h, 90, 200));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}
}

class PngMimeDataTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void offersPngAndTheQtImageFormat()
    {
        // KSystemClipboard's data-control source writes imageData() for image/png when hasImage() (measured on KF6 6.24); a
        // plain QMimeData with only setData("image/png") sent every other process an empty picture.
        const QByteArray png = makePng(40, 30);
        PngMimeData mime(png);
        QVERIFY(mime.hasFormat(QStringLiteral("image/png")));
        QVERIFY(mime.hasImage());
        QCOMPARE(mime.formats(), (QStringList{QStringLiteral("image/png"), QStringLiteral("application/x-qt-image")}));
    }

    void pngIsServedAsGiven()
    {
        const QByteArray png = makePng(40, 30);
        PngMimeData mime(png);
        QCOMPARE(mime.data(QStringLiteral("image/png")), png);
    }

    void theQtImageIsDecodedOnDemandAndMatches()
    {
        const QByteArray png = makePng(40, 30);
        PngMimeData mime(png);
        const QImage image = qvariant_cast<QImage>(mime.imageData());
        QCOMPARE(image.size(), QSize(40, 30));
        QVERIFY(ClipboardImage::samePixels(png, [&] {
            QByteArray out;
            QBuffer buffer(&out);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return out;
        }()));
    }

    void otherFormatsAreNotOffered()
    {
        PngMimeData mime(makePng(8, 8));
        QVERIFY(!mime.hasText());
        QVERIFY(!mime.hasFormat(QStringLiteral("text/plain")));
        QVERIFY(mime.data(QStringLiteral("text/plain")).isEmpty());
    }

    void thePasswordManagerHintIsOnlyOfferedWhenAsked()
    {
        PngMimeData plain(makePng(8, 8));
        QVERIFY(!plain.hasFormat(QStringLiteral("x-kde-passwordManagerHint")));
        PngMimeData secret(makePng(8, 8), QByteArrayLiteral("secret"));
        QVERIFY(secret.hasFormat(QStringLiteral("x-kde-passwordManagerHint")));
        QCOMPARE(secret.data(QStringLiteral("x-kde-passwordManagerHint")), QByteArrayLiteral("secret"));
        QVERIFY(secret.formats().contains(QStringLiteral("image/png")));
    }

    void samePixelsIgnoresTheEncoding()
    {
        const QByteArray a = makePng(32, 32);
        QImage image = QImage::fromData(a, "PNG");
        QByteArray b; // another encoding of the same pixels: a different compression level
        QBuffer buffer(&b);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG", 0);
        QVERIFY(a != b);
        QVERIFY(ClipboardImage::samePixels(a, b));
        QVERIFY(!ClipboardImage::samePixels(a, makePng(32, 33)));
        image.setPixelColor(3, 3, Qt::white);
        QByteArray c;
        QBuffer cb(&c);
        cb.open(QIODevice::WriteOnly);
        image.save(&cb, "PNG");
        QVERIFY(!ClipboardImage::samePixels(a, c));
        QVERIFY(!ClipboardImage::samePixels(a, QByteArray("not a png")));
    }

    void pixelHashFollowsThePixelsNotTheBytes()
    {
        const QByteArray a = makePng(32, 32);
        QImage image = QImage::fromData(a, "PNG");
        QByteArray b;
        QBuffer buffer(&b);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG", 0);
        QVERIFY(a != b);
        QVERIFY(ClipboardImage::pixelHash(a) != 0);
        QCOMPARE(ClipboardImage::pixelHash(a), ClipboardImage::pixelHash(b));
        QVERIFY(ClipboardImage::pixelHash(a) != ClipboardImage::pixelHash(makePng(32, 33)));
        image.setPixelColor(1, 1, Qt::white);
        QByteArray c;
        QBuffer cb(&c);
        cb.open(QIODevice::WriteOnly);
        image.save(&cb, "PNG");
        QVERIFY(ClipboardImage::pixelHash(a) != ClipboardImage::pixelHash(c));
        QCOMPARE(ClipboardImage::pixelHash(QByteArray("not a png")), quint64(0));
    }

    void mimeOrderPutsPngFirstAndAgreesWithPreferredMime()
    {
        QCOMPARE(ClipboardImage::mimeOrder().first(), QStringLiteral("image/png"));
        QCOMPARE(ClipboardImage::preferredMime({QStringLiteral("image/bmp"), QStringLiteral("image/png")}), QStringLiteral("image/png"));
        QCOMPARE(ClipboardImage::preferredMime({QStringLiteral("image/bmp")}), QStringLiteral("image/bmp"));
        QVERIFY(ClipboardImage::preferredMime({QStringLiteral("text/plain")}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(PngMimeDataTest)
#include "PngMimeDataTest.moc"
