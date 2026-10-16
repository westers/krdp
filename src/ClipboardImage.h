// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QBuffer>
#include <QByteArray>
#include <QByteArrayView>
#include <QImage>
#include <QHashFunctions>
#include <QImageReader>
#include <QPainter>
#include <QSize>
#include <QStringList>
#include <QtEndian>

namespace KRdp::ClipboardImage
{
/**
 * OPT-072: image conversion for the clipboard bridge, as pure functions (QImage needs no display).
 *
 * The bridge carries images as PNG between the broker and the worker. On the cliprdr wire an image is
 * either the registered "PNG" format (passed through untouched after a structure check) or CF_DIB /
 * CF_DIBV5 (converted here). Every input is untrusted: a client's clipboard, or whatever an application
 * on the host put there. Nothing is allocated for a picture before its declared size has been checked
 * against the caps below (a decompression-bomb guard on width * height), and nothing here ever logs or
 * returns pixel contents in an error.
 */
/// The most an encoded image (PNG) is, on the worker wire and as the "PNG" cliprdr format.
constexpr qint64 MaxEncodedBytes = 16 * 1024 * 1024;
/// The most a raw DIB is on the cliprdr wire (either direction).
constexpr qint64 MaxDibBytes = 64 * 1024 * 1024;
/// The most one side of a picture is, and the most pixels.
constexpr int MaxDimension = 8192;
constexpr qint64 MaxPixels = qint64(MaxDimension) * MaxDimension;

/// Standard Windows clipboard format ids (MS-RDPECLIP 2.2.3.1.1) and the registered PNG format name.
constexpr quint32 CfDib = 8;
constexpr quint32 CfDibV5 = 17;
constexpr quint32 PngFormatId = 0xC001; ///< the id this side gives "PNG" in its own format lists
inline bool isPngFormatName(const QString &name)
{
    return name.compare(QLatin1String("PNG"), Qt::CaseInsensitive) == 0 || name.compare(QLatin1String("image/png"), Qt::CaseInsensitive) == 0;
}

/// The host clipboard formats a picture is read from, best first: PNG, then the formats Qt can decode for us.
inline const QStringList &mimeOrder()
{
    static const QStringList order = {QStringLiteral("image/png"), QStringLiteral("image/jpeg"), QStringLiteral("image/bmp"),
                                      QStringLiteral("image/x-bmp"), QStringLiteral("image/x-ms-bmp"), QStringLiteral("image/webp"),
                                      QStringLiteral("image/tiff"), QStringLiteral("image/gif")};
    return order;
}

/// The host clipboard format to read a picture from, among \a mimeTypes: PNG first, then the formats Qt can decode
/// for us; empty when the clipboard offers no picture. Text-only and file copies are never pictures.
inline QString preferredMime(const QStringList &mimeTypes)
{
    for (const QString &candidate : mimeOrder()) {
        if (mimeTypes.contains(candidate)) return candidate;
    }
    return {};
}

enum class Error : quint8 {
    None,
    Empty, ///< nothing to convert
    Malformed, ///< not a picture of the kind it says, truncated, or inconsistent
    TooLarge, ///< over the byte cap (encoded or DIB)
    TooManyPixels, ///< over the dimension cap
    Unsupported, ///< a valid but unsupported kind (RLE, JPEG-in-DIB, an unknown header)
};

inline const char *errorName(Error error)
{
    switch (error) {
    case Error::None: return "none";
    case Error::Empty: return "empty";
    case Error::Malformed: return "malformed";
    case Error::TooLarge: return "oversize";
    case Error::TooManyPixels: return "dimensions";
    case Error::Unsupported: return "unsupported";
    }
    return "unknown";
}

struct Result {
    QByteArray data;
    Error error = Error::None;
    QSize size;
    bool ok() const { return error == Error::None; }
};

namespace detail
{
inline Result fail(Error error, QSize size = {})
{
    return Result{{}, error, size};
}

inline quint32 le32(const char *p)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(p));
}
inline quint16 le16(const char *p)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(p));
}
inline quint32 be32(const char *p)
{
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(p));
}
inline void put32(QByteArray &out, quint32 value)
{
    char b[4];
    qToLittleEndian<quint32>(value, reinterpret_cast<uchar *>(b));
    out.append(b, 4);
}
inline void put16(QByteArray &out, quint16 value)
{
    char b[2];
    qToLittleEndian<quint16>(value, reinterpret_cast<uchar *>(b));
    out.append(b, 2);
}

