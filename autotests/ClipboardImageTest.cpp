// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QBuffer>
#include <QTest>

#include "ClipboardImage.h"

using namespace KRdp::ClipboardImage;

namespace
{
QImage sample(int w = 6, int h = 4, bool alpha = false)
{
    QImage image(w, h, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            image.setPixel(x, y, qRgba(x * 40 % 256, y * 60 % 256, (x + y) * 30 % 256, alpha && x == 0 ? 0 : 255));
        }
    }
    return image;
}

QByteArray png(const QImage &image)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return out;
}

void put32le(QByteArray &out, quint32 v) { for (int i = 0; i < 4; ++i) out.append(char((v >> (8 * i)) & 0xff)); }
void put16le(QByteArray &out, quint16 v) { out.append(char(v & 0xff)); out.append(char(v >> 8)); }

// A BITMAPINFOHEADER-family header, then \a tail (masks, palette, pixels) as given.
QByteArray dibHeader(quint32 headerSize, qint32 w, qint32 h, quint16 bits, quint32 compression, quint32 colorsUsed = 0, quint16 planes = 1)
{
    QByteArray out;
    put32le(out, headerSize);
    put32le(out, quint32(w));
    put32le(out, quint32(h));
    put16le(out, planes);
    put16le(out, bits);
    put32le(out, compression);
    put32le(out, 0);
    put32le(out, 2835);
    put32le(out, 2835);
    put32le(out, colorsUsed);
    put32le(out, 0);
    out.append(QByteArray(qMax<qint64>(0, qint64(headerSize) - 40), '\0'));
    return out;
}

Result dibOf(const QByteArray &dib)
{
    return dibToPng(dib);
}

/// A real PNG with its IHDR width and height replaced (CRC left stale; the structure check does not read it).
QByteArray pngWithDeclaredSize(quint32 w, quint32 h)
{
    QByteArray data = png(sample());
    for (int i = 0; i < 4; ++i) {
        data[16 + i] = char((w >> (8 * (3 - i))) & 0xff);
        data[20 + i] = char((h >> (8 * (3 - i))) & 0xff);
    }
    return data;
}
}

class ClipboardImageTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void pngValidates()
    {
        const auto data = png(sample());
        const auto result = validatePng(data);
        QVERIFY(result.ok());
        QCOMPARE(result.size, QSize(6, 4));
        QCOMPARE(result.data, data);
    }

    void pngRefusesEmptyAndGarbageAndTruncated()
    {
        QCOMPARE(validatePng({}).error, Error::Empty);
        QCOMPARE(validatePng(QByteArray(100, 'x')).error, Error::Malformed);
        const auto data = png(sample());
        QCOMPARE(validatePng(QByteArrayView(data).first(data.size() - 1)).error, Error::Malformed); // IEND cut
        QCOMPARE(validatePng(QByteArrayView(data).first(40)).error, Error::Malformed);
        QCOMPARE(validatePng(QByteArray(data + QByteArray("junk"))).error, Error::Malformed); // data after IEND
        QCOMPARE(validatePng(QByteArray("\x89PNG\r\n\x1a\n", 8)).error, Error::Malformed);
    }

    void pngRefusesOversizeBytes()
    {
        QByteArray big = png(sample());
        big.append(QByteArray(MaxEncodedBytes, '\0'));
        QCOMPARE(validatePng(big).error, Error::TooLarge);
    }

    void pngBombIsRefusedFromTheHeaderAlone()
    {
        // 60000 x 60000 declared by a 70-byte file: refused before anything could allocate it.
        const auto bomb = pngWithDeclaredSize(60000, 60000);
        QVERIFY(bomb.size() < 200);
        QCOMPARE(validatePng(bomb).error, Error::TooManyPixels);
        QCOMPARE(validatePng(pngWithDeclaredSize(8193, 1)).error, Error::TooManyPixels);
        QCOMPARE(validatePng(pngWithDeclaredSize(1, 8193)).error, Error::TooManyPixels);
        QCOMPARE(validatePng(pngWithDeclaredSize(0xffffffffu, 0xffffffffu)).error, Error::Malformed);
        QCOMPARE(validatePng(pngWithDeclaredSize(0, 4)).error, Error::Malformed);
        QVERIFY(validatePng(pngWithDeclaredSize(8192, 8192)).ok()); // exactly at the cap
        QCOMPARE(pngToDib(bomb).error, Error::TooManyPixels);
    }

    void pngRefusesBadIhdrFields()
    {
        auto data = png(sample());
        data[24] = 7; // bit depth
        QCOMPARE(validatePng(data).error, Error::Malformed);
        data = png(sample());
        data[28] = 2; // interlace method
        QCOMPARE(validatePng(data).error, Error::Malformed);
        data = png(sample());
        data[12] = 'X'; // the first chunk is not IHDR
        QCOMPARE(validatePng(data).error, Error::Malformed);
    }

    void pngToDibAndBack()
    {
        const QImage image = sample(7, 5); // odd width: rows are padded to 4 bytes
        const auto dib = pngToDib(png(image));
        QVERIFY(dib.ok());
        QCOMPARE(dib.data.size(), 40 + 24 * 5); // 7*3 = 21 -> 24 bytes per row
        QCOMPARE(qint32(detail::le32(dib.data.constData() + 8)), 5); // bottom-up
        const auto back = dibToPng(dib.data);
        QVERIFY(back.ok());
        QImage again;
        QVERIFY(again.loadFromData(back.data, "PNG"));
        QCOMPARE(again.size(), image.size());
        QCOMPARE(again.convertToFormat(QImage::Format_RGB32), image.convertToFormat(QImage::Format_RGB32));
    }

    void pngToDibFlattensTransparencyOntoWhite()
    {
        const auto dib = pngToDib(png(sample(3, 2, true)));
        QVERIFY(dib.ok());
        const auto back = dibToPng(dib.data);
        QImage again;
        QVERIFY(again.loadFromData(back.data, "PNG"));
        QCOMPARE(again.pixel(0, 0) & 0xffffff, 0xffffffu); // x == 0 was fully transparent
    }

    void pngToDibRefusesWhatWouldPassTheRawCap()
    {
        // 8000 x 8000 x 3 = 192 MB raw: the PNG is tiny, the DIB is refused before decoding.
        QCOMPARE(pngToDib(pngWithDeclaredSize(8000, 8000)).error, Error::TooLarge);
    }

    void dibReads32BitAndTopDown()
    {
        QByteArray dib = dibHeader(40, 2, -2, 32, 0); // top-down BI_RGB, B G R X
        const quint8 px[2][2][4] = {{{255, 0, 0, 0}, {0, 255, 0, 0}}, {{0, 0, 255, 0}, {10, 20, 30, 0}}};
        for (auto &row : px) for (auto &p : row) dib.append(reinterpret_cast<const char *>(p), 4);
        const auto result = dibToPng(dib);
        QVERIFY(result.ok());
        QImage image;
        QVERIFY(image.loadFromData(result.data, "PNG"));
        QCOMPARE(image.pixel(0, 0) & 0xffffff, 0x0000ffu); // blue
        QCOMPARE(image.pixel(1, 0) & 0xffffff, 0x00ff00u);
        QCOMPARE(image.pixel(0, 1) & 0xffffff, 0xff0000u);
        QCOMPARE(image.pixel(1, 1) & 0xffffff, 0x1e140au);
    }

    void dibReadsPalette8Bit()
    {
        QByteArray dib = dibHeader(40, 2, 2, 8, 0, 2);
        put32le(dib, 0x00ff0000); // index 0: red
        put32le(dib, 0x000000ff); // index 1: blue
        const char rows[] = {0, 1, 0, 0, 1, 0, 0, 0}; // two 4-byte rows
        dib.append(rows, 8);
        const auto result = dibToPng(dib);
        QVERIFY(result.ok());
        QImage image;
        QVERIFY(image.loadFromData(result.data, "PNG"));
        QCOMPARE(image.size(), QSize(2, 2));
        QCOMPARE(image.pixel(0, 1) & 0xffffff, 0xff0000u); // bottom-up: the first row is the bottom
        QCOMPARE(image.pixel(1, 1) & 0xffffff, 0x0000ffu);
    }

    void dibReadsV5WithAlpha()
    {
        QByteArray dib = dibHeader(124, 1, 1, 32, 3); // BI_BITFIELDS, masks inside the V5 header
        const quint32 masks[4] = {0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000};
        for (int i = 0; i < 4; ++i) for (int b = 0; b < 4; ++b) dib[40 + i * 4 + b] = char((masks[i] >> (8 * b)) & 0xff);
        dib.append(QByteArray::fromHex("20406080")); // B G R A
        const auto result = dibToPng(dib);
        QVERIFY2(result.ok(), errorName(result.error));
        QImage image;
        QVERIFY(image.loadFromData(result.data, "PNG"));
        QVERIFY(image.hasAlphaChannel());
        QCOMPARE(qAlpha(image.pixel(0, 0)), 0x80);
    }

    void dibRefusesBadHeaders()
    {
        QCOMPARE(dibToPng({}).error, Error::Empty);
        QCOMPARE(dibToPng(QByteArray(20, 0)).error, Error::Malformed); // shorter than a header
        QCOMPARE(dibOf(dibHeader(41, 1, 1, 24, 0) + QByteArray(8, 0)).error, Error::Malformed); // header size
        QCOMPARE(dibOf(dibHeader(40, 1, 1, 24, 0, 0, 2) + QByteArray(8, 0)).error, Error::Malformed); // planes
        QCOMPARE(dibOf(dibHeader(40, 1, 1, 7, 0) + QByteArray(8, 0)).error, Error::Malformed); // bit count
        QCOMPARE(dibOf(dibHeader(40, 0, 1, 24, 0) + QByteArray(8, 0)).error, Error::Malformed); // zero width
        QCOMPARE(dibOf(dibHeader(40, 1, 0, 24, 0) + QByteArray(8, 0)).error, Error::Malformed);
        QCOMPARE(dibOf(dibHeader(40, 1, 1, 24, 9) + QByteArray(8, 0)).error, Error::Malformed); // compression
        QCOMPARE(dibOf(dibHeader(40, 1, 1, 24, 3) + QByteArray(24, 0)).error, Error::Malformed); // bitfields on 24 bits
        QCOMPARE(dibOf(dibHeader(40, 1, 1, 8, 0, 300) + QByteArray(1200, 0)).error, Error::Malformed); // palette too big
        QCOMPARE(dibOf(dibHeader(12, 1, 1, 24, 0) + QByteArray(8, 0)).error, Error::Unsupported); // BITMAPCOREHEADER
    }

    void dibRefusesUnsupportedCompression()
    {
        QCOMPARE(dibOf(dibHeader(40, 4, 4, 8, 1, 2) + QByteArray(64, 0)).error, Error::Unsupported); // RLE8
        QCOMPARE(dibOf(dibHeader(40, 4, 4, 4, 2, 2) + QByteArray(64, 0)).error, Error::Unsupported); // RLE4
        QCOMPARE(dibOf(dibHeader(40, 4, 4, 24, 4) + QByteArray(64, 0)).error, Error::Unsupported); // embedded JPEG
        QCOMPARE(dibOf(dibHeader(40, 4, 4, 24, 5) + QByteArray(64, 0)).error, Error::Unsupported); // embedded PNG
    }

    void dibRefusesTruncatedPixelData()
    {
        QCOMPARE(dibOf(dibHeader(40, 4, 4, 24, 0) + QByteArray(47, 0)).error, Error::Malformed); // needs 48
        QVERIFY(dibOf(dibHeader(40, 4, 4, 24, 0) + QByteArray(48, 0)).ok());
    }

    void dibBombIsRefusedFromTheHeaderAlone()
    {
        // A 40-byte header claiming 100000 x 100000, and one a hair over the dimension cap.
        QCOMPARE(dibOf(dibHeader(40, 100000, 100000, 32, 0)).error, Error::TooManyPixels);
        QCOMPARE(dibOf(dibHeader(40, 8193, 1, 24, 0)).error, Error::TooManyPixels);
        QCOMPARE(dibOf(dibHeader(40, 1, -8193, 24, 0)).error, Error::TooManyPixels);
        QCOMPARE(dibOf(dibHeader(40, qint32(0x7fffffff), qint32(0x7fffffff), 32, 0)).error, Error::TooManyPixels);
        QCOMPARE(dibOf(dibHeader(40, 8192, 8192, 32, 0)).error, Error::Malformed); // at the cap, but no pixels follow
        QCOMPARE(dibOf(dibHeader(40, qint32(0x80000000), 1, 24, 0)).error, Error::Malformed); // negative width
    }

    void dibRefusesOversizeBytes()
    {
        QCOMPARE(dibOf(dibHeader(40, 1, 1, 24, 0) + QByteArray(MaxDibBytes, 0)).error, Error::TooLarge);
    }

    void toPngPassesPngAndConvertsOthers()
    {
        const QImage image = sample();
        const auto data = png(image);
        const auto same = toPng(data);
        QVERIFY(same.ok());
        QCOMPARE(same.data, data);
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(image.save(&buffer, "JPEG"));
        const auto converted = toPng(jpeg);
        QVERIFY(converted.ok());
        QCOMPARE(converted.size, image.size());
        QVERIFY(validatePng(converted.data).ok());
        QByteArray bmp;
        QBuffer bmpBuffer(&bmp);
        bmpBuffer.open(QIODevice::WriteOnly);
        QVERIFY(image.save(&bmpBuffer, "BMP"));
        QVERIFY(toPng(bmp).ok());
        QCOMPARE(toPng({}).error, Error::Empty);
        QCOMPARE(toPng(QByteArray("this is not a picture")).error, Error::Malformed);
    }

    void formatNames()
    {
        QVERIFY(isPngFormatName(QStringLiteral("PNG")));
        QVERIFY(isPngFormatName(QStringLiteral("image/png")));
        QVERIFY(!isPngFormatName(QStringLiteral("HTML Format")));
        QVERIFY(!isPngFormatName(QString()));
    }

    void errorNamesAreStable()
    {
        QCOMPARE(QString::fromLatin1(errorName(Error::TooLarge)), QStringLiteral("oversize"));
        QCOMPARE(QString::fromLatin1(errorName(Error::TooManyPixels)), QStringLiteral("dimensions"));
        QCOMPARE(QString::fromLatin1(errorName(Error::Malformed)), QStringLiteral("malformed"));
    }
};

QTEST_GUILESS_MAIN(ClipboardImageTest)

#include "ClipboardImageTest.moc"
