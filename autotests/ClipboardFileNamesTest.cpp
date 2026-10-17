// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 1: the clipboard file-name sanitizer. Hostile names, collisions, limits. Pure (no display, no I/O).

#include <QElapsedTimer>
#include <QTest>

#include "ClipboardFileNames.h"

using namespace KRdp::ClipboardFileNames;
namespace Limits = KRdp::ClipboardFileLimits;

Q_DECLARE_METATYPE(KRdp::ClipboardFileNames::Reject)

class ClipboardFileNamesTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void plainPathsAreKept()
    {
        const auto r = sanitize({{QStringLiteral("a.txt"), false}, {QStringLiteral("dir"), true}, {QStringLiteral("dir\\b.txt"), false}, {QStringLiteral("dir\\sub\\c"), false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries.size(), 4);
        QCOMPARE(r.entries[0].relativePath, QStringLiteral("a.txt"));
        QCOMPARE(r.entries[2].relativePath, QStringLiteral("dir/b.txt"));
        QCOMPARE(r.entries[3].components, (QStringList{QStringLiteral("dir"), QStringLiteral("sub"), QStringLiteral("c")}));
        QVERIFY(r.entries[1].isDirectory);
        for (const auto &e : r.entries) QVERIFY(!e.renamed);
    }
    void emptyListIsFine()
    {
        const auto r = sanitize({});
        QVERIFY(r.ok());
        QVERIFY(r.entries.isEmpty());
    }

    void hostileRefusesTheWholeTransfer_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<Reject>("reject");
        QTest::newRow("empty") << QString() << Reject::EmptyComponent;
        QTest::newRow("dot") << QStringLiteral(".") << Reject::DotComponent;
        QTest::newRow("dotdot") << QStringLiteral("..") << Reject::DotComponent;
        QTest::newRow("dotdot first") << QStringLiteral("..\\..\\etc\\passwd") << Reject::DotComponent;
        QTest::newRow("dotdot middle") << QStringLiteral("a\\..\\b") << Reject::DotComponent;
        QTest::newRow("dot middle") << QStringLiteral("a\\.\\b") << Reject::DotComponent;
        QTest::newRow("dotdot last") << QStringLiteral("a\\..") << Reject::DotComponent;
        QTest::newRow("empty middle (double backslash)") << QStringLiteral("a\\\\b") << Reject::EmptyComponent;
        QTest::newRow("trailing backslash") << QStringLiteral("a\\") << Reject::EmptyComponent;
        QTest::newRow("absolute backslash") << QStringLiteral("\\etc\\passwd") << Reject::AbsolutePath;
        QTest::newRow("absolute slash") << QStringLiteral("/etc/passwd") << Reject::AbsolutePath;
        QTest::newRow("unc") << QStringLiteral("\\\\server\\share\\f") << Reject::UncPath;
        QTest::newRow("unc slashes") << QStringLiteral("//server/share") << Reject::UncPath;
        QTest::newRow("unc mixed") << QStringLiteral("\\/server") << Reject::UncPath;
        QTest::newRow("drive") << QStringLiteral("C:\\Windows\\x") << Reject::DriveLetter;
        QTest::newRow("drive relative") << QStringLiteral("c:foo") << Reject::DriveLetter;
        QTest::newRow("drive lower") << QStringLiteral("z:\\") << Reject::DriveLetter;
        QTest::newRow("stream") << QStringLiteral("file.txt:hidden") << Reject::StreamName;
        QTest::newRow("stream default") << QStringLiteral("file.txt::$DATA") << Reject::StreamName;
        QTest::newRow("colon in directory") << QStringLiteral("ab:b\\c") << Reject::StreamName;
        QTest::newRow("single letter then colon is a drive") << QStringLiteral("a:b\\c") << Reject::DriveLetter;
        QTest::newRow("nul") << QString(QStringLiteral("a") + QChar(0) + QStringLiteral("b")) << Reject::Nul;
        QTest::newRow("tab") << QStringLiteral("a\tb") << Reject::ControlCharacter;
        QTest::newRow("newline") << QStringLiteral("a\nb") << Reject::ControlCharacter;
        QTest::newRow("esc") << QStringLiteral("a\x1b[31m") << Reject::ControlCharacter;
        QTest::newRow("del") << QString(QStringLiteral("a") + QChar(0x7f)) << Reject::ControlCharacter;
        QTest::newRow("c1") << QString(QStringLiteral("a") + QChar(0x85)) << Reject::ControlCharacter;
        QTest::newRow("slash in component") << QStringLiteral("a/b") << Reject::SlashInComponent;
        QTest::newRow("slash after dir") << QStringLiteral("a\\b/c") << Reject::SlashInComponent;
        QTest::newRow("component 256 bytes") << QString(256, QLatin1Char('a')) << Reject::ComponentTooLong;
        QTest::newRow("component 3-byte chars over 255 bytes") << QString(86, QChar(0x20ac)) << Reject::ComponentTooLong; // 258 bytes
        QTest::newRow("bidi in extension") << QString(QStringLiteral("doc.t") + QChar(0x202e) + QStringLiteral("xt")) << Reject::FormatInExtension;
        QTest::newRow("zero width in extension") << QString(QStringLiteral("a.e") + QChar(0x200b) + QStringLiteral("xe")) << Reject::FormatInExtension;
        QTest::newRow("unpaired surrogate in extension") << QString(QStringLiteral("a.x") + QChar(0xd800)) << Reject::FormatInExtension;
        QTest::newRow("deep") << QStringList(QList<QString>(65, QStringLiteral("d"))).join(QLatin1Char('\\')) << Reject::TooDeep;
    }
    void hostileRefusesTheWholeTransfer()
    {
        QFETCH(QString, path);
        QFETCH(Reject, reject);
        // Hidden among good entries: the whole transfer goes.
        const auto r = sanitize({{QStringLiteral("good.txt"), false}, {path, false}, {QStringLiteral("good2.txt"), false}});
        QVERIFY(!r.ok());
        QCOMPARE(*r.reject, reject);
        QVERIFY(r.entries.isEmpty());
        QVERIFY(qstrlen(rejectName(reject)) > 0);
    }

    void longButLegalNamesAreKept()
    {
        QVERIFY(sanitize({{QString(255, QLatin1Char('a')), false}}).ok());
        QVERIFY(sanitize({{QString(85, QChar(0x20ac)), false}}).ok()); // 255 bytes
        const QString depth64 = QStringList(QList<QString>(64, QStringLiteral("d"))).join(QLatin1Char('\\'));
        QVERIFY(sanitize({{depth64, false}}).ok());
    }
    void pathOver4096BytesIsRefused()
    {
        QStringList parts;
        for (int i = 0; i < 17; ++i) parts.append(QString(250, QLatin1Char('a')));
        const auto r = sanitize({{parts.join(QLatin1Char('\\')), false}});
        QVERIFY(!r.ok());
        QCOMPARE(*r.reject, Reject::PathTooLong);
    }
    void tooManyEntriesIsRefusedWithoutWork()
    {
        QList<RawEntry> many(qsizetype(Limits::MaxEntries) + 1, RawEntry{QStringLiteral("x"), false});
        const auto r = sanitize(many);
        QVERIFY(!r.ok());
        QCOMPARE(*r.reject, Reject::TooManyEntries);
    }
    void maxEntriesWithUniqueNamesWorks()
    {
        QList<RawEntry> list;
        for (quint32 i = 0; i < Limits::MaxEntries; ++i) list.append({QString::number(i), false});
        const auto r = sanitize(list);
        QVERIFY(r.ok());
        QCOMPARE(r.entries.size(), qsizetype(Limits::MaxEntries));
    }
    void totalPathBytesAreCapped()
    {
        // 70 paths of ~1 MiB would need 100k entries; use long names x many entries within the entry cap
        QList<RawEntry> list;
        const QString longName(240, QLatin1Char('n'));
        for (int i = 0; i < 100000; ++i) list.append({QString::number(i) + longName.left(240 - QString::number(i).size()) + QStringLiteral("\\") + longName, false});
        // 100000 * ~485 bytes = 48 MB > 32 MiB
        const auto r = sanitize(list);
        QVERIFY(!r.ok());
        QCOMPARE(*r.reject, Reject::TotalPathBytes);
    }

    void formatCharactersOutsideTheExtensionBecomeUnderscore()
    {
        const auto r = sanitize({{QString(QStringLiteral("inv") + QChar(0x202e) + QStringLiteral("fdp.exe")), false}, {QString(QStringLiteral("a") + QChar(0x200b) + QStringLiteral("b")), false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries[0].relativePath, QStringLiteral("inv_fdp.exe"));
        QVERIFY(r.entries[0].renamed);
        QCOMPARE(r.entries[1].relativePath, QStringLiteral("a_b"));
    }
    void unpairedSurrogateBecomesUnderscoreOutsideTheExtension()
    {
        const auto r = sanitize({{QString(QStringLiteral("a") + QChar(0xdc00) + QStringLiteral("b.txt")), false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries[0].relativePath, QStringLiteral("a_b.txt"));
    }
    void pairedSurrogatesSurvive()
    {
        const QString emoji = QString::fromUcs4(U"photo\U0001F600.png");
        const auto r = sanitize({{emoji, false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries[0].relativePath, emoji);
        QVERIFY(!r.entries[0].renamed);
    }
    void nfcIsApplied()
    {
        const QString decomposed = QString::fromUtf8("e\xcc\x81.txt"); // e + combining acute
        const auto r = sanitize({{decomposed, false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries[0].relativePath, QString::fromUtf8("\xc3\xa9.txt"));
        QVERIFY(r.entries[0].renamed);
    }
    void leadingDotsAndSpacesStayOnLinuxMode()
    {
        const auto r = sanitize({{QStringLiteral(".hidden"), false}, {QStringLiteral("trail. "), false}, {QStringLiteral("CON"), false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries[1].relativePath, QStringLiteral("trail. "));
        QCOMPARE(r.entries[2].relativePath, QStringLiteral("CON"));
    }

    void windowsSafeModeRenames_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("CON") << QStringLiteral("CON") << QStringLiteral("_CON");
        QTest::newRow("con lower") << QStringLiteral("con") << QStringLiteral("_con");
        QTest::newRow("CON with extension") << QStringLiteral("CON.txt") << QStringLiteral("_CON.txt");
        QTest::newRow("PRN") << QStringLiteral("prn") << QStringLiteral("_prn");
        QTest::newRow("AUX") << QStringLiteral("Aux.log") << QStringLiteral("_Aux.log");
        QTest::newRow("NUL") << QStringLiteral("NUL") << QStringLiteral("_NUL");
        QTest::newRow("COM1") << QStringLiteral("COM1") << QStringLiteral("_COM1");
        QTest::newRow("com9 ext") << QStringLiteral("com9.dat") << QStringLiteral("_com9.dat");
        QTest::newRow("LPT3") << QStringLiteral("LPT3") << QStringLiteral("_LPT3");
        QTest::newRow("COM0 is fine") << QStringLiteral("COM0") << QStringLiteral("COM0");
        QTest::newRow("COM10 is fine") << QStringLiteral("COM10") << QStringLiteral("COM10");
        QTest::newRow("CONSOLE is fine") << QStringLiteral("CONSOLE") << QStringLiteral("CONSOLE");
        QTest::newRow("trailing dot") << QStringLiteral("file.") << QStringLiteral("file");
        QTest::newRow("trailing dots and spaces") << QStringLiteral("file. . ") << QStringLiteral("file");
        QTest::newRow("only dots") << QStringLiteral("...") << QStringLiteral("_");
        QTest::newRow("only spaces") << QStringLiteral("   ") << QStringLiteral("_");
        QTest::newRow("8.3 alias") << QStringLiteral("PROGRA~1") << QStringLiteral("PROGRA_1");
        QTest::newRow("8.3 alias ext") << QStringLiteral("abc~12.txt") << QStringLiteral("abc_12.txt");
        QTest::newRow("tilde in a long name stays") << QStringLiteral("my~notes.txt") << QStringLiteral("my~notes.txt");
        QTest::newRow("illegal characters") << QStringLiteral("a<b>c\"d|e?f*g") << QStringLiteral("a_b_c_d_e_f_g");
    }
    void windowsSafeModeRenames()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        Options options;
        options.windowsSafe = true;
        const auto r = sanitize({{input, false}}, options);
        QVERIFY(r.ok());
        QCOMPARE(r.entries[0].relativePath, expected);
    }
    void windowsSafeAlsoAppliesToDirectoriesInThePath()
    {
        Options options;
        options.windowsSafe = true;
        const auto r = sanitize({{QStringLiteral("AUX"), true}, {QStringLiteral("AUX\\f.txt"), false}}, options);
        QVERIFY(r.ok());
        QCOMPARE(r.entries[0].relativePath, QStringLiteral("_AUX"));
        QCOMPARE(r.entries[1].relativePath, QStringLiteral("_AUX/f.txt"));
    }

    void collisionsAreRenamed()
    {
        const auto r = sanitize({{QStringLiteral("a.txt"), false}, {QStringLiteral("A.TXT"), false}, {QStringLiteral("a.txt"), false}, {QStringLiteral("a (2).txt"), false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries[0].relativePath, QStringLiteral("a.txt"));
        QCOMPARE(r.entries[1].relativePath, QStringLiteral("A (2).TXT"));
        QCOMPARE(r.entries[2].relativePath, QStringLiteral("a (3).txt"));
        QCOMPARE(r.entries[3].relativePath, QStringLiteral("a (2) (2).txt")); // "a (2).txt" is taken (case-folded) by the renamed A.TXT
        QVERIFY(r.entries[1].renamed && r.entries[2].renamed && r.entries[3].renamed);
        QSet<QString> seen;
        for (const auto &e : r.entries) {
            QVERIFY(!seen.contains(e.relativePath.toCaseFolded()));
            seen.insert(e.relativePath.toCaseFolded());
        }
    }
    void normalizationCollisionsAreRenamed()
    {
        const auto r = sanitize({{QString::fromUtf8("\xc3\xa9.txt"), false}, {QString::fromUtf8("e\xcc\x81.txt"), false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries[0].relativePath, QString::fromUtf8("\xc3\xa9.txt"));
        QCOMPARE(r.entries[1].relativePath, QString::fromUtf8("\xc3\xa9 (2).txt"));
    }
    void collisionAfterReplacementIsRenamed()
    {
        const auto r = sanitize({{QStringLiteral("a_b"), false}, {QString(QStringLiteral("a") + QChar(0x200b) + QStringLiteral("b")), false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries[1].relativePath, QStringLiteral("a_b (2)"));
    }
    void collisionRenameKeepsNameWithinTheLimit()
    {
        const QString big(255, QLatin1Char('x'));
        const auto r = sanitize({{big, false}, {big.toUpper(), false}});
        QVERIFY(r.ok());
        QVERIFY(utf8Length(r.entries[1].components.last()) <= Limits::MaxComponentBytes);
        QVERIFY(r.entries[1].components.last().endsWith(QStringLiteral(" (2)")));
    }
    void manyIdenticalNamesStayLinear()
    {
        QList<RawEntry> list(qsizetype(Limits::MaxEntries), RawEntry{QStringLiteral("same.txt"), false});
        QElapsedTimer timer;
        timer.start();
        const auto r = sanitize(list);
        QVERIFY(r.ok());
        QVERIFY2(timer.elapsed() < 10000, qPrintable(QString::number(timer.elapsed())));
        QSet<QString> seen;
        for (const auto &e : r.entries) seen.insert(e.relativePath.toCaseFolded());
        QCOMPARE(seen.size(), qsizetype(Limits::MaxEntries));
        QCOMPARE(r.entries.last().relativePath, QStringLiteral("same (100000).txt"));
    }
    void renamedDirectoryCarriesItsChildren()
    {
        const auto r = sanitize({{QStringLiteral("Docs"), true},
                                 {QStringLiteral("Docs\\a"), false},
                                 {QStringLiteral("docs"), true},
                                 {QStringLiteral("docs\\a"), false},
                                 {QStringLiteral("docs\\b"), false},
                                 {QStringLiteral("Docs\\b"), false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries[1].relativePath, QStringLiteral("Docs/a"));
        QCOMPARE(r.entries[2].relativePath, QStringLiteral("docs (2)"));
        QCOMPARE(r.entries[3].relativePath, QStringLiteral("docs (2)/a"));
        QCOMPARE(r.entries[4].relativePath, QStringLiteral("docs (2)/b"));
        QCOMPARE(r.entries[5].relativePath, QStringLiteral("Docs/b"));
    }
    void sameDirectoryListedTwiceMerges()
    {
        const auto r = sanitize({{QStringLiteral("d"), true}, {QStringLiteral("d\\x"), false}, {QStringLiteral("d"), true}, {QStringLiteral("d\\y"), false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries[2].relativePath, QStringLiteral("d"));
        QCOMPARE(r.entries[3].relativePath, QStringLiteral("d/y"));
    }
    void implicitParentsAreSupported()
    {
        const auto r = sanitize({{QStringLiteral("a\\b\\c.txt"), false}, {QStringLiteral("a\\b\\d.txt"), false}});
        QVERIFY(r.ok());
        QCOMPARE(r.entries[1].relativePath, QStringLiteral("a/b/d.txt"));
    }
    void fileUsedAsDirectoryIsAConflict()
    {
        const auto r = sanitize({{QStringLiteral("a"), false}, {QStringLiteral("a\\b"), false}});
        QVERIFY(!r.ok());
        QCOMPARE(*r.reject, Reject::TypeConflict);
        const auto r2 = sanitize({{QStringLiteral("a"), true}, {QStringLiteral("a"), false}});
        QVERIFY(!r2.ok());
        QCOMPARE(*r2.reject, Reject::TypeConflict);
    }
    void nothingEverEscapesTheRoot_data()
    {
        QTest::addColumn<QString>("path");
        for (const char *p : {"..", "../x", "..\\x", "a\\..\\..\\x", "\\x", "/x", "C:x", "C:\\x", "\\\\h\\s", "x\\..", ".\\x", "a\\.\\b", "a\\\\b", "a/../b"}) {
            QTest::newRow(p) << QString::fromLatin1(p);
        }
    }
    void nothingEverEscapesTheRoot()
    {
        QFETCH(QString, path);
        const auto r = sanitize({{path, false}});
        QVERIFY(!r.ok());
    }
    void everyAcceptedResultIsSafe()
    {
        // A mix of odd but accepted names: whatever comes out has no separators, dots-only, controls or format characters.
        const auto r = sanitize({{QStringLiteral("...a"), false},
                                 {QString(QStringLiteral("x") + QChar(0x2066) + QStringLiteral("y")), false},
                                 {QString::fromUtf8("\xef\xbb\xbfbom.txt"), false},
                                 {QStringLiteral("  spaced  "), false},
                                 {QStringLiteral("-rf"), false},
                                 {QStringLiteral("~"), false}});
        QVERIFY(r.ok());
        for (const auto &e : r.entries) {
            for (const QString &c : e.components) {
                QVERIFY(!c.isEmpty());
                QVERIFY(c != QLatin1String(".") && c != QLatin1String(".."));
                QVERIFY(!c.contains(QLatin1Char('/')) && !c.contains(QLatin1Char('\\')) && !c.contains(QLatin1Char(':')));
                for (QChar ch : c) QVERIFY(!isControl(ch.unicode()) && !isFormatOrBidi(ch.unicode()));
            }
        }
    }
    void errorTextNeverCarriesTheName()
    {
        const QString secret = QStringLiteral("canary-token-xyz\\..\\x");
        const auto r = sanitize({{secret, false}});
        QVERIFY(!r.ok());
        QVERIFY(!QString::fromLatin1(rejectName(*r.reject)).contains(QStringLiteral("canary")));
    }
    void utf8LengthMatchesQt()
    {
        for (const QString &s : {QString(), QStringLiteral("abc"), QString::fromUtf8("\xc3\xa9\xe2\x82\xac"), QString::fromUcs4(U"\U0001F600x")}) {
            QCOMPARE(utf8Length(s), s.toUtf8().size());
        }
        // An unpaired surrogate is counted as a 3-byte replacement (an upper bound; Qt writes one byte).
        QCOMPARE(utf8Length(QString(QStringLiteral("a") + QChar(0xd800))), qsizetype(4));
    }
};

QTEST_GUILESS_MAIN(ClipboardFileNamesTest)

#include "ClipboardFileNamesTest.moc"
