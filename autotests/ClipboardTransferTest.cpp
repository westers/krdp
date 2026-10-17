// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078: the KRDPCTL `clipboard-transfer` record (strict parse, reply) and the host owner's ClipboardFileMaxRate value.

#include <QTest>

#include "ClipboardTransfer.h"

using namespace KRdp;
using namespace KRdp::ClipboardTransfer;
using ClipboardFilePacing::Ceiling;
using ClipboardFilePacing::RequestedMode;

namespace
{
QJsonObject record(const QString &mode, QJsonValue cap = QJsonValue::Undefined, QJsonValue id = QStringLiteral("r1"), QJsonValue version = 1)
{
    QJsonObject object{{QStringLiteral("type"), QStringLiteral("clipboard-transfer")}, {QStringLiteral("v"), version}, {QStringLiteral("id"), id}, {QStringLiteral("mode"), mode}};
    if (!cap.isUndefined()) object.insert(QStringLiteral("capBytesPerSecond"), cap);
    return object;
}
}

class ClipboardTransferTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theThreeModesAndTheCapParse()
    {
        QCOMPARE(parse(record(QStringLiteral("auto")))->mode, RequestedMode::Auto);
        QCOMPARE(parse(record(QStringLiteral("balanced")))->mode, RequestedMode::Balanced);
        QCOMPARE(parse(record(QStringLiteral("unlimited")))->mode, RequestedMode::Unlimited);
        const auto cap = parse(record(QStringLiteral("cap"), 5'000'000));
        QVERIFY(cap);
        QCOMPARE(cap->mode, RequestedMode::FixedCap);
        QCOMPARE(cap->capBytesPerSecond, std::optional<quint64>(5'000'000));
        QCOMPARE(parse(record(QStringLiteral("cap"), double(MinCapBytesPerSecond)))->capBytesPerSecond, std::optional<quint64>(MinCapBytesPerSecond));
        QCOMPARE(parse(record(QStringLiteral("cap"), double(MaxCapBytesPerSecond)))->capBytesPerSecond, std::optional<quint64>(MaxCapBytesPerSecond));
        QCOMPARE(parse(record(QStringLiteral("auto")))->id, QStringLiteral("r1"));
    }

    void anythingElseIsRefused()
    {
        QVERIFY(!parse(record(QStringLiteral("fast"))));
        QVERIFY(!parse(record(QStringLiteral("AUTO"))));
        QVERIFY(!parse(record(QStringLiteral("auto"), QJsonValue::Undefined, QStringLiteral("r1"), 2))); // version
        QVERIFY(!parse(record(QStringLiteral("auto"), QJsonValue::Undefined, QString()))); // empty id
        QVERIFY(!parse(record(QStringLiteral("auto"), QJsonValue::Undefined, QString(65, QLatin1Char('x'))))); // long id
        QVERIFY(!parse(record(QStringLiteral("auto"), QJsonValue::Undefined, 7))); // id not a string
        QVERIFY(!parse(record(QStringLiteral("cap")))); // a cap without its rate
        QVERIFY(!parse(record(QStringLiteral("cap"), 99'999))); // below 0.1 MB/s
        QVERIFY(!parse(record(QStringLiteral("cap"), 1'000'000'001.0))); // above 1000 MB/s
        QVERIFY(!parse(record(QStringLiteral("cap"), 0)));
        QVERIFY(!parse(record(QStringLiteral("cap"), -5)));
        QVERIFY(!parse(record(QStringLiteral("cap"), 1.5e6 + 0.5))); // not a whole number
        QVERIFY(!parse(record(QStringLiteral("cap"), QStringLiteral("5000000")))); // a string
        QVERIFY(!parse(record(QStringLiteral("auto"), 5'000'000))); // a rate without "cap"
        QVERIFY(!parse(record(QStringLiteral("unlimited"), 5'000'000)));
        QJsonObject other = record(QStringLiteral("auto"));
        other.insert(QStringLiteral("type"), QStringLiteral("audio-priority"));
        QVERIFY(!parse(other));
        QVERIFY(!parse({}));
    }

    void theReplyDescribesWhatIsInForce()
    {
        const auto request = record(QStringLiteral("unlimited"));
        // no ceiling: unlimited is honoured
        auto policy = ClipboardFilePacing::effectivePolicy(RequestedMode::Unlimited, std::nullopt, Ceiling::none());
        auto answer = reply(request, policy, Ceiling::none());
        QCOMPARE(answer.value(QStringLiteral("type")).toString(), QStringLiteral("clipboard-transfer"));
        QCOMPARE(answer.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(answer.value(QStringLiteral("id")).toString(), QStringLiteral("r1"));
        QCOMPARE(answer.value(QStringLiteral("effective")).toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("unlimited"));
        QCOMPARE(answer.value(QStringLiteral("ceiling")).toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("auto"));
        // a rate ceiling beats unlimited and says so
        policy = ClipboardFilePacing::effectivePolicy(RequestedMode::Unlimited, std::nullopt, Ceiling::rate(2'500'000));
        answer = reply(request, policy, Ceiling::rate(2'500'000));
        QCOMPARE(answer.value(QStringLiteral("effective")).toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("cap"));
        QCOMPARE(answer.value(QStringLiteral("effective")).toObject().value(QStringLiteral("capBytesPerSecond")).toInteger(), qint64(2'500'000));
        QCOMPARE(answer.value(QStringLiteral("ceiling")).toObject().value(QStringLiteral("capBytesPerSecond")).toInteger(), qint64(2'500'000));
        // balanced ceiling
        policy = ClipboardFilePacing::effectivePolicy(RequestedMode::Auto, std::nullopt, Ceiling::balanced());
        QCOMPARE(describe(policy).value(QStringLiteral("mode")).toString(), QStringLiteral("balanced"));
        // the lower of a cap and a ceiling
        policy = ClipboardFilePacing::effectivePolicy(RequestedMode::FixedCap, 8'000'000, Ceiling::rate(2'500'000));
        QCOMPARE(describe(policy).value(QStringLiteral("capBytesPerSecond")).toInteger(), qint64(2'500'000));
        policy = ClipboardFilePacing::effectivePolicy(RequestedMode::FixedCap, 1'000'000, Ceiling::rate(2'500'000));
        QCOMPARE(describe(policy).value(QStringLiteral("capBytesPerSecond")).toInteger(), qint64(1'000'000));
        // an error answer
        answer = reply(request, policy, Ceiling::none(), QStringLiteral("invalid clipboard-transfer request"));
        QCOMPARE(answer.value(QStringLiteral("ok")).toBool(), false);
        QCOMPARE(answer.value(QStringLiteral("message")).toString(), QStringLiteral("invalid clipboard-transfer request"));
        QCOMPARE(reply(record(QStringLiteral("auto"), QJsonValue::Undefined, QString(200, QLatin1Char('i'))), policy, Ceiling::none()).value(QStringLiteral("id")).toString().size(), 64);
    }

    void theHostOwnersCeilingSpellingIsStrict()
    {
        QCOMPARE(normalizeCeiling(QStringLiteral("auto")), std::optional<QString>(QStringLiteral("auto")));
        QCOMPARE(normalizeCeiling(QStringLiteral("balanced")), std::optional<QString>(QStringLiteral("balanced")));
        for (const char *good : {"0.1", "0.5", "1", "2.5", "10", "125", "999.999", "1000", "0.100"}) QVERIFY2(normalizeCeiling(QString::fromLatin1(good)), good);
        for (const char *bad : {"", "0", "0.0", "0.09", "0.099", "1000.001", "1001", "10000", "-1", "+1", "1e3", "01", "00.5", ".5", "5.", "1.2345", "AUTO", "Balanced", " auto", "auto ", "5 MB", "5M", "NaN", "inf", "1,5"})
            QVERIFY2(!normalizeCeiling(QString::fromLatin1(bad)), bad);
        QVERIFY(parseCeiling(QStringLiteral("auto"))->kind == Ceiling::Kind::None);
        QVERIFY(parseCeiling(QStringLiteral("balanced"))->kind == Ceiling::Kind::Balanced);
        const auto rate = parseCeiling(QStringLiteral("2.5"));
        QVERIFY(rate && rate->kind == Ceiling::Kind::Rate);
        QCOMPARE(rate->bytesPerSecond, quint64(2'500'000));
        QCOMPARE(parseCeiling(QStringLiteral("0.1"))->bytesPerSecond, quint64(100'000));
        QCOMPARE(parseCeiling(QStringLiteral("1000"))->bytesPerSecond, quint64(1'000'000'000));
        QVERIFY(!parseCeiling(QStringLiteral("fast")));
    }
};

QTEST_GUILESS_MAIN(ClipboardTransferTest)
#include "ClipboardTransferTest.moc"
