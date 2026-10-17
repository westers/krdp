// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 1: FileGroupDescriptorW codec: round trip, truncation, hostile counts, 64-bit sizes, odd UTF-16.

#undef QT_USE_QSTRINGBUILDER // QByteArray + QByteArray must stay a QByteArray in the table rows
#include <QTest>

#include "ClipboardFileDescriptor.h"

using namespace KRdp::ClipboardFileDescriptor;
namespace Limits = KRdp::ClipboardFileLimits;

Q_DECLARE_METATYPE(KRdp::ClipboardFileDescriptor::Error)

namespace
{
/// A raw descriptor with count \a count and \a body appended verbatim.
QByteArray header(quint32 count)
{
    QByteArray data(4, '\0');
    qToLittleEndian<quint32>(count, data.data());
    return data;
}
/// One FILEDESCRIPTORW by hand (independent of the encoder).
QByteArray rawItem(quint32 flags, quint32 attrs, quint32 high, quint32 low, const QList<quint16> &name, bool terminate = true, quint64 writeTime = 0)
{
    QByteArray item(DescriptorBytes, '\0');
    qToLittleEndian<quint32>(flags, item.data());
    qToLittleEndian<quint32>(attrs, item.data() + 36);
    qToLittleEndian<quint64>(writeTime, item.data() + 56);
    qToLittleEndian<quint32>(high, item.data() + 64);
    qToLittleEndian<quint32>(low, item.data() + 68);
    for (int i = 0; i < name.size() && i < NameUnits; ++i) qToLittleEndian<quint16>(name[i], item.data() + NameOffset + i * 2);
    if (!terminate) {
        for (int i = name.size(); i < NameUnits; ++i) qToLittleEndian<quint16>(0x41, item.data() + NameOffset + i * 2);
    }
    return item;
}
QList<quint16> units(const QString &s)
{
    QList<quint16> v;
    for (QChar c : s) v.append(c.unicode());
    return v;
}
}

class ClipboardFileDescriptorTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void layoutConstants()
    {
        QCOMPARE(DescriptorBytes, 592);
        QCOMPARE(NameOffset, 72);
        QCOMPARE(MaxNameUnits, 259);
        QVERIFY(4 + qint64(Limits::MaxEntries) * DescriptorBytes <= Limits::MaxDescriptorWireBytes);
    }
    void roundTrip()
    {
        const auto t = KRdp::ClipboardFileDescriptor::fileTimeFromUnixSeconds(1'760'000'000, 500'000'000);
        QVERIFY(t);
        const QList<EncodeItem> items{{QStringLiteral("a.txt"), false, 12, t}, {QStringLiteral("dir"), true, 999, std::nullopt}, {QStringLiteral("dir\\b.bin"), false, 0, std::nullopt}};
        const auto encoded = encode(items);
        QVERIFY(encoded.ok());
        QCOMPARE(encoded.data.size(), 4 + 3 * DescriptorBytes);
        const auto decoded = decode(encoded.data);
        QVERIFY(decoded.ok());
        QCOMPARE(decoded.items.size(), 3);
        QCOMPARE(decoded.items[0].name, QStringLiteral("a.txt"));
        QCOMPARE(decoded.items[0].size, std::optional<quint64>(12));
        QCOMPARE(decoded.items[0].writeTime, t);
        QVERIFY(!decoded.items[0].isDirectory());
        QVERIFY(decoded.items[1].isDirectory());
        QCOMPARE(decoded.items[1].size, std::optional<quint64>(0)); // a directory carries no size
        QCOMPARE(decoded.items[2].name, QStringLiteral("dir\\b.bin"));
        QVERIFY(!decoded.items[2].writeTime);
    }
    void sizesAbove4GiBSurvive_data()
    {
        QTest::addColumn<quint64>("size");
        QTest::newRow("0") << quint64(0);
        QTest::newRow("1") << quint64(1);
        QTest::newRow("4GiB - 1") << quint64(0xffffffffULL);
        QTest::newRow("4GiB") << quint64(0x100000000ULL);
        QTest::newRow("5GiB") << quint64(5) * 1024 * 1024 * 1024;
        QTest::newRow("high only") << quint64(0x7fffffff00000000ULL);
        QTest::newRow("max") << quint64(0xffffffffffffffffULL);
    }
    void sizesAbove4GiBSurvive()
    {
        QFETCH(quint64, size);
        const auto enc = encode({{QStringLiteral("big"), false, size, std::nullopt}});
        QVERIFY(enc.ok());
        const auto dec = decode(enc.data);
        QVERIFY(dec.ok());
        QCOMPARE(dec.items[0].size, std::optional<quint64>(size));
        // And the hand-built form puts High at 64 and Low at 68.
        const auto hand = header(1) + rawItem(FdFileSize, 0, quint32(size >> 32), quint32(size), units(QStringLiteral("big")));
        QCOMPARE(decode(hand).items[0].size, std::optional<quint64>(size));
    }
    void sizeOnlyWithTheFlag()
    {
        const auto dec = decode(header(1) + rawItem(0, 0, 1, 2, units(QStringLiteral("x"))));
        QVERIFY(dec.ok());
        QVERIFY(!dec.items[0].size);
        QVERIFY(!dec.items[0].writeTime);
        QCOMPARE(dec.items[0].attributes, 0u);
    }
    void attributesOnlyWithTheFlag()
    {
        const auto with = decode(header(1) + rawItem(FdAttributes, AttrDirectory | AttrReparsePoint, 0, 0, units(QStringLiteral("x"))));
        QVERIFY(with.items[0].isDirectory() && with.items[0].isReparsePoint());
        const auto without = decode(header(1) + rawItem(0, AttrDirectory, 0, 0, units(QStringLiteral("x"))));
        QVERIFY(!without.items[0].isDirectory());
    }
    void emptyListIsValid()
    {
        const auto dec = decode(header(0));
        QVERIFY(dec.ok());
        QVERIFY(dec.items.isEmpty());
        QVERIFY(encode({}).ok());
    }

    void truncatedAndHostile_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::addColumn<Error>("error");
        QTest::newRow("empty") << QByteArray() << Error::TooShort;
        QTest::newRow("3 bytes") << QByteArray(3, '\x01') << Error::TooShort;
        QTest::newRow("count 1, no body") << header(1) << Error::LengthMismatch;
        QTest::newRow("count 1, short body") << header(1) + QByteArray(DescriptorBytes - 1, 'x') << Error::LengthMismatch;
        QTest::newRow("count 1, extra byte") << header(1) + rawItem(0, 0, 0, 0, units(QStringLiteral("a"))) + QByteArray(1, '\0') << Error::LengthMismatch;
        QTest::newRow("count 0, trailing") << header(0) + QByteArray(1, '\0') << Error::LengthMismatch;
        QTest::newRow("count 100001") << header(100001) << Error::TooManyItems;
        QTest::newRow("count 0xffffffff") << header(0xffffffffu) << Error::TooManyItems;
        QTest::newRow("count 0x80000000") << header(0x80000000u) << Error::TooManyItems;
        QTest::newRow("count 0x1c71c72 (x592 wraps 32 bits)") << header(0x1c71c72u) << Error::TooManyItems;
        QTest::newRow("unterminated name") << header(1) + rawItem(0, 0, 0, 0, QList<quint16>(), false) << Error::UnterminatedName;
        QTest::newRow("empty name") << header(1) + rawItem(0, 0, 0, 0, QList<quint16>()) << Error::EmptyName;
        QTest::newRow("second item bad") << header(2) + rawItem(0, 0, 0, 0, units(QStringLiteral("ok"))) + rawItem(0, 0, 0, 0, QList<quint16>(), false) << Error::UnterminatedName;
    }
    void truncatedAndHostile()
    {
        QFETCH(QByteArray, data);
        QFETCH(Error, error);
        const auto dec = decode(data);
        QVERIFY(!dec.ok());
        QCOMPARE(*dec.error, error);
        QVERIFY(dec.items.isEmpty());
        QVERIFY(qstrlen(errorName(error)) > 0);
    }
    void everyTruncationOfAValidDescriptorFails()
    {
        const auto valid = encode({{QStringLiteral("a"), false, 1, std::nullopt}, {QStringLiteral("b"), false, 2, std::nullopt}});
        QVERIFY(valid.ok());
        for (int n = 0; n < valid.data.size(); ++n) {
            QVERIFY2(!decode(QByteArrayView(valid.data).first(n)).ok(), qPrintable(QString::number(n)));
        }
        QVERIFY(decode(valid.data).ok());
    }
    void oversizedPayloadIsRefusedBeforeParsing()
    {
        QByteArray huge(int(Limits::MaxDescriptorWireBytes) + 1, '\0');
        const auto dec = decode(huge);
        QVERIFY(!dec.ok());
        QCOMPARE(*dec.error, Error::TooLarge);
    }
    void oddUtf16IsKeptRawForTheSanitizer()
    {
        // unpaired surrogates, a bidi override and text after the NUL (ignored)
        QList<quint16> name{0x61, 0xd800, 0x62, 0x202e, 0x63, 0x00, 0x7a, 0x7a};
        const auto dec = decode(header(1) + rawItem(0, 0, 0, 0, name));
        QVERIFY(dec.ok());
        QCOMPARE(dec.items[0].name.size(), 5);
        QCOMPARE(dec.items[0].name[1].unicode(), char16_t(0xd800));
        QCOMPARE(dec.items[0].name[3].unicode(), char16_t(0x202e));
        // A name of exactly 259 units decodes; 260 without NUL does not.
        const QList<quint16> longest(259, 0x61);
        QCOMPARE(decode(header(1) + rawItem(0, 0, 0, 0, longest)).items[0].name.size(), 259);
        QVERIFY(!decode(header(1) + rawItem(0, 0, 0, 0, QList<quint16>(260, 0x61))).ok());
    }
    void namesWithPathSeparatorsAreNotInterpretedHere()
    {
        const auto dec = decode(header(1) + rawItem(0, 0, 0, 0, units(QStringLiteral("..\\..\\x"))));
        QVERIFY(dec.ok());
        QCOMPARE(dec.items[0].name, QStringLiteral("..\\..\\x")); // the sanitizer refuses it later
    }

    void encoderLimits()
    {
        QVERIFY(encode({{QString(259, QLatin1Char('a')), false, 1, std::nullopt}}).ok());
        const auto tooLong = encode({{QString(260, QLatin1Char('a')), false, 1, std::nullopt}});
        QVERIFY(!tooLong.ok());
        QCOMPARE(*tooLong.error, Error::NameTooLong);
        QCOMPARE(*encode({{QString(), false, 1, std::nullopt}}).error, Error::EmptyName);
        QCOMPARE(*encode({{QString(QStringLiteral("a") + QChar(0) + QStringLiteral("b")), false, 1, std::nullopt}}).error, Error::NameHasNul);
        QList<EncodeItem> many(qsizetype(Limits::MaxEntries) + 1, EncodeItem{QStringLiteral("x"), false, 1, std::nullopt});
        QCOMPARE(*encode(many).error, Error::TooManyItems);
        // The maximum list encodes and decodes within the wire cap.
        many.removeLast();
        const auto max = encode(many);
        QVERIFY(max.ok());
        QVERIFY(max.data.size() <= Limits::MaxDescriptorWireBytes);
        QCOMPARE(decode(max.data).items.size(), qsizetype(Limits::MaxEntries));
    }
    void fileTimeConversions()
    {
        QCOMPARE(fileTimeFromUnixSeconds(0), std::optional<quint64>(116444736000000000ULL));
        QCOMPARE(unixSecondsFromFileTime(116444736000000000ULL), qint64(0));
        QCOMPARE(fileTimeFromUnixSeconds(-11644473600LL), std::optional<quint64>(0));
        QVERIFY(!fileTimeFromUnixSeconds(-11644473601LL));
        QVERIFY(!fileTimeFromUnixSeconds(std::numeric_limits<qint64>::max()));
        QCOMPARE(unixSecondsFromFileTime(*fileTimeFromUnixSeconds(1'760'000'000, 999'999'999)), qint64(1'760'000'000));
    }
};

QTEST_GUILESS_MAIN(ClipboardFileDescriptorTest)

#include "ClipboardFileDescriptorTest.moc"
