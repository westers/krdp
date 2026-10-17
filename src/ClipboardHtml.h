// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringDecoder>

#include <optional>

namespace KRdp::ClipboardHtml
{
/// Largest HTML (UTF-8 bytes of the document handed to or taken from a host application) either direction carries.
constexpr qsizetype MaxHtmlBytes = 8 * 1024 * 1024;
/// The most "HTML Format" can be on the cliprdr wire: the document plus its header and the NUL (the header is 105 bytes
/// when we write it; other senders add a SourceURL line, so leave room).
constexpr qsizetype MaxWireBytes = MaxHtmlBytes + 4096;

/// The registered clipboard format name (MS "HTML Clipboard Format") and the id this side gives it in its own lists.
inline const char *formatName()
{
    return "HTML Format";
}
constexpr quint32 HtmlFormatId = 0xC002;

inline bool isHtmlFormatName(const QString &name)
{
    return name.compare(QLatin1String("HTML Format"), Qt::CaseInsensitive) == 0;
}

enum class Error { None, Empty, TooLarge, Malformed, Inconsistent };

inline const char *errorName(Error error)
{
    switch (error) {
    case Error::None: return "none";
    case Error::Empty: return "empty";
    case Error::TooLarge: return "oversize";
    case Error::Malformed: return "malformed";
    case Error::Inconsistent: return "inconsistent";
    }
    return "unknown";
}

struct Result {
    QByteArray data;
    Error error = Error::None;
    bool ok() const { return error == Error::None; }
};

namespace detail
{
constexpr qsizetype HeaderLength = 105; // "Version:0.9\r\n" + four "Key:0000000000\r\n" lines, as Windows and FreeRDP write it
constexpr qsizetype MaxHeaderScan = 2048;
inline const QByteArray StartMarker = QByteArrayLiteral("<!--StartFragment-->");
inline const QByteArray EndMarker = QByteArrayLiteral("<!--EndFragment-->");

inline QByteArray removeNuls(QByteArray bytes)
{
    if (bytes.contains('\0')) bytes.replace('\0', QByteArray());
    return bytes;
}

/// Valid UTF-8 out: an invalid sequence becomes U+FFFD, NULs are dropped.
inline QByteArray sanitizeUtf8(const QByteArray &bytes)
{
    QByteArray clean = removeNuls(bytes);
    if (!QByteArrayView(clean).isValidUtf8()) clean = QString::fromUtf8(clean).toUtf8();
    return clean;
}

inline QByteArray utf16ToUtf8(QStringConverter::Encoding encoding, QByteArrayView bytes)
{
    QStringDecoder decoder(encoding);
    const QString text = decoder.decode(bytes);
    return text.toUtf8();
}

inline bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline bool onlySpace(QByteArrayView view)
{
    for (const char c : view) {
        if (!isSpace(c)) return false;
    }
    return true;
}

/// A header number: ASCII digits (zero padding allowed, at most 10 of them) or a leading '-' (all the sender's way of saying
/// "unknown"; -1 is taken as absent). Anything else is malformed.
enum class Number { Absent, Value, Bad };
inline Number parseNumber(QByteArrayView text, qint64 &value)
{
    if (!text.isEmpty() && text.front() == '-') {
        for (qsizetype i = 1; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') return Number::Bad;
        }
        return text.size() > 1 ? Number::Absent : Number::Bad;
    }
    if (text.isEmpty() || text.size() > 10) return Number::Bad;
    qint64 parsed = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return Number::Bad;
        parsed = parsed * 10 + (c - '0');
    }
    value = parsed;
    return Number::Value;
}

struct Header {
    std::optional<qint64> startHtml;
    std::optional<qint64> endHtml;
    std::optional<qint64> startFragment;
    std::optional<qint64> endFragment;
    qsizetype length = 0; ///< bytes of the header, including its last line break
};

/// The header at the start of \a data, or nullopt for none ("Version:" first) or an unreadable one (\a bad set).
inline std::optional<Header> parseHeader(QByteArrayView data, bool &bad)
{
    bad = false;
    if (!data.startsWith("Version:")) return std::nullopt;
    Header header;
    qsizetype pos = 0;
    const qsizetype scan = qMin(data.size(), MaxHeaderScan);
    while (pos < scan) {
        qsizetype eol = data.first(scan).indexOf('\n', pos);
        if (eol < 0) break;
        QByteArrayView line = data.mid(pos, eol - pos);
        if (line.endsWith('\r')) line.chop(1);
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) break; // the first line that is not "Key:value" starts the document
        bool keyOk = true;
        for (const char c : line.first(colon)) keyOk = keyOk && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-');
        if (!keyOk) break;
        const QByteArray key = QByteArray(line.first(colon)).toLower();
        const QByteArrayView value = line.sliced(colon + 1);
        std::optional<qint64> *target = key == "starthtml" ? &header.startHtml : key == "endhtml" ? &header.endHtml
            : key == "startfragment" ? &header.startFragment : key == "endfragment" ? &header.endFragment : nullptr;
        if (target) {
            qint64 number = 0;
            const Number kind = parseNumber(value, number);
            if (kind == Number::Bad) {
                bad = true;
                return std::nullopt;
            }
            if (kind == Number::Value) *target = number;
        }
        pos = eol + 1;
        header.length = pos;
    }
    return header;
}