inline bool dimensionsOk(qint64 w, qint64 h)
{
    return w >= 1 && h >= 1 && w <= MaxDimension && h <= MaxDimension && w * h <= MaxPixels;
}
}

/**
 * Validate the structure of a PNG without decoding it: the signature, an IHDR first with sane fields,
 * well-formed chunks (each fits in the data), IDAT present and an IEND last with nothing after it. The
 * declared dimensions are checked against the caps. CRCs are not verified (a decoder does). This is all
 * a PNG needs before it is passed on: the pixel count a decoder will allocate comes from the IHDR, and
 * that has been bounded.
 */
inline Result validatePng(QByteArrayView png)
{
    using namespace detail;
    if (png.isEmpty()) return fail(Error::Empty);
    if (png.size() > MaxEncodedBytes) return fail(Error::TooLarge);
    static const char signature[8] = {char(0x89), 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (png.size() < 8 + 25 + 12 || memcmp(png.constData(), signature, 8) != 0) return fail(Error::Malformed);
    const char *p = png.constData();
    const qint64 size = png.size();
    qint64 pos = 8;
    bool first = true;
    bool idat = false;
    bool iend = false;
    QSize dimensions;
    while (pos < size) {
        if (iend || size - pos < 12) return fail(Error::Malformed); // data after IEND, or a cut-off chunk header
        const quint32 length = be32(p + pos);
        const char *type = p + pos + 4;
        if (length > 0x7fffffffu || qint64(length) + 12 > size - pos) return fail(Error::Malformed);
        if (first) {
            if (memcmp(type, "IHDR", 4) != 0 || length != 13) return fail(Error::Malformed);
            const quint32 w = be32(p + pos + 8);
            const quint32 h = be32(p + pos + 12);
            const quint8 depth = quint8(p[pos + 16]);
            const quint8 colour = quint8(p[pos + 17]);
            const quint8 compression = quint8(p[pos + 18]);
            const quint8 filter = quint8(p[pos + 19]);
            const quint8 interlace = quint8(p[pos + 20]);
            if (!w || !h || w > 0x7fffffffu || h > 0x7fffffffu) return fail(Error::Malformed);
            if (w > quint32(MaxDimension) || h > quint32(MaxDimension) || qint64(w) * qint64(h) > MaxPixels) {
                return fail(Error::TooManyPixels, QSize(int(qMin<quint32>(w, 0x7fffffff)), int(qMin<quint32>(h, 0x7fffffff))));
            }
            const bool depthOk = (colour == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16))
                || (colour == 2 && (depth == 8 || depth == 16)) || (colour == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8))
                || ((colour == 4 || colour == 6) && (depth == 8 || depth == 16));
            if (!depthOk || compression != 0 || filter != 0 || interlace > 1) return fail(Error::Malformed);
            dimensions = QSize(int(w), int(h));
            first = false;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            idat = true;
        } else if (memcmp(type, "IEND", 4) == 0) {
            if (length != 0) return fail(Error::Malformed);
            iend = true;
        }
        pos += 12 + length;
    }
    if (first || !idat || !iend) return fail(Error::Malformed);
    return Result{QByteArray(png.constData(), png.size()), Error::None, dimensions};
}

namespace detail
{
inline QImage decodeLimited(const QByteArray &bytes, const char *format, QSize expected)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format);
    reader.setAllocationLimit(512); // MB; the dimension checks come first, this is the backstop
    if (reader.size().isValid() && reader.size() != expected) return {};
    return reader.read();
}

inline QByteArray encodePng(const QImage &image)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) return {};
    return out;
}

inline qint64 dibStride(qint64 width, qint64 bits)
{
    return ((width * bits + 31) / 32) * 4;
}
}

/** A PNG image's bytes to PNG as the bridge carries it (validated; the same bytes). */
inline Result pngPassThrough(QByteArrayView png)
{
    return validatePng(png);
}

