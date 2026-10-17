// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-077 fix: the QMimeData a client's rich copy becomes on the host (plain text beside text/html), daemon-free.

#include <QMimeData>
#include <QTest>

#include "ClipboardMime.h"

using namespace KRdp;

class ClipboardMimeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void offersTheSendersPlainTextBesideTheHtml()
    {
        const QString text = QStringLiteral("Dsfsd sdfsd sdfsdf\nDsfsd sdfsd sdfsdf\nDsfsd sdfsd sdfsdf");
        const QByteArray html = "<html><body><!--StartFragment--><p><b>Dsfsd sdfsd sdfsdf</b></p><!--EndFragment--></body></html>";
        std::unique_ptr<QMimeData> mime(buildClipboardMime(text, html, false));
        QVERIFY(mime->formats().contains(QStringLiteral("text/plain")));
        QVERIFY(mime->formats().contains(QStringLiteral("text/html")));
        QCOMPARE(mime->text(), text);
        QCOMPARE(mime->data(QStringLiteral("text/plain")), text.toUtf8()); // what Paste Special -> Unformatted text receives
        QCOMPARE(mime->data(QStringLiteral("text/html")), html);
        QVERIFY(!mime->data(QStringLiteral("text/plain")).contains('<')); // nothing of the markup in the plain text
        QVERIFY(!mime->data(QStringLiteral("text/plain")).contains('\0'));
        QVERIFY(!mime->hasFormat(QStringLiteral("x-kde-passwordManagerHint")));
    }
    void nonAsciiTextIsExactUtf8()
    {
        const QString text = QStringLiteral("D\u00e4rf \u00fcber \u20ac\n\u65e5\u672c\u8a9e");
        std::unique_ptr<QMimeData> mime(buildClipboardMime(text, "<p>x</p>", false));
        QCOMPARE(mime->data(QStringLiteral("text/plain")), text.toUtf8());
        QCOMPARE(mime->text(), text);
    }
    void htmlAloneAndTextAlone()
    {
        std::unique_ptr<QMimeData> htmlOnly(buildClipboardMime({}, "<p>x</p>", false));
        QVERIFY(!htmlOnly->hasFormat(QStringLiteral("text/plain")));
        QVERIFY(htmlOnly->hasFormat(QStringLiteral("text/html")));
        std::unique_ptr<QMimeData> textOnly(buildClipboardMime(QStringLiteral("abc"), {}, false));
        QVERIFY(!textOnly->hasFormat(QStringLiteral("text/html")));
        QCOMPARE(textOnly->text(), QStringLiteral("abc"));
    }
    void aGreeterOrLockedWriteAsksForNoHistory()
    {
        std::unique_ptr<QMimeData> mime(buildClipboardMime(QStringLiteral("abc"), "<p>abc</p>", true));
        QCOMPARE(mime->data(QStringLiteral("x-kde-passwordManagerHint")), QByteArrayLiteral("secret"));
        QCOMPARE(mime->text(), QStringLiteral("abc"));
    }
};

QTEST_GUILESS_MAIN(ClipboardMimeTest)
#include "ClipboardMimeTest.moc"