/// "<meta charset>" so a browser or office suite reading the selection as bytes does not guess Latin-1.
inline QByteArray ensureCharset(QByteArray html)
{
    const QByteArray head = html.left(2048).toLower();
    if (head.contains("charset")) return html;
    static const QByteArray meta = QByteArrayLiteral("<meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\">");
    const qsizetype headTag = head.indexOf("<head");
    if (headTag >= 0) {
        const qsizetype close = head.indexOf('>', headTag);
        if (close >= 0) {
            html.insert(close + 1, meta);
            return html;
        }
    }
    return meta + html;
}
}

/**
 * OPT-077: a host application's text/html bytes as UTF-8. Some sources (Chrome, X11 bridges) offer UTF-16, with or without a
 * byte order mark; everything else is taken as UTF-8 (a UTF-8 BOM dropped, an invalid sequence replaced). NULs are removed.
 * Never longer than the input times two; the caller bounds the input.
 */
inline QByteArray normaliseHostHtml(const QByteArray &raw)
{
    const auto *p = reinterpret_cast<const uchar *>(raw.constData());
    const qsizetype n = raw.size();
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        return detail::removeNuls(detail::utf16ToUtf8(QStringDecoder::Utf16LE, QByteArrayView(raw).sliced(2).first((n - 2) & ~qsizetype(1))));
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        return detail::removeNuls(detail::utf16ToUtf8(QStringDecoder::Utf16BE, QByteArrayView(raw).sliced(2).first((n - 2) & ~qsizetype(1))));
    }
    if (n >= 4 && p[0] != 0 && p[1] == 0 && p[2] != 0 && p[3] == 0) { // ASCII-looking UTF-16LE without a BOM
        return detail::removeNuls(detail::utf16ToUtf8(QStringDecoder::Utf16LE, QByteArrayView(raw).first(n & ~qsizetype(1))));
    }
    if (n >= 4 && p[0] == 0 && p[1] != 0 && p[2] == 0 && p[3] != 0) {
        return detail::removeNuls(detail::utf16ToUtf8(QStringDecoder::Utf16BE, QByteArrayView(raw).first(n & ~qsizetype(1))));
    }
    QByteArray bytes = raw;
    if (bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);
    return detail::sanitizeUtf8(bytes);
}

/**
 * OPT-077: the cliprdr "HTML Format" payload (MS "HTML Clipboard Format": a text header with byte offsets, then the HTML,
 * normally NUL terminated) as the HTML document a host application gets as text/html.
 *
 * Accepted, as real senders write it: zero-padded or short offsets, CRLF or LF header lines, extra header keys (SourceURL,
 * StartSelection...), a missing or -1 EndHTML (the document then runs to the end), no trailing NUL, and HTML with no header
 * at all (it must then start with '<'). The document is the StartHTML..EndHTML range, or when only fragment offsets are given
 * the fragment. Refused: an unreadable number, an offset past the data, StartHTML after EndHTML or inside the header, a
 * fragment outside the document, no usable offset, an empty or over-long (MaxHtmlBytes) document. The output is valid
 * UTF-8 with no NUL and carries a charset declaration. Nothing is logged here; \a Result::error names the refusal.
 */
inline Result unwrapMsHtml(const QByteArray &wire)
{
    using namespace detail;
    if (wire.size() > MaxWireBytes) return {{}, Error::TooLarge};
    qsizetype size = wire.size();
    while (size > 0 && wire[size - 1] == '\0') --size; // trailing terminators (some senders write several)
    const QByteArrayView data = QByteArrayView(wire).first(size);
    if (data.isEmpty() || onlySpace(data)) return {{}, Error::Empty};

    bool bad = false;
    const auto header = parseHeader(data, bad);
    if (bad) return {{}, Error::Malformed};
    QByteArray html;
    if (!header) {
        qsizetype first = 0;
        while (first < size && isSpace(data[first])) ++first;
        if (data[first] != '<') return {{}, Error::Malformed};
        html = wire.left(size);
    } else {
        // The wire's own length bounds every offset; the NUL(s) are not part of it.
        const qint64 limit = wire.size();
        const auto inRange = [&](const std::optional<qint64> &v) { return !v || (*v >= header->length && *v <= limit); };
        if (!inRange(header->startHtml) || !inRange(header->endHtml) || !inRange(header->startFragment) || !inRange(header->endFragment)) {
            return {{}, Error::Inconsistent};
        }
        qint64 begin = 0;
        qint64 end = 0;
        if (header->startHtml) {
            begin = *header->startHtml;
            end = header->endHtml ? *header->endHtml : size;
            if (header->startFragment && header->endFragment
                && (*header->startFragment < begin || *header->endFragment > end || *header->startFragment > *header->endFragment)) {
                return {{}, Error::Inconsistent};
            }
        } else if (header->startFragment) {
            begin = *header->startFragment;
            end = header->endFragment ? *header->endFragment : size;
        } else {
            return {{}, Error::Malformed};
        }
        end = qMin<qint64>(end, size); // a sender whose EndHTML counts the terminator
        if (begin > end) return {{}, Error::Inconsistent};
        if (end - begin > MaxHtmlBytes) return {{}, Error::TooLarge};
        html = wire.mid(begin, end - begin);
    }
    html = sanitizeUtf8(html);
    if (html.size() > MaxHtmlBytes) return {{}, Error::TooLarge};
    if (onlySpace(html)) return {{}, Error::Empty};
    html = ensureCharset(html);
    if (html.size() > MaxHtmlBytes) return {{}, Error::TooLarge};
    return {html, Error::None};
}