/// True when both are decodable pictures with the same size and pixels, whatever their encoding (a PNG can be re-encoded
/// without changing a pixel). Callers validate the dimensions first (validatePng / toPng); this decodes both.
inline bool samePixels(QByteArrayView a, QByteArrayView b)
{
    const QImage x = QImage::fromData(a.toByteArray(), "PNG");
    const QImage y = QImage::fromData(b.toByteArray(), "PNG");
    if (x.isNull() || y.isNull() || x.size() != y.size()) return false;
    return x.convertToFormat(QImage::Format_ARGB32) == y.convertToFormat(QImage::Format_ARGB32);
}

/// A hash of a picture's size and pixels, not of its bytes: the same picture in another PNG encoding hashes the same
/// (KSystemClipboard serves other processes its own re-encoding, so byte hashes of the "same" copy differ between the
/// writer and a reader). 0 when \a png does not decode. The caller has validated the dimensions already.
inline quint64 pixelHash(QByteArrayView png)
{
    const QImage image = QImage::fromData(png.toByteArray(), "PNG");
    if (image.isNull()) return 0;
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    const quint64 h = qHashBits(argb.constBits(), size_t(argb.sizeInBytes()), qHash(argb.size().width() * 100003 + argb.size().height()));
    return h ? h : 1;
}

/**
 * Any image format a host application may have put on the clipboard (PNG, JPEG, BMP, ...) as PNG. A PNG is
 * validated and passed through; anything else is decoded after its declared size has been checked, then
 * encoded. Refused when over the dimension or the encoded-size cap.
 */
inline Result toPng(QByteArrayView encoded)
{
    using namespace detail;
    if (encoded.isEmpty()) return fail(Error::Empty);
    if (encoded.size() >= 8 && encoded.first(4) == QByteArrayView("\x89PNG", 4)) return validatePng(encoded);
    if (encoded.size() > MaxDibBytes) return fail(Error::TooLarge);
    const QByteArray bytes(encoded.constData(), encoded.size());
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader probe(&buffer);
    const QSize declared = probe.size();
    if (!declared.isValid()) return fail(Error::Malformed);
    if (!dimensionsOk(declared.width(), declared.height())) return fail(Error::TooManyPixels, declared);
    probe.setAllocationLimit(512);
    const QImage image = probe.read();
    if (image.isNull()) return fail(Error::Malformed, declared);
    const QByteArray png = encodePng(image);
    if (png.isEmpty()) return fail(Error::Malformed, declared);
    if (png.size() > MaxEncodedBytes) return fail(Error::TooLarge, declared);
    return Result{png, Error::None, image.size()};
}

/**
 * A CF_DIB / CF_DIBV5 payload (BITMAPINFOHEADER or newer, palette, pixels; no file header) as PNG.
 * The header is parsed and bounded here before Qt sees a byte: size, dimensions, plane count, bit depth,
 * compression (RLE and embedded JPEG/PNG are refused), palette size, and that the pixel data is all
 * there. Only then is a BMP file header put in front and the picture decoded.
 */
