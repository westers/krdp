// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "ClipboardText.h"

using namespace Qt::StringLiterals;
using namespace KRdp::ClipboardText;

class ClipboardTextTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void hostConvertsCrlfToLf()
    {
        QCOMPARE(toHost(u"line one\r\nline two"_s), u"line one\nline two"_s);
        QCOMPARE(toHost(u"a\r\nb\r\n"_s), u"a\nb\n"_s);
        QCOMPARE(toHost(u"\r\n"_s), u"\n"_s);
    }

    void hostKeepsLfAndLoneCr()
    {
        QCOMPARE(toHost(u"already\nlf"_s), u"already\nlf"_s);
        QCOMPARE(toHost(u"lone\rcr"_s), u"lone\rcr"_s);
        QCOMPARE(toHost(u"trailing\r"_s), u"trailing\r"_s);
        // A CR that is not part of a CRLF pair survives next to one that is.
        QCOMPARE(toHost(u"a\r\r\nb"_s), u"a\r\nb"_s);
        QCOMPARE(toHost(QString()), QString());
        QCOMPARE(toHost(u"no newline"_s), u"no newline"_s);
    }

    void wireConvertsLfToCrlf()
    {
        QCOMPARE(toWire(u"line one\nline two"_s), u"line one\r\nline two"_s);
        QCOMPARE(toWire(u"a\nb\n"_s), u"a\r\nb\r\n"_s);
        QCOMPARE(toWire(u"\n"_s), u"\r\n"_s);
        QCOMPARE(toWire(u"\n\n"_s), u"\r\n\r\n"_s);
    }

    void wireKeepsExistingCrlfAndLoneCr()
    {
        // Host text that already carries CRLF must not become CR CR LF.
        QCOMPARE(toWire(u"dos\r\nfile"_s), u"dos\r\nfile"_s);
        QCOMPARE(toWire(u"mixed\r\none\ntwo"_s), u"mixed\r\none\r\ntwo"_s);
        QCOMPARE(toWire(u"lone\rcr"_s), u"lone\rcr"_s);
        QCOMPARE(toWire(u"trailing\r"_s), u"trailing\r"_s);
        QCOMPARE(toWire(QString()), QString());
        QCOMPARE(toWire(u"no newline"_s), u"no newline"_s);
    }

    void roundTripIsStableForLfText()
    {
        const auto host = u"one\ntwo\nthree"_s;
        QCOMPARE(toHost(toWire(host)), host);
        const auto wire = u"one\r\ntwo\r\nthree"_s;
        QCOMPARE(toWire(toHost(wire)), wire);
    }
};

QTEST_GUILESS_MAIN(ClipboardTextTest)

#include "ClipboardTextTest.moc"