/**
 * OPT-077: \a html (UTF-8, as a host application offers text/html, already through normaliseHostHtml) as the cliprdr
 * "HTML Format" payload, NUL terminated, with exact byte offsets: a 105-byte header (ten-digit offsets, CRLF) and the
 * document. A document that already has <!--StartFragment--> and <!--EndFragment--> keeps them; one with a <body> gets the
 * markers inside it; a bare fragment is wrapped in <html><body>. StartFragment/EndFragment point just after / at the markers.
 */
inline Result wrapForWire(const QByteArray &html)
{
    using namespace detail;
    QByteArray doc = sanitizeUtf8(html);
    if (doc.startsWith("\xEF\xBB\xBF")) doc.remove(0, 3);
    if (onlySpace(doc)) return {{}, Error::Empty};
    if (doc.size() > MaxHtmlBytes) return {{}, Error::TooLarge};

    qsizetype startMarker = doc.indexOf(StartMarker);
    qsizetype endMarker = startMarker >= 0 ? doc.indexOf(EndMarker, startMarker + StartMarker.size()) : -1;
    if (startMarker < 0 || endMarker < 0) {
        const QByteArray lower = doc.toLower();
        const qsizetype body = lower.indexOf("<body");
        const qsizetype bodyClose = body >= 0 ? lower.indexOf('>', body) : -1;
        if (bodyClose >= 0) {
            const qsizetype closing = lower.lastIndexOf("</body");
            doc.insert(bodyClose + 1, StartMarker);
            const qsizetype at = closing > bodyClose ? closing + StartMarker.size() : doc.size();
            doc.insert(at, EndMarker);
        } else {
            doc = QByteArrayLiteral("<html><body>") + StartMarker + doc + EndMarker + QByteArrayLiteral("</body></html>");
        }
        startMarker = doc.indexOf(StartMarker);
        endMarker = doc.indexOf(EndMarker, startMarker + StartMarker.size());
    }
    const qint64 startHtml = HeaderLength;
    const qint64 endHtml = HeaderLength + doc.size();
    const qint64 startFragment = HeaderLength + startMarker + StartMarker.size();
    const qint64 endFragment = HeaderLength + endMarker;
    if (endHtml + 1 > MaxWireBytes) return {{}, Error::TooLarge};
    const auto number = [](qint64 value) { return QByteArray::number(value).rightJustified(10, '0'); };
    QByteArray out = "Version:0.9\r\nStartHTML:" + number(startHtml) + "\r\nEndHTML:" + number(endHtml) + "\r\nStartFragment:" + number(startFragment)
        + "\r\nEndFragment:" + number(endFragment) + "\r\n";
    out += doc;
    out.append('\0');
    return {out, Error::None};
}

/// Which clipboard formats a copy with \a hasText, \a hasHtml and \a hasImage is bridged as.
struct Choice {
    bool text = false;
    bool html = false;
    bool image = false;
    bool operator==(const Choice &) const = default;
};

/**
 * OPT-077, the combination rule. Decided from which formats exist, never from their contents (an offer is announced before
 * anything is read):
 *   - HTML is "rich" when there is HTML and also text, or HTML and no picture. Rich = HTML plus the plain text, offered
 *     together so the receiving application picks the richest it understands. The picture is dropped.
 *   - HTML with a picture and no text is the picture (a browser's "Copy image" offers <img> HTML and image/png): the
 *     picture wins and the HTML is not bridged.
 *   - Without rich HTML the earlier rules stand: text, and a picture beside it only as an additional announced format.
 * \a announce is what a sender lists; \a request is what a receiver asks for (a picture only when there is no text,
 * as in OPT-072).
 */
inline Choice announce(bool hasText, bool hasHtml, bool hasImage)
{
    Choice choice;
    choice.html = hasHtml && (hasText || !hasImage);
    choice.text = hasText;
    choice.image = hasImage && !choice.html;
    return choice;
}

inline Choice request(bool hasText, bool hasHtml, bool hasImage)
{
    Choice choice = announce(hasText, hasHtml, hasImage);
    choice.image = choice.image && !hasText;
    return choice;
}
}