inline Result dibToPng(QByteArrayView dib)
{
    using namespace detail;
    if (dib.isEmpty()) return fail(Error::Empty);
    if (dib.size() > MaxDibBytes) return fail(Error::TooLarge);
    if (dib.size() < 40) return fail(Error::Malformed);
    const char *p = dib.constData();
    const quint32 headerSize = le32(p);
    if (headerSize != 40 && headerSize != 108 && headerSize != 124) return fail(headerSize == 12 || headerSize == 64 ? Error::Unsupported : Error::Malformed);
    if (qint64(dib.size()) < headerSize) return fail(Error::Malformed);
    const qint32 width = qint32(le32(p + 4));
    const qint32 heightRaw = qint32(le32(p + 8));
    const qint64 height = heightRaw < 0 ? -qint64(heightRaw) : qint64(heightRaw);
    const quint16 planes = le16(p + 12);
    const quint16 bits = le16(p + 14);
    const quint32 compression = le32(p + 16);
    const quint32 colorsUsed = le32(p + 32);
    if (width < 1 || height < 1) return fail(Error::Malformed);
    if (!dimensionsOk(width, height)) return fail(Error::TooManyPixels, QSize(int(qMin<qint64>(width, MaxDimension + 1)), int(qMin<qint64>(height, MaxDimension + 1))));
    if (planes != 1) return fail(Error::Malformed);
    if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) return fail(Error::Malformed);
    enum : quint32 { BiRgb = 0, BiRle8 = 1, BiRle4 = 2, BiBitfields = 3, BiJpeg = 4, BiPng = 5, BiAlphaBitfields = 6 };
    if (compression == BiRle8 || compression == BiRle4 || compression == BiJpeg || compression == BiPng) return fail(Error::Unsupported);
    if (compression != BiRgb && compression != BiBitfields && compression != BiAlphaBitfields) return fail(Error::Malformed);
    if ((compression == BiBitfields || compression == BiAlphaBitfields) && bits != 16 && bits != 32) return fail(Error::Malformed);
    qint64 maskBytes = 0;
    if (headerSize == 40 && compression == BiBitfields) maskBytes = 12;
    else if (headerSize == 40 && compression == BiAlphaBitfields) maskBytes = 16;
    qint64 palette = 0;
    if (bits <= 8) {
        const quint32 maximum = quint32(1) << bits;
        if (colorsUsed > maximum) return fail(Error::Malformed);
        palette = colorsUsed ? colorsUsed : maximum;
    } else if (colorsUsed) {
        if (colorsUsed > 256) return fail(Error::Malformed);
        palette = colorsUsed;
    }
    const qint64 pixelsOffset = qint64(headerSize) + maskBytes + palette * 4;
    const qint64 pixelBytes = dibStride(width, bits) * height;
    if (pixelsOffset + pixelBytes > dib.size()) return fail(Error::Malformed, QSize(width, int(height)));

    QByteArray bmp;
    bmp.reserve(14 + dib.size());
    bmp.append("BM", 2);
    put32(bmp, quint32(14 + dib.size()));
    put32(bmp, 0);
    put32(bmp, quint32(14 + pixelsOffset));
    bmp.append(dib.constData(), dib.size());
    const QImage image = decodeLimited(bmp, "BMP", QSize(width, int(height)));
    if (image.isNull()) return fail(Error::Malformed, QSize(width, int(height)));
    const QByteArray png = encodePng(image);
    if (png.isEmpty()) return fail(Error::Malformed, image.size());
    if (png.size() > MaxEncodedBytes) return fail(Error::TooLarge, image.size());
    return Result{png, Error::None, image.size()};
}

/**
 * A PNG as a CF_DIB payload: BITMAPINFOHEADER, 24 bits, bottom-up, any transparency flattened onto white
 * (the common DIB readers ignore alpha). Refused, before any decoding, when the raw result would pass
 * MaxDibBytes or the PNG is malformed or over the dimension caps.
 */
inline Result pngToDib(QByteArrayView png)
{
    using namespace detail;
    const Result checked = validatePng(png);
    if (!checked.ok()) return checked;
    const qint64 width = checked.size.width();
    const qint64 height = checked.size.height();
    const qint64 stride = dibStride(width, 24);
    if (40 + stride * height > MaxDibBytes) return fail(Error::TooLarge, checked.size);
    const QImage decoded = decodeLimited(checked.data, "PNG", checked.size);
    if (decoded.isNull() || decoded.size() != checked.size) return fail(Error::Malformed, checked.size);
    QImage flat(decoded.size(), QImage::Format_RGB888);
    flat.fill(Qt::white);
    {
        QPainter painter(&flat);
        painter.drawImage(0, 0, decoded);
    }
    QByteArray out;
    out.reserve(40 + stride * height);
    put32(out, 40);
    put32(out, quint32(width));
    put32(out, quint32(height)); // positive: bottom-up
    put16(out, 1);
    put16(out, 24);
    put32(out, 0); // BI_RGB
    put32(out, quint32(stride * height));
    put32(out, 2835);
    put32(out, 2835);
    put32(out, 0);
    put32(out, 0);
    QByteArray row(int(stride), '\0');
    for (qint64 y = height - 1; y >= 0; --y) {
        const uchar *src = flat.constScanLine(int(y));
        for (qint64 x = 0; x < width; ++x) { // RGB -> BGR
            row[int(x * 3)] = char(src[x * 3 + 2]);
            row[int(x * 3 + 1)] = char(src[x * 3 + 1]);
            row[int(x * 3 + 2)] = char(src[x * 3]);
        }
        out.append(row);
    }
    return Result{out, Error::None, checked.size};
}
}
