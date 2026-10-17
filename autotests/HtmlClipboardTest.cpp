// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "ClipboardHtml.h"

using namespace KRdp::ClipboardHtml;
using namespace Qt::StringLiterals;

namespace
{
QByteArray num(int v, int width = 10)
{
    return QByteArray::number(v).rightJustified(width, '0');
}

/// A header the way Windows / Word writes it, offsets computed from \a doc.
QByteArray windows(const QByteArray &doc, const QByteArray &eol = "\r\n", const QByteArray &extra = {})
{
    const QByteArray s = QByteArrayLiteral("<!--StartFragment-->");
    const QByteArray e = QByteArrayLiteral("<!--EndFragment-->");
    const int fragStart = doc.indexOf(s) + s.size();
    const int fragEnd = doc.indexOf(e);
    QByteArray header;
    // Two passes: the header length does not depend on the digits (ten each).
    for (int pass = 0; pass < 2; ++pass) {
        const int base = header.size();
        header = "Version:0.9" + eol + "StartHTML:" + num(base ? base : 0) + eol;
        header += "EndHTML:" + num(base + doc.size()) + eol + "StartFragment:" + num(base + fragStart) + eol + "EndFragment:" + num(base + fragEnd) + eol + extra;
        if (pass == 0) {
            const int length = header.size();
            header = "Version:0.9" + eol + "StartHTML:" + num(length) + eol + "EndHTML:" + num(length + doc.size()) + eol + "StartFragment:" + num(length + fragStart) + eol
                + "EndFragment:" + num(length + fragEnd) + eol + extra;
        }
    }
    return header + doc;
}

const QByteArray Doc = "<html><body>\r\n<!--StartFragment--><b>bold</b> text<!--EndFragment-->\r\n</body></html>";
}


namespace
{
// OPT-077 fix fixtures. LibreOffice Writer 26.2 text/html for a 3-line selection (captured on Sol, 2026-10-17): no fragment markers.
const QByteArray WriterHtml = QByteArrayLiteral(
    "<!DOCTYPE html>\n<html>\n<head>\n\t<meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\"/>\n\t<title></title>\n"
    "\t<meta name=\"generator\" content=\"LibreOffice 26.2.6.3 (Linux)\"/>\n\t<style type=\"text/css\">\n"
    "\t\t@page { size: 8.27in 11.69in; margin: 0.79in }\n\t\tp { margin-bottom: 0.1in; line-height: 115%; background: transparent }\n\t</style>\n</head>\n"
    "<body lang=\"en-US\" link=\"#000080\" vlink=\"#800000\" dir=\"ltr\"><p style=\"line-height: 100%; margin-bottom: 0in\">\n<b>Dsfsd sdfsd sdfsdf</b></p>\n"
    "<p style=\"line-height: 100%; margin-bottom: 0in\"><i>Dsfsd sdfsd sdfsdf</i></p>\n<p style=\"line-height: 100%; margin-bottom: 0in\">Dsfsd sdfsd sdfsdf</p>\n</body>\n</html>");
// Chrome: markers are there, a bare span fragment inside <body>.
const QByteArray ChromeHtml = QByteArrayLiteral(
    "<html><body>\n<!--StartFragment--><span style=\"color: rgb(0, 0, 0);\">D\xC3\xA4rf \xE2\x82\xAC sdfsd</span><br><span>Dsfsd sdfsd sdfsdf</span><!--EndFragment-->\n</body></html>");

qint64 headerValue(const QByteArray &wire, const char *key)
{
    const qsizetype at = wire.indexOf(QByteArray(QByteArray(key) + ":"));
    return at < 0 ? -1 : wire.mid(at + qstrlen(key) + 1, 10).toLongLong();
}
int count(const QByteArray &haystack, const QByteArray &needle)
{
    int n = 0;
    for (qsizetype i = haystack.indexOf(needle); i >= 0; i = haystack.indexOf(needle, i + needle.size())) ++n;
    return n;
}
}

class HtmlClipboardTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // ---- unwrap: what real senders write ----
    void unwrapsAWindowsHeader()
    {
        const auto result = unwrapMsHtml(windows(Doc) + '\0');
        QVERIFY(result.ok());
        QVERIFY(result.data.contains("<!--StartFragment--><b>bold</b> text<!--EndFragment-->"));
        QVERIFY(result.data.contains("charset"));
    }

    void unwrapsLfOnlyHeader()
    {
        const auto result = unwrapMsHtml(windows(Doc, "\n"));
        QVERIFY(result.ok());
        QVERIFY(result.data.contains("<b>bold</b>"));
    }

    void unwrapsShortAndUnpaddedOffsets()
    {
        // Offsets written without zero padding (Chrome on Windows writes ten digits, Firefox does not always).
        const QByteArray body = "<html><body><!--StartFragment-->x<!--EndFragment--></body></html>";
        QByteArray header = "Version:1.0\r\nStartHTML:0\r\nEndHTML:0\r\nStartFragment:0\r\nEndFragment:0\r\n";
        const int length = header.size();
        header = "Version:1.0\r\nStartHTML:" + QByteArray::number(length) + "\r\nEndHTML:" + QByteArray::number(length + body.size())
            + "\r\nStartFragment:" + QByteArray::number(length + 40) + "\r\nEndFragment:" + QByteArray::number(length + 41) + "\r\n";
        // Same length class as the placeholder (all numbers are 2-3 digits), so recompute until stable.
        for (int i = 0; i < 3; ++i) {
            const int l = header.size();
            header = "Version:1.0\r\nStartHTML:" + QByteArray::number(l) + "\r\nEndHTML:" + QByteArray::number(l + body.size())
                + "\r\nStartFragment:" + QByteArray::number(l + 40) + "\r\nEndFragment:" + QByteArray::number(l + 41) + "\r\n";
        }
        const auto result = unwrapMsHtml(header + body);
        QVERIFY2(result.ok(), errorName(result.error));
        QVERIFY(result.data.contains("<body>"));
    }

    void unwrapsExtraKeysAndMixedCaseKeys()
    {
        const QByteArray extra = "SourceURL:https://example.org/page\r\n";
        QByteArray header = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\nStartFragment:0000000000\r\nEndFragment:0000000000\r\n" + extra;
        const int l = header.size();
        header = "Version:0.9\r\nstarthtml:" + num(l) + "\r\nEndHtml:" + num(l + Doc.size()) + "\r\nStartFragment:" + num(l + 40) + "\r\nEndFragment:" + num(l + 50) + "\r\n" + extra;
        QCOMPARE(header.size() - extra.size() + extra.size(), l);
        const auto result = unwrapMsHtml(header + Doc);
        QVERIFY2(result.ok(), errorName(result.error));
        QVERIFY(result.data.contains("<b>bold</b>"));
        // "Version:" must come first; a lowercase one is not a header and the text does not start with '<'.
        QCOMPARE(unwrapMsHtml("version:0.9\r\n" + header.mid(13) + Doc).error, Error::Malformed);
    }

    void missingEndHtmlRunsToTheEnd()
    {
        const QByteArray body = "<html><body>hi</body></html>";
        const QByteArray header = "Version:0.9\r\nStartHTML:0000000040\r\n";
        // the header is 13 + 22 = 35 bytes, so StartHTML is 35.
        const QByteArray fixed = "Version:0.9\r\nStartHTML:0000000035\r\n";
        QCOMPARE(fixed.size(), 35);
        const auto result = unwrapMsHtml(fixed + body + '\0');
        QVERIFY2(result.ok(), errorName(result.error));
        QVERIFY(result.data.contains("<html><body>hi</body></html>"));
        QVERIFY(!result.data.contains('\0'));
        Q_UNUSED(header);
    }

    void minusOneEndHtmlIsAbsent()
    {
        const QByteArray body = "<p>x</p>";
        const QByteArray header = "Version:1.0\r\nStartHTML:0000000047\r\nEndHTML:-1\r\n";
        QCOMPARE(header.size(), 47);
        const auto result = unwrapMsHtml(header + body);
        QVERIFY2(result.ok(), errorName(result.error));
        QVERIFY(result.data.contains("<p>x</p>"));
    }

    void fragmentOnlyOffsets()
    {
        const QByteArray header = "Version:0.9\r\nStartFragment:0000000063\r\nEndFragment:0000000073\r\n";
        QCOMPARE(header.size(), 63);
        const auto result = unwrapMsHtml(header + "<b>abc</b>");
        QVERIFY2(result.ok(), errorName(result.error));
        QVERIFY(result.data.contains("<b>abc</b>"));
    }

    void headerlessHtmlIsAccepted()
    {
        const auto result = unwrapMsHtml("  \r\n<b>raw</b>\0"_ba);
        QVERIFY(result.ok());
        QVERIFY(result.data.contains("<b>raw</b>"));
        QCOMPARE(unwrapMsHtml("plain words"_ba).error, Error::Malformed);
    }

    void endHtmlCountingTheTerminatorIsClamped()
    {
        // A sender whose EndHTML includes the NUL: the document still ends at the last real byte.
        const QByteArray doc = "<p>t</p>";
        const QByteArray header = "Version:0.9\r\nStartHTML:0000000055\r\nEndHTML:0000000064\r\n";
        QCOMPARE(header.size(), 55);
        const auto result = unwrapMsHtml(header + doc + '\0');
        QVERIFY2(result.ok(), errorName(result.error));
        QVERIFY(result.data.contains("<p>t</p>"));
        QVERIFY(!result.data.contains('\0'));
    }

    // ---- unwrap: refusals ----
    void refusesInconsistentOffsets()
    {
        const QByteArray body = "<p>0123456789</p>";
        const auto make = [&](int sh, int eh) {
            return "Version:0.9\r\nStartHTML:" + num(sh) + "\r\nEndHTML:" + num(eh) + "\r\n" + body;
        };
        const int h = 13 + 22 + 20; // 55
        QVERIFY(unwrapMsHtml(make(h, h + body.size())).ok());
        QCOMPARE(unwrapMsHtml(make(h, h + body.size() + 100)).error, Error::Inconsistent); // past the data
        QCOMPARE(unwrapMsHtml(make(h + 5, h + 2)).error, Error::Inconsistent);           // start after end
        QCOMPARE(unwrapMsHtml(make(5, h + body.size())).error, Error::Inconsistent);      // inside the header
    }

    void refusesFragmentOutsideDocument()
    {
        const QByteArray body = "<html><body><!--StartFragment-->a<!--EndFragment--></body></html>";
        const int h = 13 + 22 + 20 + 26 + 24; // 105
        const auto make = [&](int sf, int ef) {
            return "Version:0.9\r\nStartHTML:" + num(h) + "\r\nEndHTML:" + num(h + body.size()) + "\r\nStartFragment:" + num(sf) + "\r\nEndFragment:" + num(ef) + "\r\n" + body;
        };
        QVERIFY(unwrapMsHtml(make(h + 32, h + 33)).ok());
        QCOMPARE(unwrapMsHtml(make(h - 1, h + 33)).error, Error::Inconsistent);
        QCOMPARE(unwrapMsHtml(make(h + 33, h + 32)).error, Error::Inconsistent);
        QCOMPARE(unwrapMsHtml(make(h + 32, h + body.size() + 5)).error, Error::Inconsistent);
    }

    void refusesBadNumbers()
    {
        QCOMPARE(unwrapMsHtml("Version:0.9\r\nStartHTML:abc\r\nEndHTML:0000000040\r\n<p>x</p>"_ba).error, Error::Malformed);
        QCOMPARE(unwrapMsHtml("Version:0.9\r\nStartHTML:00000000000000099\r\n<p>x</p>"_ba).error, Error::Malformed); // too many digits
        QCOMPARE(unwrapMsHtml("Version:0.9\r\nStartHTML:\r\n<p>x</p>"_ba).error, Error::Malformed);
        QCOMPARE(unwrapMsHtml("Version:0.9\r\nStartHTML:-\r\n<p>x</p>"_ba).error, Error::Malformed);
        QCOMPARE(unwrapMsHtml("Version:0.9\r\nSourceURL:x\r\n<p>x</p>"_ba).error, Error::Malformed); // no usable offset
    }

    void refusesEmptyAndOverlong()
    {
        QCOMPARE(unwrapMsHtml({}).error, Error::Empty);
        QCOMPARE(unwrapMsHtml(QByteArray(10, '\0')).error, Error::Empty);
        QCOMPARE(unwrapMsHtml("   \r\n  "_ba).error, Error::Empty);
        QCOMPARE(unwrapMsHtml(QByteArray(MaxWireBytes + 1, '<')).error, Error::TooLarge);
        // Declared and real length both over the document cap but under the wire cap.
        const QByteArray big = "<p>" + QByteArray(MaxHtmlBytes, 'a') + "</p>";
        QCOMPARE(unwrapMsHtml("Version:0.9\r\nStartHTML:0000000035\r\n"_ba + big).error, Error::TooLarge);
        QVERIFY(unwrapMsHtml("Version:0.9\r\nStartHTML:0000000035\r\n"_ba + "<p>" + QByteArray(1000, 'a') + "</p>").ok());
    }

    void hostileHeadersDoNotCrash()
    {
        // Offsets near the 32-bit and 64-bit edges, a header with no line break, a very long first line.
        QCOMPARE(unwrapMsHtml("Version:0.9\r\nStartHTML:4294967295\r\nEndHTML:4294967296\r\n<p>x</p>"_ba).error, Error::Inconsistent);
        QCOMPARE(unwrapMsHtml("Version:0.9"_ba).error, Error::Malformed);
        QVERIFY(!unwrapMsHtml("Version:" + QByteArray(100000, '9')).ok());
        QVERIFY(!unwrapMsHtml(QByteArray("Version:0.9\r\n") + QByteArray(5000, 'k') + ":1\r\n<p>x</p>").ok());
        for (int i = 0; i < 300; ++i) {
            QByteArray junk;
            for (int j = 0; j < 80; ++j) junk.append(char((i * 31 + j * 17) & 0xFF));
            unwrapMsHtml("Version:0.9\r\nStartHTML:0000000040\r\n" + junk);
            unwrapMsHtml(junk);
        }
    }

    void outputIsValidUtf8()
    {
        const QByteArray body = "<p>caf\xC3\xA9 \xFF\xFE bad \xE2\x82</p>"; // valid, then invalid bytes, then a truncated sequence
        const auto result = unwrapMsHtml("Version:0.9\r\nStartHTML:0000000035\r\n"_ba + body);
        QVERIFY(result.ok());
        QVERIFY(QByteArrayView(result.data).isValidUtf8());
        QVERIFY(result.data.contains("caf\xC3\xA9"));
        QVERIFY(!result.data.contains('\0'));
        const auto nuls = unwrapMsHtml("Version:0.9\r\nStartHTML:0000000037\r\n<p>a\0b</p>"_ba);
        QVERIFY(nuls.ok());
        QVERIFY(!nuls.data.contains('\0'));
    }

    void charsetIsAddedOnceAndInsideHead()
    {
        QVERIFY(unwrapMsHtml("<p>x</p>"_ba).data.startsWith("<meta"));
        const auto head = unwrapMsHtml("<html><head><title>t</title></head><body>b</body></html>"_ba);
        QVERIFY(head.data.startsWith("<html><head><meta"));
        const auto already = unwrapMsHtml("<html><head><meta charset=\"utf-8\"></head></html>"_ba);
        QCOMPARE(already.data.count("charset"), 1);
    }

    // ---- wrap ----
    void wrapProducesExactOffsets()
    {
        const auto wrapped = wrapForWire("<b>bold</b> and plain"_ba);
        QVERIFY(wrapped.ok());
        QVERIFY(wrapped.data.endsWith('\0'));
        QVERIFY(wrapped.data.startsWith("Version:0.9\r\nStartHTML:0000000105\r\nEndHTML:"));
        const QByteArray w = wrapped.data;
        const auto field = [&](const char *name) {
            const int at = w.indexOf(QByteArray(name).append(":")) + int(qstrlen(name)) + 1;
            return w.mid(at, 10).toInt();
        };
        QCOMPARE(field("StartHTML"), 105);
        QCOMPARE(field("EndHTML"), int(w.size() - 1)); // everything but the NUL
        QCOMPARE(w.mid(105, 12), QByteArray("<html><body>"));
        // Fragment offsets point just after the start marker and at the end marker.
        QCOMPARE(w.mid(field("StartFragment") - 20, 20), QByteArray("<!--StartFragment-->"));
        QCOMPARE(w.mid(field("EndFragment"), 18), QByteArray("<!--EndFragment-->"));
        QCOMPARE(w.mid(field("StartFragment"), field("EndFragment") - field("StartFragment")), QByteArray("<b>bold</b> and plain"));
    }

    void wrapKeepsExistingMarkersAndBody()
    {
        const auto keeps = wrapForWire(Doc);
        QVERIFY(keeps.ok());
        QCOMPARE(keeps.data.count("StartFragment-->"), 1);
        QCOMPARE(keeps.data.mid(105, Doc.size()), Doc);
        const auto body = wrapForWire("<!DOCTYPE html><html><head></head><BODY class=x>hello</BODY></html>"_ba);
        QVERIFY(body.ok());
        QVERIFY(body.data.contains("<BODY class=x><!--StartFragment-->hello<!--EndFragment--></BODY>"));
    }

    void wrapRoundTripsThroughUnwrap()
    {
        const QList<QPair<QByteArray, QByteArray>> cases = {
            {"<b>x</b>", "<b>x</b>"},
            {Doc, "<!--StartFragment--><b>bold</b> text<!--EndFragment-->"},
            {"<p>caf\xC3\xA9 \xE2\x82\xAC</p>", "<p>caf\xC3\xA9 \xE2\x82\xAC</p>"},
            {"<html><body>one<!--StartFragment-->two<!--EndFragment-->three</body></html>", "one<!--StartFragment-->two<!--EndFragment-->three"},
        };
        for (const auto &[html, needle] : cases) {
            const auto wrapped = wrapForWire(html);
            QVERIFY(wrapped.ok());
            const auto back = unwrapMsHtml(wrapped.data);
            QVERIFY2(back.ok(), errorName(back.error));
            QVERIFY(QByteArrayView(back.data).isValidUtf8());
            QVERIFY2(back.data.contains(needle), html.constData());
        }
    }

    void wrapRefusesEmptyAndOversize()
    {
        QCOMPARE(wrapForWire({}).error, Error::Empty);
        QCOMPARE(wrapForWire("  \n"_ba).error, Error::Empty);
        QCOMPARE(wrapForWire(QByteArray(MaxHtmlBytes + 1, 'a')).error, Error::TooLarge);
        QVERIFY(wrapForWire(QByteArray(MaxHtmlBytes - 200, 'a')).ok());
        // The wrapper's own markup and header must not push a document at the cap over the wire cap.
        const auto atCap = wrapForWire(QByteArray(MaxHtmlBytes, 'a'));
        QVERIFY(atCap.ok());
        QVERIFY(atCap.data.size() <= MaxWireBytes);
    }

    // ---- OPT-077 fix: LibreOffice Writer and Chrome fixtures through the wrapper, exact offsets, no duplicated content ----
    void realWorldFixturesWrapWithExactOffsets_data()
    {
        QTest::addColumn<QByteArray>("html");
        QTest::newRow("writer") << WriterHtml;
        QTest::newRow("chrome") << ChromeHtml;
    }
    void realWorldFixturesWrapWithExactOffsets()
    {
        QFETCH(QByteArray, html);
        const auto wrapped = wrapForWire(html);
        QVERIFY(wrapped.ok());
        const QByteArray &wire = wrapped.data;
        QVERIFY(wire.endsWith('\0'));
        QCOMPARE(headerValue(wire, "StartHTML"), qint64(105));
        const qint64 startHtml = headerValue(wire, "StartHTML");
        const qint64 endHtml = headerValue(wire, "EndHTML");
        const qint64 startFragment = headerValue(wire, "StartFragment");
        const qint64 endFragment = headerValue(wire, "EndFragment");
        QCOMPARE(endHtml, qint64(wire.size() - 1)); // up to, not including, the NUL
        QCOMPARE(wire.at(startHtml), '<');
        QVERIFY(wire.mid(startFragment - 20, 20) == "<!--StartFragment-->");
        QVERIFY(wire.mid(endFragment, 18) == "<!--EndFragment-->");
        // exactly one pair of markers, and the content once
        QCOMPARE(count(wire, "<!--StartFragment-->"), 1);
        QCOMPARE(count(wire, "<!--EndFragment-->"), 1);
        QCOMPARE(count(wire, "Dsfsd sdfsd sdfsdf"), count(html, "Dsfsd sdfsd sdfsdf"));
        // the fragment is what is between the markers, nothing of the head
        const QByteArray fragment = wire.mid(startFragment, endFragment - startFragment);
        QVERIFY(!fragment.contains("<head"));
        QVERIFY(!fragment.contains("<style"));
    }
    void realWorldFixturesUnwrapToTheSameDocumentOnceOver_data()
    {
        QTest::addColumn<QByteArray>("html");
        QTest::newRow("writer") << WriterHtml;
        QTest::newRow("chrome") << ChromeHtml;
    }
    void realWorldFixturesUnwrapToTheSameDocumentOnceOver()
    {
        QFETCH(QByteArray, html);
        const auto once = unwrapMsHtml(wrapForWire(html).data);
        QVERIFY(once.ok());
        // the content appears exactly as often as in the source (no StartHTML..EndHTML plus fragment doubling)
        QCOMPARE(count(once.data, "Dsfsd sdfsd sdfsdf"), count(html, "Dsfsd sdfsd sdfsdf"));
        QCOMPARE(count(once.data, "<!--StartFragment-->"), 1);
        // a second trip (client -> host -> client -> host) changes nothing
        const auto wire = wrapForWire(normaliseHostHtml(once.data));
        const auto twice = unwrapMsHtml(wire.data);
        QCOMPARE(twice.data, once.data);
        QCOMPARE(wrapForWire(normaliseHostHtml(twice.data)).data, wire.data);
    }

    void wrapOutputIsValidUtf8WithoutNul()
    {
        const auto wrapped = wrapForWire("<p>a\xFF" "b\0c</p>"_ba);
        QVERIFY(wrapped.ok());
        QVERIFY(QByteArrayView(wrapped.data.left(wrapped.data.size() - 1)).isValidUtf8());
        QVERIFY(!wrapped.data.left(wrapped.data.size() - 1).contains('\0'));
    }

    // ---- host html (what a Wayland application offers) ----
    void normalisesUtf16AndUtf8()
    {
        const QString text = QStringLiteral("<p>café €</p>");
        QByteArray le("\xFF\xFE", 2);
        le.append(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
        QCOMPARE(normaliseHostHtml(le), text.toUtf8());
        QByteArray be("\xFE\xFF", 2);
        for (const QChar c : text) {
            be.append(char(c.unicode() >> 8));
            be.append(char(c.unicode() & 0xFF));
        }
        QCOMPARE(normaliseHostHtml(be), text.toUtf8());
        QByteArray bare(reinterpret_cast<const char *>(text.utf16()), text.size() * 2); // UTF-16LE, no BOM
        QCOMPARE(normaliseHostHtml(bare), text.toUtf8());
        QCOMPARE(normaliseHostHtml("\xEF\xBB\xBF<p>x</p>"_ba), QByteArray("<p>x</p>"));
        QVERIFY(QByteArrayView(normaliseHostHtml("<p>\xFF</p>"_ba)).isValidUtf8());
        QVERIFY(!normaliseHostHtml("<p>a\0b</p>"_ba).contains('\0'));
        QCOMPARE(normaliseHostHtml({}), QByteArray());
    }

    // ---- the combination rule ----
    void combinationRule_data()
    {
        QTest::addColumn<bool>("text");
        QTest::addColumn<bool>("html");
        QTest::addColumn<bool>("image");
        QTest::addColumn<bool>("announceText");
        QTest::addColumn<bool>("announceHtml");
        QTest::addColumn<bool>("announceImage");
        QTest::addColumn<bool>("requestImage");
        //                                          t      h      i      aT     aH     aI     rI
        QTest::newRow("text only")              << true  << false << false << true  << false << false << false;
        QTest::newRow("html only")              << false << true  << false << false << true  << false << false;
        QTest::newRow("html and text")          << true  << true  << false << true  << true  << false << false;
        QTest::newRow("image only")             << false << false << true  << false << false << true  << true;
        QTest::newRow("text and image")         << true  << false << true  << true  << false << true  << false;
        QTest::newRow("chrome copy image")      << false << true  << true  << false << false << true  << true; // html <img> + png: the picture wins
        QTest::newRow("html text image")        << true  << true  << true  << true  << true  << false << false; // a spreadsheet range: rich, picture dropped
        QTest::newRow("nothing")                << false << false << false << false << false << false << false;
    }

    void combinationRule()
    {
        QFETCH(bool, text);
        QFETCH(bool, html);
        QFETCH(bool, image);
        QFETCH(bool, announceText);
        QFETCH(bool, announceHtml);
        QFETCH(bool, announceImage);
        QFETCH(bool, requestImage);
        const Choice a = announce(text, html, image);
        QCOMPARE(a.text, announceText);
        QCOMPARE(a.html, announceHtml);
        QCOMPARE(a.image, announceImage);
        QCOMPARE(request(text, html, image).image, requestImage);
        QCOMPARE(request(text, html, image).html, announceHtml);
    }

    void formatNameMatchesCaseInsensitively()
    {
        QVERIFY(isHtmlFormatName(QStringLiteral("HTML Format")));
        QVERIFY(isHtmlFormatName(QStringLiteral("html format")));
        QVERIFY(!isHtmlFormatName(QStringLiteral("HTML")));
        QVERIFY(!isHtmlFormatName(QString()));
    }
};

QTEST_GUILESS_MAIN(HtmlClipboardTest)
#include "HtmlClipboardTest.moc"
