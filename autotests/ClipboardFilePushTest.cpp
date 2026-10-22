// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 3: the client-to-host push plan (descriptor -> sanitized entries, fetch list, skips, totals), pure.
// Spec 4.1 (names, Windows-origin round trip, `~1`), 4.3 (hostile descriptors: counts, depth, 64-bit sizes), 5 (huge files).

#include <QElapsedTimer>
#include <QTest>

#include "ClipboardFilePush.h"

using namespace KRdp;
using namespace KRdp::ClipboardFilePush;
using ClipboardFileDescriptor::Descriptor;
using namespace Qt::StringLiterals;

namespace
{
Descriptor file(const QString &name, quint64 size, std::optional<quint64> writeTime = std::nullopt)
{
    Descriptor d;
    d.name = name;
    d.flags = ClipboardFileDescriptor::FdAttributes | ClipboardFileDescriptor::FdFileSize;
    d.attributes = ClipboardFileDescriptor::AttrNormal;
    d.size = size;
    d.writeTime = writeTime;
    return d;
}
Descriptor dir(const QString &name)
{
    Descriptor d;
    d.name = name;
    d.flags = ClipboardFileDescriptor::FdAttributes;
    d.attributes = ClipboardFileDescriptor::AttrDirectory;
    return d;
}
QByteArray encoded(const QList<ClipboardFileDescriptor::EncodeItem> &items)
{
    const auto result = ClipboardFileDescriptor::encode(items);
    return result.data;
}
}

class ClipboardFilePushTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void plainTree()
    {
        const auto plan = makePlan(QList<Descriptor>{dir(u"photos"_s), file(u"photos\\a.jpg"_s, 1000), file(u"photos\\b.jpg"_s, 0), file(u"notes.txt"_s, 5)}, true);
        QVERIFY(plan.ok());
        QCOMPARE(plan.entries.size(), 4);
        QCOMPARE(plan.entries[0].relativePath, u"photos"_s);
        QVERIFY(plan.entries[0].isDirectory);
        QCOMPARE(plan.entries[1].relativePath, u"photos/a.jpg"_s);
        QCOMPARE(plan.entries[1].index, quint32(1));
        QCOMPARE(plan.files, 3);
        QCOMPARE(plan.directories, 1);
        QCOMPARE(plan.dataFiles, 2);
        QCOMPARE(plan.totalBytes, quint64(1005));
        QCOMPARE(plan.topLevelCount, 2);
        QCOMPARE(plan.skippedTotal(), 0);
        QVERIFY(!plan.renamed);
        // The fetch list names only files that have bytes, in descriptor order.
        const auto fetch = fetchList(plan);
        QCOMPARE(fetch.size(), 2);
        QCOMPARE(fetch[0].index, quint32(1));
        QCOMPARE(fetch[0].size, quint64(1000));
        QCOMPARE(fetch[1].index, quint32(3));
        QVERIFY(!plan.entries[2].needsData()); // the empty file is created, never requested
        QVERIFY(plan.entries[2].wanted());
    }

    void modifiedTimeIsKept()
    {
        const auto fileTime = ClipboardFileDescriptor::fileTimeFromUnixSeconds(1'700'000'000);
        const auto plan = makePlan(QList<Descriptor>{file(u"a"_s, 1, fileTime), file(u"b"_s, 1, std::nullopt), file(u"c"_s, 1, quint64(0))}, true);
        QVERIFY(plan.ok());
        QCOMPARE(plan.entries[0].mtimeSeconds, std::optional<qint64>(1'700'000'000));
        QVERIFY(!plan.entries[1].mtimeSeconds);
        QVERIFY(!plan.entries[2].mtimeSeconds); // an unset FILETIME is not 1601
    }

    // spec 4.1: a Windows-origin set round-trips: reserved names, trailing dots/spaces, 8.3 aliases and `<>"|?*` are renamed on the host,
    // and what the host created is stable: planning the sanitized names again changes nothing.
    void windowsOriginNamesRoundTrip_data()
    {
        QTest::addColumn<QString>("raw");
        QTest::addColumn<QString>("expected");
        QTest::newRow("short alias") << u"PROGRA~1"_s << u"PROGRA_1"_s;
        QTest::newRow("short alias with ext") << u"REPORT~12.TXT"_s << u"REPORT_12.TXT"_s;
        QTest::newRow("device") << u"CON.txt"_s << u"_CON.txt"_s;
        QTest::newRow("device lower") << u"nul"_s << u"_nul"_s;
        QTest::newRow("com port") << u"COM3.log"_s << u"_COM3.log"_s;
        QTest::newRow("trailing dot") << u"report."_s << u"report"_s;
        QTest::newRow("trailing space") << u"report "_s << u"report"_s;
        QTest::newRow("question mark") << u"what?.txt"_s << u"what_.txt"_s;
        QTest::newRow("star and bar") << u"a*b|c.txt"_s << u"a_b_c.txt"_s;
        QTest::newRow("quote") << u"say \"hi\".txt"_s << u"say _hi_.txt"_s;
        QTest::newRow("untouched") << u"Résumé 2026 (final).docx"_s << u"Résumé 2026 (final).docx"_s;
    }
    void windowsOriginNamesRoundTrip()
    {
        QFETCH(QString, raw);
        QFETCH(QString, expected);
        const auto plan = makePlan(QList<Descriptor>{file(raw, 3)}, true);
        QVERIFY(plan.ok());
        QCOMPARE(plan.entries[0].relativePath, expected);
        QCOMPARE(plan.renamed, raw != expected);
        const auto again = makePlan(QList<Descriptor>{file(plan.entries[0].relativePath, 3)}, true);
        QVERIFY(again.ok());
        QCOMPARE(again.entries[0].relativePath, expected); // idempotent
        QVERIFY(!again.renamed);
    }
    void windowsHostileDirectoryNamesApplyToTheWholePath()
    {
        const auto plan = makePlan(QList<Descriptor>{dir(u"PROGRA~1"_s), file(u"PROGRA~1\\aux.cfg"_s, 1)}, true);
        QVERIFY(plan.ok());
        QCOMPARE(plan.entries[0].relativePath, u"PROGRA_1"_s);
        QCOMPARE(plan.entries[1].relativePath, u"PROGRA_1/_aux.cfg"_s);
    }
    void collisionsAreRenamedNeverOverwritten()
    {
        const auto plan = makePlan(QList<Descriptor>{file(u"a.txt"_s, 1), file(u"A.TXT"_s, 1), file(u"a.txt"_s, 1)}, true);
        QVERIFY(plan.ok());
        QSet<QString> names;
        for (const auto &entry : plan.entries) names.insert(entry.relativePath.toCaseFolded());
        QCOMPARE(names.size(), 3);
        QVERIFY(plan.renamed);
    }

    // spec 4.1: one hostile name refuses the whole transfer, even when its entry would have been skipped.
    void hostileNameRefusesEverything_data()
    {
        QTest::addColumn<QString>("raw");
        QTest::newRow("parent") << u"..\\x"_s;
        QTest::newRow("parent inside") << u"a\\..\\..\\x"_s;
        QTest::newRow("dot") << u"a\\.\\b"_s;
        QTest::newRow("absolute backslash") << u"\\etc\\passwd"_s;
        QTest::newRow("absolute slash") << u"/etc/passwd"_s;
        QTest::newRow("drive") << u"C:\\Windows\\x"_s;
        QTest::newRow("drive relative") << u"C:x"_s;
        QTest::newRow("unc") << u"\\\\server\\share\\x"_s;
        QTest::newRow("stream") << u"a.txt:evil"_s;
        QTest::newRow("slash inside") << u"a\\b/c"_s;
        QTest::newRow("control") << QString(u"a\x01b"_s);
        QTest::newRow("del") << QString(u"a\x7f"_s);
        QTest::newRow("c1 control") << QString(u"a\u0085b"_s);
        QTest::newRow("empty component") << u"a\\\\b"_s;
        QTest::newRow("trailing separator") << u"a\\"_s;
        QTest::newRow("bidi in extension") << QString(u"invoice.\u202Etxt"_s);
        QTest::newRow("deep") << QStringList(QList<QString>(70, u"d"_s)).join(u'\\');
        QTest::newRow("component too long") << QString(300, u'x');
    }
    void hostileNameRefusesEverything()
    {
        QFETCH(QString, raw);
        Descriptor reparse = file(raw, 1);
        reparse.attributes |= ClipboardFileDescriptor::AttrReparsePoint; // would be skipped, still checked
        const auto plan = makePlan(QList<Descriptor>{file(u"fine.txt"_s, 1), reparse}, true);
        QVERIFY(!plan.ok());
        QCOMPARE(plan.refusal, Refusal::Names);
        QVERIFY(plan.nameReject.has_value());
        QVERIFY(plan.entries.isEmpty());
        QCOMPARE(fetchList(plan).size(), 0);
        QVERIFY2(!QByteArray(refusalName(plan.refusal)).contains("fine"), "refusal names are fixed words");
    }

    void skipRules()
    {
        Descriptor link = file(u"link"_s, 10);
        link.attributes |= ClipboardFileDescriptor::AttrReparsePoint;
        Descriptor sizeless = file(u"nosize"_s, 0);
        sizeless.size.reset();
        Descriptor linkDir = dir(u"junction"_s);
        linkDir.attributes |= ClipboardFileDescriptor::AttrReparsePoint;
        const auto plan = makePlan(QList<Descriptor>{file(u"ok"_s, 7), link, sizeless, linkDir, file(u"junction\\inside"_s, 9), dir(u"junction\\sub"_s),
                                                    file(u"junction\\sub\\deep"_s, 9)},
                                   true);
        QVERIFY(plan.ok());
        QCOMPARE(plan.skippedReparse, 2); // the symlink and the junction
        QCOMPARE(plan.skippedUnknownSize, 1);
        QCOMPARE(plan.skippedBelow, 3); // everything below the skipped directory
        QCOMPARE(plan.skippedTotal(), 6);
        QCOMPARE(plan.files, 1);
        QCOMPARE(plan.totalBytes, quint64(7));
        QCOMPARE(plan.entries[1].skip, Skip::Reparse);
        QCOMPARE(plan.entries[2].skip, Skip::UnknownSize);
        QCOMPARE(plan.entries[4].skip, Skip::BelowSkipped);
        QCOMPARE(fetchList(plan).size(), 1);
    }

    // spec 5 / S1: without the negotiated huge flag a file over 4 GiB - 1 is skipped and counted, never truncated.
    void hugeFilesNeedTheNegotiatedFlag()
    {
        const quint64 just = 0xffffffffULL;
        const auto without = makePlan(QList<Descriptor>{file(u"a"_s, just), file(u"b"_s, just + 1), file(u"c"_s, 5ULL << 30)}, false);
        QVERIFY(without.ok());
        QCOMPARE(without.skippedHuge, 2);
        QCOMPARE(without.files, 1);
        QCOMPARE(without.totalBytes, just);
        QCOMPARE(without.entries[1].skip, Skip::NeedsHuge);
        const auto with = makePlan(QList<Descriptor>{file(u"a"_s, just), file(u"b"_s, just + 1), file(u"c"_s, 5ULL << 30)}, true);
        QCOMPARE(with.skippedHuge, 0);
        QCOMPARE(with.files, 3);
        QCOMPARE(with.totalBytes, just + (just + 1) + (5ULL << 30));
    }

    // spec 4.3: 64-bit sizes, saturating arithmetic.
    void sizesAre64BitAndTotalsSaturate()
    {
        const quint64 big = 0xfffffffffffffff0ULL;
        const auto plan = makePlan(QList<Descriptor>{file(u"a"_s, big), file(u"b"_s, big), file(u"c"_s, 100)}, true);
        QVERIFY(plan.ok());
        QCOMPARE(plan.entries[0].size, big);
        QCOMPARE(plan.totalBytes, ~quint64(0));
    }

    void nothingToCopy()
    {
        QCOMPARE(makePlan(QList<Descriptor>{}, true).refusal, Refusal::NothingToCopy);
        Descriptor link = file(u"link"_s, 10);
        link.attributes |= ClipboardFileDescriptor::AttrReparsePoint;
        const auto plan = makePlan(QList<Descriptor>{link}, true);
        QCOMPARE(plan.refusal, Refusal::NothingToCopy);
        QCOMPARE(plan.skippedReparse, 1); // the notice can still say why
        // An empty directory alone is something to create.
        QVERIFY(makePlan(QList<Descriptor>{dir(u"empty"_s)}, true).ok());
    }

    // spec 4.3: the descriptor list is hostile before any name is read.
    void hostileDescriptorBytes()
    {
        QCOMPARE(makePlan(QByteArray(), true).refusal, Refusal::Descriptor);
        QCOMPARE(makePlan(QByteArray(3, 'x'), true).refusal, Refusal::Descriptor);
        // a count that does not match the payload
        QByteArray lie(4, '\0');
        qToLittleEndian<quint32>(5, lie.data());
        QCOMPARE(makePlan(lie, true).descriptorError, std::optional(ClipboardFileDescriptor::Error::LengthMismatch));
        // a count over the limit is refused from the header alone (nothing is allocated from it)
        qToLittleEndian<quint32>(ClipboardFileLimits::MaxEntries + 1, lie.data());
        const auto over = makePlan(lie, true);
        QCOMPARE(over.refusal, Refusal::Descriptor);
        QCOMPARE(over.descriptorError, std::optional(ClipboardFileDescriptor::Error::TooManyItems));
        qToLittleEndian<quint32>(0xffffffffu, lie.data());
        QCOMPARE(makePlan(lie, true).descriptorError, std::optional(ClipboardFileDescriptor::Error::TooManyItems));
        // an unterminated name (all 260 units non-zero)
        auto good = encoded({{u"a"_s, false, 1, std::nullopt}});
        for (int u = 0; u < ClipboardFileDescriptor::NameUnits; ++u) qToLittleEndian<quint16>(u'x', good.data() + 4 + ClipboardFileDescriptor::NameOffset + u * 2);
        QCOMPARE(makePlan(good, true).descriptorError, std::optional(ClipboardFileDescriptor::Error::UnterminatedName));
        // an empty name
        auto empty = encoded({{u"a"_s, false, 1, std::nullopt}});
        qToLittleEndian<quint16>(0, empty.data() + 4 + ClipboardFileDescriptor::NameOffset);
        QCOMPARE(makePlan(empty, true).descriptorError, std::optional(ClipboardFileDescriptor::Error::EmptyName));
        // trailing garbage
        QCOMPARE(makePlan(encoded({{u"a"_s, false, 1, std::nullopt}}) + "x", true).refusal, Refusal::Descriptor);
        // and a good one passes
        QVERIFY(makePlan(encoded({{u"a"_s, false, 1, std::nullopt}}), true).ok());
    }

    // spec 4.3: the entry limit is a safety valve, not a size limit; the maximum still plans in reasonable time.
    void maximumEntriesPlan()
    {
        QList<Descriptor> items;
        items.reserve(ClipboardFileLimits::MaxEntries);
        for (quint32 i = 0; i < ClipboardFileLimits::MaxEntries; ++i) items.append(file(QStringLiteral("d%1\\f%2").arg(i % 1000).arg(i), 1));
        // directories implied by the paths: fine
        QElapsedTimer timer;
        timer.start();
        const auto plan = makePlan(items, true);
        QVERIFY(plan.ok());
        QCOMPARE(plan.entries.size(), int(ClipboardFileLimits::MaxEntries));
        QCOMPARE(plan.totalBytes, quint64(ClipboardFileLimits::MaxEntries));
        QVERIFY2(timer.elapsed() < 20000, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(plan.topLevelCount, 1000);
        // one more is refused
        items.append(file(u"extra"_s, 1));
        QCOMPARE(makePlan(items, true).refusal, Refusal::Names);
        QCOMPARE(makePlan(items, true).nameReject, std::optional(ClipboardFileNames::Reject::TooManyEntries));
    }

    void totalPathBytesAreBounded()
    {
        QList<Descriptor> items;
        const QString longName = QString(250, u'n');
        for (int i = 0; i < 70000; ++i) items.append(file(QStringLiteral("%1\\%2\\%2%3").arg(i % 400).arg(longName).arg(i), 1)); // > 32 MiB of path text
        const auto plan = makePlan(items, true);
        QCOMPARE(plan.refusal, Refusal::Names);
        QCOMPARE(plan.nameReject, std::optional(ClipboardFileNames::Reject::TotalPathBytes));
    }

    void topLevelUnitsAndTheOfferRule()
    {
        const auto plan = makePlan(QList<Descriptor>{dir(u"tree"_s), file(u"tree\\a"_s, 5), file(u"tree\\b"_s, 5), file(u"loose.txt"_s, 3), file(u"empty.txt"_s, 0), dir(u"emptydir"_s)}, true);
        QVERIFY(plan.ok());
        QCOMPARE(plan.topLevelCount, 4);
        // the empty file (index 4) is created when the transfer is accepted, so the caller counts it complete from the start; the empty folder needs nothing
        QCOMPARE(finishedTopLevel(plan, {4}), QStringList({u"empty.txt"_s, u"emptydir"_s}));
        // one file of the tree is not enough for the tree
        QCOMPARE(finishedTopLevel(plan, {1, 3, 4}), QStringList({u"loose.txt"_s, u"empty.txt"_s, u"emptydir"_s}));
        QCOMPARE(finishedTopLevel(plan, {1, 2, 3, 4}), QStringList({u"tree"_s, u"loose.txt"_s, u"empty.txt"_s, u"emptydir"_s}));
        // a file not yet complete keeps only its own unit back
        QCOMPARE(finishedTopLevel(plan, {1, 2, 4}), QStringList({u"tree"_s, u"empty.txt"_s, u"emptydir"_s}));
    }

    void skippedUnitsAreNeverOffered()
    {
        Descriptor link = file(u"link"_s, 10);
        link.attributes |= ClipboardFileDescriptor::AttrReparsePoint;
        const auto plan = makePlan(QList<Descriptor>{link, file(u"real"_s, 1)}, true);
        QVERIFY(plan.ok());
        QCOMPARE(finishedTopLevel(plan, {1}), QStringList({u"real"_s}));
    }

    void refusalNamesAreFixedWords()
    {
        for (quint8 r = 0; r <= LastRefusal; ++r) {
            const QByteArray name = refusalName(Refusal(r));
            QVERIFY(!name.isEmpty());
            QVERIFY(!name.contains('/') && !name.contains(' '));
        }
    }
};

QTEST_APPLESS_MAIN(ClipboardFilePushTest)
#include "ClipboardFilePushTest.moc"
