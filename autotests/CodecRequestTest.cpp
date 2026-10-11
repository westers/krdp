// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-062 S2: the KRDPCTL `codec` request with the client's own order / encode / decoders, parsed (valid, malformed,
// oversized, unknown, duplicate, empty) and applied to a real VideoStream (no socket): the reply names the encoder,
// what was skipped and why, the baseline; the capabilities and the stats sample carry the new fields.

#include "CodecRequest.h"
#include "EncoderSupport.h"
#include "LayoutControl.h"
#include "RdpConnection.h"
#include "Server.h"
#include "StatsReporter.h"
#include "VideoStream.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using namespace KRdp;
using CodecPolicy::Family;

namespace
{
CodecPolicy::Encoders hal() // AMD: hardware AVC / HEVC / AV1, software H.264
{
    CodecPolicy::Encoders e;
    e.avc = {true, true};
    e.hevc = {true, false};
    e.av1 = {true, false};
    return e;
}
CodecPolicy::Encoders sol() // NVENC HEVC; AVC and AV1 software only
{
    CodecPolicy::Encoders e;
    e.avc = {false, true};
    e.hevc = {true, true};
    e.av1 = {false, true};
    return e;
}
CodecPolicy::Encoders softwareOnly()
{
    CodecPolicy::Encoders e;
    e.avc = {false, true};
    e.hevc = {false, true};
    e.av1 = {false, true};
    return e;
}
CodecPolicy::EncoderLabels labels()
{
    CodecPolicy::EncoderLabels l;
    l.hardware[size_t(Family::Avc)] = {QStringLiteral("vaapi"), {}, {}};
    l.hardware[size_t(Family::Hevc)] = {QStringLiteral("nvenc"), QStringLiteral("0000:09:00.0"), QStringLiteral("NVIDIA GeForce RTX 2070")};
    l.hardware[size_t(Family::Av1)] = {QStringLiteral("vaapi"), {}, {}};
    l.software[size_t(Family::Avc)] = {QStringLiteral("libx264"), {}, {}};
    l.software[size_t(Family::Hevc)] = {QStringLiteral("libx265"), {}, {}};
    l.software[size_t(Family::Av1)] = {QStringLiteral("libsvtav1"), {}, {}};
    return l;
}
QJsonObject json(const char *text)
{
    return QJsonDocument::fromJson(text).object();
}
}

class CodecRequestTest : public QObject
{
    Q_OBJECT

    struct Fixture {
        Server server;
        RdpConnection connection{&server, -1};
        Fixture()
        {
            QCoreApplication::removePostedEvents(&connection, QEvent::MetaCall); // no socket to initialise
        }
        VideoStream *stream() { return connection.videoStream(); }
        QJsonObject ask(const char *request, const CodecPolicy::Encoders &encoders, std::optional<CodecPolicy::SoftwareAllowance> allowance = {})
        {
            stream()->setEncoderPolicy(encoders, CodecPolicy::SoftwareEncoding::Auto);
            stream()->setEncoderLabels(labels());
            if (allowance) stream()->setSoftwareAllowance(*allowance);
            const auto parsed = CodecRequest::parse(json(request));
            if (!parsed) return {};
            return CodecRequest::apply(*stream(), *parsed, &lastLog);
        }
        QString lastLog;
    };

    static QStringList whyOf(const QJsonObject &reply, const QString &codec)
    {
        for (const auto &entry : reply.value(QLatin1String("skipped")).toArray()) {
            if (entry.toObject().value(QLatin1String("codec")).toString() == codec) {
                QStringList why;
                for (const auto &reason : entry.toObject().value(QLatin1String("why")).toArray()) why << reason.toString();
                return why;
            }
        }
        return {QStringLiteral("<not skipped>")};
    }

private Q_SLOTS:
    // ---- parser ----

    // The literal the client (krdp-client CodecSelectionTest::decodersJsonIsThePerCodecPathMap) builds for a Hal-like
    // machine: VAAPI + NVDEC are two hardware decoders but ONE "hw" on the wire (contract: at most two DISTINCT paths).
    void halClientDecodersSampleIsAccepted()
    {
        QString error;
        const auto r = CodecRequest::parse(QJsonDocument::fromJson(R"({"type":"codec","v":1,"requestId":"r1","codecs":["hevc","av1"],"adaptive":true,
            "decode":{"avc":"sw","hevc":"hw","av1":"hw"},"order":["av1","hevc","avc"],"encode":"any","decodeMode":"any",
            "decoders":{"avc":["sw"],"hevc":["hw","sw"],"av1":["hw","sw"]}})").object(), &error);
        QVERIFY2(r, qPrintable(error));
        QCOMPARE(r->selection->decodersOf(Family::Av1), (CodecPolicy::DecoderPaths{true, true}));
        QCOMPARE(r->selection->order.first(), Family::Av1);
    }

    void validRecordCarriesTheClientsSelection()
    {
        const auto r = CodecRequest::parse(json(R"({"type":"codec","v":1,"requestId":"r1","codecs":["hevc","av1"],"adaptive":false,
            "decode":{"avc":"sw","hevc":"hw","av1":"sw"},
            "order":["av1","hevc","avc"],"encode":"software","decodeMode":"hardware",
            "decoders":{"avc":["sw"],"hevc":["hw","sw"],"av1":[]}})"));
        QVERIFY(r);
        QVERIFY(r->selection);
        const auto &s = *r->selection;
        QCOMPARE(s.order, (QList<Family>{Family::Av1, Family::Hevc, Family::Avc}));
        QCOMPARE(s.encode, CodecPolicy::Mode::Software);
        QCOMPARE(s.decode, CodecPolicy::Mode::Hardware);
        QVERIFY(!s.adaptive);
        QCOMPARE(s.decodersOf(Family::Avc), (CodecPolicy::DecoderPaths{false, true}));
        QCOMPARE(s.decodersOf(Family::Hevc), (CodecPolicy::DecoderPaths{true, true}));
        QCOMPARE(s.decodersOf(Family::Av1), (CodecPolicy::DecoderPaths{false, false})); // [] = cannot
        // `decode` (what the client will use, for the AV1 tiles) is the first entry of each list; [] = unknown.
        QCOMPARE(r->decode.avc, CodecPolicy::DecodePath::Software);
        QCOMPARE(r->decode.hevc, CodecPolicy::DecodePath::Hardware);
        QCOMPARE(r->decode.av1, CodecPolicy::DecodePath::Unknown);
        QVERIFY(r->orderText.contains(QStringLiteral("order [av1,hevc,avc] encode software decode hardware")));
    }

    void anOldRecordHasNoSelection()
    {
        const auto r = CodecRequest::parse(json(R"({"codecs":["hevc"],"adaptive":true,"decode":{"hevc":"hw"}})"));
        QVERIFY(r);
        QVERIFY(!r->selection);
        QCOMPARE(r->codecs, (QVector<VideoCodec>{VideoCodec::Hevc}));
    }

    void emptyOrderMeansStandardAvcOnly()
    {
        const auto r = CodecRequest::parse(json(R"({"codecs":[],"order":[]})"));
        QVERIFY(r);
        QVERIFY(r->selection);
        QVERIFY(r->selection->order.isEmpty());
        Fixture f;
        const auto reply = f.ask(R"({"codecs":[],"order":[],"encode":"any"})", hal());
        QCOMPARE(reply.value(QLatin1String("selected")).toString(), QStringLiteral("avc"));
        QVERIFY(!f.stream()->privateCodecPolicyActive());
    }

    void encodeAloneUsesTheCodecsListAsTheOrder()
    {
        const auto r = CodecRequest::parse(json(R"({"codecs":["av1","hevc"],"encode":"hardware"})"));
        QVERIFY(r);
        QVERIFY(r->selection);
        QCOMPARE(r->selection->order, (QList<Family>{Family::Av1, Family::Hevc}));
        QCOMPARE(r->selection->encode, CodecPolicy::Mode::Hardware);
        QCOMPARE(r->selection->decode, CodecPolicy::Mode::Any);
    }

    void aMissingDecodersKeyFallsBackToTheOldDecodeMap()
    {
        const auto r = CodecRequest::parse(json(R"({"codecs":["hevc","av1"],"decode":{"hevc":"hw","av1":"sw"},"order":["hevc","av1","avc"],"decoders":{"av1":["sw"]}})"));
        QVERIFY(r && r->selection);
        QCOMPARE(r->selection->decodersOf(Family::Hevc), (CodecPolicy::DecoderPaths{true, false})); // from `decode`
        QCOMPARE(r->selection->decodersOf(Family::Av1), (CodecPolicy::DecoderPaths{false, true})); // from `decoders`
    }

    void namesAreTrimmedAndCaseInsensitiveAndUnknownKeysIgnored()
    {
        const auto r = CodecRequest::parse(json(R"({"order":[" AV1 ","Hevc"],"encode":" Hardware ","future":{"x":1}})"));
        QVERIFY(r && r->selection);
        QCOMPARE(r->selection->order, (QList<Family>{Family::Av1, Family::Hevc}));
        QCOMPARE(r->selection->encode, CodecPolicy::Mode::Hardware);
    }

    void malformedRecordsAreRefusedWithAReason_data()
    {
        QTest::addColumn<QByteArray>("record");
        QTest::newRow("order is not an array") << QByteArray(R"({"order":"av1"})");
        QTest::newRow("order object") << QByteArray(R"({"order":{"0":"av1"}})");
        QTest::newRow("order entry not a string") << QByteArray(R"({"order":["av1",2]})");
        QTest::newRow("unknown codec") << QByteArray(R"({"order":["vp9","avc"]})");
        QTest::newRow("unknown codec in the middle") << QByteArray(R"({"order":["av1","h265","avc"]})");
        QTest::newRow("avc420 is not a family name") << QByteArray(R"({"order":["avc420"]})");
        QTest::newRow("duplicate") << QByteArray(R"({"order":["av1","hevc","av1"]})");
        QTest::newRow("duplicate differing in case") << QByteArray(R"({"order":["av1","AV1"]})");
        QTest::newRow("four entries") << QByteArray(R"({"order":["av1","hevc","avc","avc"]})");
        QTest::newRow("oversized order") << QByteArray(R"({"order":)" + QJsonDocument(QJsonArray::fromStringList(QStringList(4000, QStringLiteral("av1")))).toJson(QJsonDocument::Compact) + "}");
        QTest::newRow("encode unknown") << QByteArray(R"({"order":["av1"],"encode":"gpu"})");
        QTest::newRow("encode not a string") << QByteArray(R"({"order":["av1"],"encode":3})");
        QTest::newRow("encode empty") << QByteArray(R"({"order":["av1"],"encode":""})");
        QTest::newRow("decodeMode unknown") << QByteArray(R"({"order":["av1"],"decodeMode":"cuda"})");
        QTest::newRow("decoders not an object") << QByteArray(R"({"decoders":["hw"]})");
        QTest::newRow("decoders unknown codec") << QByteArray(R"({"decoders":{"vp9":["sw"]}})");
        QTest::newRow("decoders value not an array") << QByteArray(R"({"decoders":{"av1":"sw"}})");
        QTest::newRow("decoders unknown path") << QByteArray(R"({"decoders":{"av1":["gpu"]}})");
        QTest::newRow("decoders path not a string") << QByteArray(R"({"decoders":{"av1":[1]}})");
        QTest::newRow("decoders duplicate path") << QByteArray(R"({"decoders":{"av1":["hw","hw"]}})");
        QTest::newRow("decoders three paths") << QByteArray(R"({"decoders":{"av1":["hw","sw","hw"]}})");
        QTest::newRow("decoders oversized") << QByteArray(R"({"decoders":{"av1":)" + QJsonDocument(QJsonArray::fromStringList(QStringList(5000, QStringLiteral("sw")))).toJson(QJsonDocument::Compact) + "}}");
        QTest::newRow("quality below 10") << QByteArray(R"({"quality":9})");
        QTest::newRow("quality zero") << QByteArray(R"({"quality":0})");
        QTest::newRow("quality above 100") << QByteArray(R"({"quality":101})");
        QTest::newRow("quality negative") << QByteArray(R"({"quality":-5})");
        QTest::newRow("quality fractional") << QByteArray(R"({"quality":55.5})");
        QTest::newRow("quality a string") << QByteArray(R"({"quality":"80"})");
        QTest::newRow("quality null") << QByteArray(R"({"quality":null})");
        QTest::newRow("quality bool") << QByteArray(R"({"quality":true})");
        QTest::newRow("quality array") << QByteArray(R"({"quality":[80]})");
        QTest::newRow("quality huge") << QByteArray(R"({"quality":1e300})");
        QTest::newRow("old codecs field still validated") << QByteArray(R"({"codecs":["vp9"],"order":["av1"]})");
    }

    void malformedRecordsAreRefusedWithAReason()
    {
        QFETCH(QByteArray, record);
        QString error;
        const auto r = CodecRequest::parse(QJsonDocument::fromJson(record).object(), &error);
        QVERIFY2(!r, qPrintable(QString::fromUtf8(record.left(100))));
        QVERIFY(!error.isEmpty());
        const auto reply = CodecRequest::invalidRecord(error);
        QCOMPARE(reply.value(QLatin1String("type")).toString(), QStringLiteral("error"));
        QCOMPARE(reply.value(QLatin1String("code")).toString(), QStringLiteral("invalid"));
        QCOMPARE(reply.value(QLatin1String("message")).toString(), error);
    }

    // OPT-066: `quality`, an integer from 10 to 100, optional, independent of the other fields.
    void qualityIsParsedAndOptional()
    {
        QCOMPARE(CodecRequest::parse(json(R"({"quality":10})"))->quality, std::optional<int>(10));
        QCOMPARE(CodecRequest::parse(json(R"({"quality":100})"))->quality, std::optional<int>(100));
        QCOMPARE(CodecRequest::parse(json(R"({"quality":72.0})"))->quality, std::optional<int>(72)); // a whole number written as a float
        QCOMPARE(CodecRequest::parse(json(R"({"codecs":["hevc"],"quality":55})"))->quality, std::optional<int>(55));
        const auto withOrder = CodecRequest::parse(json(R"({"order":["av1","avc"],"encode":"any","quality":40})"));
        QVERIFY(withOrder && withOrder->selection);
        QCOMPARE(withOrder->quality, std::optional<int>(40));
        QVERIFY(!CodecRequest::parse(json(R"({"codecs":["hevc"]})"))->quality); // absent = the host default
        // A quality alone is not an explicit selection.
        QVERIFY(!CodecRequest::parse(json(R"({"quality":40})"))->selection);
    }

    // ---- selection through a real stream ----

    void theClientsOrderIsHonoured()
    {
        Fixture f;
        auto reply = f.ask(R"({"codecs":["hevc","av1"],"order":["av1","hevc","avc"],"encode":"any","decodeMode":"software",
            "decoders":{"avc":["sw"],"hevc":["sw"],"av1":["sw"]}})", hal());
        QCOMPARE(reply.value(QLatin1String("selected")).toString(), QStringLiteral("av1"));
        QCOMPARE(reply.value(QLatin1String("backend")).toString(), QStringLiteral("hardware"));
        QCOMPARE(reply.value(QLatin1String("encoder")).toObject().value(QLatin1String("backend")).toString(), QStringLiteral("vaapi"));
        QCOMPARE(reply.value(QLatin1String("decodePath")).toString(), QStringLiteral("sw"));
        QCOMPARE(reply.value(QLatin1String("baseline")).toBool(true), false);
        QVERIFY(!reply.contains(QLatin1String("skipped")));
        QVERIFY2(f.lastLog.contains(QStringLiteral("order [av1,hevc,avc] encode any decode software")), qPrintable(f.lastLog));

        Fixture g;
        reply = g.ask(R"({"codecs":["hevc","av1"],"order":["hevc","av1","avc"],"encode":"any","decodeMode":"any"})", hal());
        QCOMPARE(reply.value(QLatin1String("selected")).toString(), QStringLiteral("hevc"));
    }

    void anOldClientsCodecOrderIsHonouredToo()
    {
        Fixture f;
        QCOMPARE(f.ask(R"({"codecs":["av1","hevc"]})", hal()).value(QLatin1String("selected")).toString(), QStringLiteral("av1"));
        Fixture g;
        QCOMPARE(g.ask(R"({"codecs":["hevc","av1"]})", hal()).value(QLatin1String("selected")).toString(), QStringLiteral("hevc"));
    }

    void theHostsSoftwareCeilingLimitsWhatTheClientAsksFor()
    {
        Fixture f;
        const auto reply = f.ask(R"({"order":["av1","hevc","avc"],"encode":"software","decoders":{"avc":["sw"],"hevc":["sw"],"av1":["sw"]}})", softwareOnly(),
                                 CodecPolicy::SoftwareAllowance{true, true, false});
        QCOMPARE(reply.value(QLatin1String("selected")).toString(), QStringLiteral("hevc"));
        QCOMPARE(reply.value(QLatin1String("backend")).toString(), QStringLiteral("software"));
        QCOMPARE(reply.value(QLatin1String("encoder")).toObject().value(QLatin1String("backend")).toString(), QStringLiteral("libx265"));
        QCOMPARE(whyOf(reply, QStringLiteral("av1")), (QStringList{QStringLiteral("softwareNotAllowed")}));
        QVERIFY(reply.value(QLatin1String("reason")).toString().contains(QStringLiteral("skipped av1 (softwareNotAllowed)")));
        QVERIFY(!reply.value(QLatin1String("baseline")).toBool());
    }

    void theCeilingIsDerivedFromSoftwareEncodingWhenNoneIsSet()
    {
        Fixture f;
        f.stream()->setEncoderPolicy(softwareOnly(), CodecPolicy::SoftwareEncoding::Never);
        f.stream()->setEncoderLabels(labels());
        const auto parsed = CodecRequest::parse(json(R"({"order":["av1","hevc","avc"],"encode":"any","decoders":{"avc":["sw"],"hevc":["sw"],"av1":["sw"]}})"));
        QVERIFY(parsed);
        const auto reply = CodecRequest::apply(*f.stream(), *parsed);
        QCOMPARE(reply.value(QLatin1String("selected")).toString(), QStringLiteral("avc")); // never: no software HEVC/AV1
        QVERIFY(reply.value(QLatin1String("baseline")).toBool());
        // Any: both ways are tried, so both reasons are reported (spec 4.1).
        const QStringList both{QStringLiteral("noHardwareEncoder"), QStringLiteral("softwareNotAllowed")};
        QCOMPARE(whyOf(reply, QStringLiteral("av1")), both);
        QCOMPARE(whyOf(reply, QStringLiteral("hevc")), both);
    }

    void nothingSatisfiableIsTheBaselineAndSaysSo()
    {
        Fixture f;
        // Sol has no AVC hardware; HEVC hardware only. Hardware-only AV1 first: AV1 has no hardware, HEVC cannot be decoded in hardware.
        const auto reply = f.ask(R"({"order":["av1","hevc","avc"],"encode":"hardware","decodeMode":"hardware",
            "decoders":{"avc":["sw"],"hevc":[],"av1":["hw"]}})", sol());
        QCOMPARE(reply.value(QLatin1String("selected")).toString(), QStringLiteral("avc"));
        QVERIFY(reply.value(QLatin1String("baseline")).toBool());
        QCOMPARE(reply.value(QLatin1String("backend")).toString(), QStringLiteral("software"));
        QCOMPARE(whyOf(reply, QStringLiteral("av1")), (QStringList{QStringLiteral("noHardwareEncoder")}));
        QCOMPARE(whyOf(reply, QStringLiteral("hevc")), (QStringList{QStringLiteral("clientCannotDecodeInHardware")}));
        QVERIFY(reply.value(QLatin1String("reason")).toString().contains(QStringLiteral("nothing in the codec list can be used")));
        QCOMPARE(reply.value(QLatin1String("encoder")).toObject().value(QLatin1String("backend")).toString(), QStringLiteral("libx264"));
    }

    void nvencNamesItsDeviceInTheReply()
    {
        Fixture f;
        const auto reply = f.ask(R"({"order":["hevc","avc"],"encode":"hardware","decoders":{"hevc":["hw"],"avc":["sw"]}})", sol());
        QCOMPARE(reply.value(QLatin1String("selected")).toString(), QStringLiteral("hevc"));
        const auto encoder = reply.value(QLatin1String("encoder")).toObject();
        QCOMPARE(encoder.value(QLatin1String("backend")).toString(), QStringLiteral("nvenc"));
        QCOMPARE(encoder.value(QLatin1String("device")).toString(), QStringLiteral("0000:09:00.0"));
        QCOMPARE(encoder.value(QLatin1String("name")).toString(), QStringLiteral("NVIDIA GeForce RTX 2070"));
        QCOMPARE(reply.value(QLatin1String("decodePath")).toString(), QStringLiteral("hw"));
        QVERIFY2(!reply.contains(QLatin1String("skipped")), "avc is ranked after hevc: it was never in the way");
    }

    void anEncoderThatFailsMovesOnAndTheDetailFollows()
    {
        Fixture f;
        auto reply = f.ask(R"({"order":["av1","hevc","avc"],"encode":"any"})", hal());
        QCOMPARE(reply.value(QLatin1String("selected")).toString(), QStringLiteral("av1"));
        f.stream()->privateCodecUnavailable(VideoCodec::Av1);
        const auto detail = f.stream()->codecDetail();
        QCOMPARE(detail.encoder, QStringLiteral("nvenc")); // hevc hardware now (the label set names NVENC for HEVC)
        QCOMPARE(detail.skipped.size(), 1);
        QCOMPARE(detail.skipped.first().codec, QStringLiteral("av1"));
        QVERIFY(!detail.baseline);
        const auto push = LayoutControl::codecRecord(QStringLiteral("hevc"), true, QStringLiteral("encoder unavailable"), detail);
        QCOMPARE(push.value(QLatin1String("skipped")).toArray().size(), 1);
    }

    void aDecodeModeOfSoftwareNeverAsksTheServerForHardwareDecodeOnlyPaths()
    {
        Fixture f;
        // The client can only decode HEVC in hardware and demands software decoding: HEVC is skipped for it.
        const auto reply = f.ask(R"({"order":["hevc","avc"],"encode":"any","decodeMode":"software","decoders":{"hevc":["hw"],"avc":["sw"]}})", hal());
        QCOMPARE(reply.value(QLatin1String("selected")).toString(), QStringLiteral("avc"));
        QCOMPARE(whyOf(reply, QStringLiteral("hevc")), (QStringList{QStringLiteral("clientCannotDecode")}));
    }

    void repliesStayBoundedForTheWorstCase()
    {
        LayoutControl::CodecDetail detail;
        detail.encoder = QString(100, QLatin1Char('x'));
        detail.device = QString(100, QLatin1Char('d'));
        detail.deviceName = QString(300, QLatin1Char('n'));
        for (int i = 0; i < 10; ++i) detail.skipped.append({QStringLiteral("av1"), QStringList(20, QStringLiteral("noHardwareEncoder"))});
        const auto record = LayoutControl::codecRecord(QStringLiteral("avc"), false, QStringLiteral("r"), detail);
        QCOMPARE(record.value(QLatin1String("encoder")).toObject().value(QLatin1String("name")).toString().size(), 48);
        QCOMPARE(record.value(QLatin1String("skipped")).toArray().size(), 3);
        QVERIFY(QJsonDocument(record).toJson(QJsonDocument::Compact).size() < 1024);
    }

    // ---- client quality (OPT-066) ----

    void theReplyAnswersTheEffectiveQualityAndEchoesTheRequest()
    {
        Fixture f;
        f.stream()->setQualityCap(75);
        auto reply = f.ask(R"({"order":["avc"],"quality":60})", hal());
        QCOMPARE(reply.value(QLatin1String("quality")).toInt(), 60);
        QCOMPARE(reply.value(QLatin1String("qualityRequested")).toInt(), 60);
        QCOMPARE(reply.value(QLatin1String("qualityEffect")).toString(), QStringLiteral("applied"));
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 60);
        QCOMPARE(f.stream()->statsSnapshot().qualityCap, 60);
        QCOMPARE(f.stream()->statsSnapshot().qualityRequested, std::optional<int>(60));
    }

    void aClientNeverExceedsTheHostsMaximum()
    {
        Fixture f;
        f.stream()->setQualityCap(75);
        auto reply = f.ask(R"({"order":["avc"],"quality":95})", hal());
        QCOMPARE(reply.value(QLatin1String("quality")).toInt(), 75); // ClientQualityMax 0 = the same as Quality
        QCOMPARE(reply.value(QLatin1String("qualityRequested")).toInt(), 95); // the echo is the request, not the result
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 75);
        QCOMPARE(f.stream()->statsSnapshot().qualityRequested, std::optional<int>(95));
        QVERIFY(f.stream()->clientQualityStatus().effective == 75);
    }

    void clientQualityMaxRaisesTheCeilingButNotTheDefault()
    {
        Fixture f;
        f.stream()->setQualityCap(60);
        f.stream()->setClientQualityMax(90);
        auto reply = f.ask(R"({"order":["avc"],"quality":85})", hal());
        QCOMPARE(reply.value(QLatin1String("quality")).toInt(), 85);
        reply = f.ask(R"({"order":["avc"],"quality":99})", hal()); // re-sent: settles first, the answer says what it will become
        QCOMPARE(reply.value(QLatin1String("quality")).toInt(), 90);
        QCOMPARE(reply.value(QLatin1String("qualityEffect")).toString(), QStringLiteral("pending"));
        Fixture g;
        g.stream()->setQualityCap(60);
        g.stream()->setClientQualityMax(90);
        QCOMPARE(g.ask(R"({"order":["avc"]})", hal()).value(QLatin1String("quality")).toInt(), 60); // no request: the host default
    }

    void aRequestBelowTheFloorIsRefusedNotClamped()
    {
        QString error;
        QVERIFY(!CodecRequest::parse(json(R"({"quality":5})"), &error));
        QCOMPARE(error, QStringLiteral("codec quality must be an integer from 10 to 100"));
    }

    void aReplyWithoutARequestHasNoRequestedField()
    {
        Fixture f;
        f.stream()->setQualityCap(70);
        const auto reply = f.ask(R"({"order":["avc"]})", hal());
        QCOMPARE(reply.value(QLatin1String("quality")).toInt(), 70);
        QVERIFY(!reply.contains(QLatin1String("qualityRequested")));
        QVERIFY(!reply.contains(QLatin1String("qualityEffect")));
        QVERIFY(!Stats::sampleRecord(f.stream()->statsSnapshot(), Stats::Snapshot{}, 1000, 1).value(QLatin1String("video")).toObject().contains(QLatin1String("qualityRequested")));
    }

    void aConsoleViewersRequestIsEchoedAndIgnored()
    {
        Fixture f;
        f.stream()->setQualityCap(80);
        f.stream()->setEncoderPolicy(hal(), CodecPolicy::SoftwareEncoding::Auto);
        const auto parsed = CodecRequest::parse(json(R"({"order":["avc"],"quality":30})"));
        QVERIFY(parsed);
        const auto reply = CodecRequest::apply(*f.stream(), *parsed, nullptr, /*controlling*/ false);
        QCOMPARE(reply.value(QLatin1String("quality")).toInt(), 80); // the value in use
        QCOMPARE(reply.value(QLatin1String("qualityRequested")).toInt(), 30);
        QCOMPARE(reply.value(QLatin1String("qualityEffect")).toString(), QStringLiteral("notController"));
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 80);
        // A connection that controlled and lost control drops its request.
        f.stream()->requestClientQuality(40);
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 40);
        f.stream()->requestClientQuality(40, false);
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 80);
    }

    void adaptiveQualityStepsBelowTheEffectiveCapNeverAbove()
    {
        Fixture f;
        f.stream()->setAdaptiveQuality(true);
        f.stream()->setQualityCap(80);
        f.stream()->requestClientQuality(50);
        QCOMPARE(int(f.stream()->statsSnapshot().quality), 50);
        QCOMPARE(int(f.stream()->statsSnapshot().qualityCap), 50);
        f.stream()->setQualityCap(70); // a host change applies at once and keeps the request
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 50);
        QVERIFY(f.stream()->statsSnapshot().quality <= 50);
    }

    void aHostChangeAppliesAtOnceAndCancelsASettlingRequest()
    {
        Fixture f;
        f.stream()->setQualityCap(80);
        f.stream()->requestClientQuality(70); // first: at once
        const auto pending = f.stream()->requestClientQuality(40); // re-sent: settles
        QCOMPARE(pending.effect, ClientQuality::Effect::Pending);
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 70);
        f.stream()->setClientQualityMax(0); // host change: recomputed now with the stored request (40)
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 40);
    }

    // A slider: the first value applies at once, later ones wait for the value to settle and for MinApplyInterval.
    void aReSentQualityIsAppliedOnceItSettles()
    {
        Fixture f;
        f.stream()->setAdaptiveQuality(false); // fixed quality: the cap is applied outright (adaptive climbs towards it)
        f.stream()->setQualityCap(100);
        QSignalSpy spy(f.stream(), &VideoStream::requestedQualityChanged);
        QCOMPARE(f.stream()->requestClientQuality(50).effect, ClientQuality::Effect::Applied);
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 50);
        spy.clear();
        for (int q = 52; q <= 60; q += 2) {
            QCOMPARE(f.stream()->requestClientQuality(q).effect, ClientQuality::Effect::Pending); // slider ticks
            QTest::qWait(50);
        }
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 50); // nothing applied yet
        QCOMPARE(spy.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(int(f.stream()->effectiveQualityCap()), 60, 4000);
        QCOMPARE(spy.count(), 1); // one reopen for the whole drag, with the latest value
        QCOMPARE(int(spy.last().at(0).value<quint8>()), 60);
    }

    // OPT-066 observability: the settle timer pushes the result, so "pending" turns into "applied" on the client.
    void theSettleTimerPushesTheAppliedResult()
    {
        Fixture f;
        f.stream()->setAdaptiveQuality(false);
        f.stream()->setQualityCap(100);
        QSignalSpy pushed(f.stream(), &VideoStream::clientQualityApplied);
        QCOMPARE(f.stream()->requestClientQuality(50).effect, ClientQuality::Effect::Applied);
        QCOMPARE(pushed.count(), 0); // the first request is answered by the reply itself
        QCOMPARE(f.stream()->requestClientQuality(70).effect, ClientQuality::Effect::Pending);
        QCOMPARE(pushed.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(pushed.count(), 1, 4000);
        const auto record = pushed.last().at(0).toJsonObject();
        QCOMPARE(record.value(QLatin1String("type")).toString(), QStringLiteral("codec"));
        QVERIFY(!record.contains(QLatin1String("requestId"))); // unsolicited
        QCOMPARE(record.value(QLatin1String("quality")).toInt(), 70);
        QCOMPARE(record.value(QLatin1String("qualityRequested")).toInt(), 70);
        QCOMPARE(record.value(QLatin1String("qualityEffect")).toString(), QStringLiteral("applied"));
        QCOMPARE(record.value(QLatin1String("reason")).toString(), QStringLiteral("client quality applied"));
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 70);
    }

    void aPushedResultForAClampedRequestCarriesTheHostCap()
    {
        Fixture f;
        f.stream()->setAdaptiveQuality(false);
        f.stream()->setQualityCap(75);
        QSignalSpy pushed(f.stream(), &VideoStream::clientQualityApplied);
        f.stream()->requestClientQuality(40);
        f.stream()->requestClientQuality(95); // above the ceiling (75)
        QTRY_COMPARE_WITH_TIMEOUT(pushed.count(), 1, 4000);
        const auto record = pushed.last().at(0).toJsonObject();
        QCOMPARE(record.value(QLatin1String("quality")).toInt(), 75);
        QCOMPARE(record.value(QLatin1String("qualityRequested")).toInt(), 95);
    }

    void aReSendWithoutQualityIsTheHostDefaultAndPushesWithoutARequest()
    {
        Fixture f;
        f.stream()->setAdaptiveQuality(false);
        f.stream()->setQualityCap(75);
        f.ask(R"({"order":["avc"],"quality":40})", hal());
        QVERIFY(!f.lastLog.contains(QStringLiteral("host default")));
        QVERIFY(f.lastLog.contains(QStringLiteral("quality asked 40, applied 40")));
        QSignalSpy pushed(f.stream(), &VideoStream::clientQualityApplied);
        const auto reply = f.ask(R"({"order":["avc"]})", hal()); // re-sent without quality
        QVERIFY2(f.lastLog.contains(QStringLiteral("quality host default (75)")), qPrintable(f.lastLog));
        QVERIFY(!reply.contains(QLatin1String("qualityRequested")));
        QTRY_COMPARE_WITH_TIMEOUT(pushed.count(), 1, 4000);
        const auto record = pushed.last().at(0).toJsonObject();
        QCOMPARE(record.value(QLatin1String("quality")).toInt(), 75);
        QVERIFY(!record.contains(QLatin1String("qualityRequested")));
        QVERIFY(!record.contains(QLatin1String("qualityEffect")));
    }

    void aViewerNeverPushesAnAppliedQuality()
    {
        Fixture f;
        f.stream()->setQualityCap(80);
        QSignalSpy pushed(f.stream(), &VideoStream::clientQualityApplied);
        f.stream()->requestClientQuality(40, false);
        f.stream()->requestClientQuality(30, false);
        QTest::qWait(2800);
        QCOMPARE(pushed.count(), 0);
        QCOMPARE(int(f.stream()->effectiveQualityCap()), 80);
    }

    // ---- capabilities ----

    void capabilitiesAdvertisePreferencesCeilingsAndEncoders()
    {
        EncoderSupport::Inputs in;
        in.vaapi = {true, true, false, QStringLiteral("/dev/dri/renderD128")};
        in.software = {true, true, true};
        const auto probe = EncoderSupport::assemble(in);
        const auto video = EncoderSupport::videoCapabilities(probe, CodecPolicy::SoftwareEncoding::Auto, CodecPolicy::SoftwareAllowance{true, true, false});
        LayoutControl::ChannelCapabilities caps;
        caps.host = QStringLiteral("console");
        caps.video = video;
        const auto record = LayoutControl::capabilitiesRecord(caps).value(QLatin1String("video")).toObject();
        QCOMPARE(record.value(QLatin1String("preferences")).toInt(), 1);
        QCOMPARE(record.value(QLatin1String("clientQuality")).toInt(), 1); // OPT-066
        const auto software = record.value(QLatin1String("software")).toObject();
        QCOMPARE(software.value(QLatin1String("avc")).toString(), QStringLiteral("allowed"));
        QCOMPARE(software.value(QLatin1String("hevc")).toString(), QStringLiteral("allowed"));
        QCOMPARE(software.value(QLatin1String("av1")).toString(), QStringLiteral("never"));
        // av1: no hardware here and the ceiling forbids software, so it is not offered; hevc offers both.
        QStringList names;
        for (const auto &c : record.value(QLatin1String("codecs")).toArray()) names << c.toObject().value(QLatin1String("name")).toString();
        QVERIFY2(names.contains(QStringLiteral("hevc")) && !names.contains(QStringLiteral("av1")), qPrintable(names.join(QLatin1Char(','))));
        // encoders: vaapi avc/hevc, software avc/hevc, no software av1 (forbidden), no hardware av1.
        QStringList encoders;
        for (const auto &e : record.value(QLatin1String("encoders")).toArray())
            encoders << e.toObject().value(QLatin1String("codec")).toString() + u'/' + e.toObject().value(QLatin1String("backend")).toString();
        QCOMPARE(encoders, (QStringList{QStringLiteral("avc/vaapi"), QStringLiteral("avc/libx264"), QStringLiteral("hevc/vaapi"), QStringLiteral("hevc/libx265")}));
    }

    void aStrictAvcCeilingStillOffersSoftwareH264AsTheLastResort()
    {
        EncoderSupport::Inputs in;
        in.software = {true, true, true};
        const auto probe = EncoderSupport::assemble(in);
        const auto video = EncoderSupport::videoCapabilities(probe, CodecPolicy::SoftwareEncoding::Never, CodecPolicy::SoftwareAllowance{false, false, false});
        QCOMPARE(video.softwareAvc, QStringLiteral("lastResort"));
        QCOMPARE(video.softwareHevc, QStringLiteral("never"));
        QVERIFY(video.codecs.first().software); // avc420 keeps its software encoder
        QCOMPARE(video.codecs.size(), 1);
        QCOMPARE(video.encoders.size(), 1);
        QVERIFY(!video.encoders.first().hardware);
    }

    void anOldHostOfferNeverAdvertisesPreferences()
    {
        EncoderSupport::Inputs in;
        const auto probe = EncoderSupport::assemble(in);
        LayoutControl::ChannelCapabilities caps;
        caps.video = EncoderSupport::videoCapabilities(probe, CodecPolicy::SoftwareEncoding::Auto); // the two-argument form is the old one
        const auto record = LayoutControl::capabilitiesRecord(caps).value(QLatin1String("video")).toObject();
        QVERIFY(!record.contains(QLatin1String("preferences")));
        QVERIFY(!record.contains(QLatin1String("software")));
        QVERIFY(!record.contains(QLatin1String("encoders")));
    }

    void theProbeNamesTheEncodersBehindEachBackend()
    {
        EncoderSupport::Inputs in;
        in.vaapi = {true, true, true, QStringLiteral("/dev/dri/renderD128")};
        in.softwareBackend = {QStringLiteral("libopenh264"), QStringLiteral("libx265"), QStringLiteral("libaom-av1")};
        const auto probe = EncoderSupport::assemble(in);
        QCOMPARE(probe.labels.of(Family::Avc, true).backend, QStringLiteral("vaapi"));
        QCOMPARE(probe.labels.of(Family::Avc, false).backend, QStringLiteral("libopenh264"));
        QCOMPARE(probe.labels.of(Family::Av1, false).backend, QStringLiteral("libaom-av1"));

        EncoderSupport::Inputs nv; // an NVIDIA-only host: HEVC from NVENC with its PCI id and name
        EncoderSupport::NvidiaEncoder encoder;
        encoder.pciId = QStringLiteral("0000:09:00.0");
        encoder.name = QStringLiteral("NVIDIA GeForce RTX 2070");
        encoder.family = Family::Hevc;
        encoder.usable = true;
        nv.nvidia = {encoder};
        const auto nvProbe = EncoderSupport::assemble(nv);
        QCOMPARE(nvProbe.labels.of(Family::Hevc, true).backend, QStringLiteral("nvenc"));
        QCOMPARE(nvProbe.labels.of(Family::Hevc, true).device, QStringLiteral("0000:09:00.0"));
        QCOMPARE(nvProbe.labels.of(Family::Hevc, true).deviceName, QStringLiteral("NVIDIA GeForce RTX 2070"));
    }

    // ---- stats ----

    void theStatsSampleNamesTheEncoderTheRequestAndTheCeiling()
    {
        Fixture f;
        f.ask(R"({"order":["hevc","av1","avc"],"encode":"hardware","decodeMode":"any","decoders":{"hevc":["hw"],"av1":["sw"],"avc":["sw"]}})", sol(),
              CodecPolicy::SoftwareAllowance{true, true, false});
        const auto snapshot = f.stream()->statsSnapshot();
        QCOMPARE(snapshot.encoder, QStringLiteral("nvenc"));
        QCOMPARE(snapshot.device, QStringLiteral("0000:09:00.0"));
        const auto sample = Stats::sampleRecord(snapshot, Stats::Snapshot{}, 1000, 1);
        const auto video = sample.value(QLatin1String("video")).toObject();
        QCOMPARE(video.value(QLatin1String("encoder")).toString(), QStringLiteral("nvenc"));
        QCOMPARE(video.value(QLatin1String("device")).toString(), QStringLiteral("0000:09:00.0"));
        QCOMPARE(video.value(QLatin1String("deviceName")).toString(), QStringLiteral("NVIDIA GeForce RTX 2070"));
        const auto policy = sample.value(QLatin1String("policy")).toObject();
        QCOMPARE(policy.value(QLatin1String("order")).toArray().size(), 3);
        QCOMPARE(policy.value(QLatin1String("order")).toArray().first().toString(), QStringLiteral("hevc"));
        QCOMPARE(policy.value(QLatin1String("encode")).toString(), QStringLiteral("hardware"));
        QCOMPARE(policy.value(QLatin1String("decodeMode")).toString(), QStringLiteral("any"));
        QCOMPARE(policy.value(QLatin1String("allowance")).toObject().value(QLatin1String("av1")).toString(), QStringLiteral("never"));
        QCOMPARE(policy.value(QLatin1String("allowance")).toObject().value(QLatin1String("avc")).toString(), QStringLiteral("allowed"));
    }

    void aSampleWithEveryNewFieldAnd16SurfacesStaysUnder2KiB()
    {
        Stats::Snapshot s;
        s.codec = QStringLiteral("av1");
        s.hardware = true;
        s.encoder = QString(24, QLatin1Char('e'));
        s.device = QString(16, QLatin1Char('d'));
        s.deviceName = QString(48, QLatin1Char('n'));
        s.order = {QStringLiteral("hevc"), QStringLiteral("av1"), QStringLiteral("avc")};
        s.encodeMode = QStringLiteral("hardware");
        s.decodeMode = QStringLiteral("software");
        s.allowance = CodecPolicy::SoftwareAllowance{false, false, false};
        s.mode = QStringLiteral("prefer");
        s.adaptive = true;
        s.size = QSize(5120, 1440);
        s.heldBack = {QStringLiteral("hevc"), QStringLiteral("av1")};
        s.retryInS = 99;
        s.quality = 100;
        s.qualityCap = 100;
        s.qualityRequested = 100;
        for (int i = 0; i < 16; ++i) s.surfaces.append({quint64(i) * 1000, 3, 1});
        const auto bytes = QJsonDocument(Stats::sampleRecord(s, Stats::Snapshot{}, 1000, 123456)).toJson(QJsonDocument::Compact);
        QVERIFY2(bytes.size() < 2048, qPrintable(QString::number(bytes.size())));
    }

    void aStreamWithoutARequestSaysNothingAboutOrderOrEncode()
    {
        Fixture f;
        f.stream()->setEncoderPolicy(hal(), CodecPolicy::SoftwareEncoding::Auto);
        const auto sample = Stats::sampleRecord(f.stream()->statsSnapshot(), Stats::Snapshot{}, 1000, 1);
        const auto policy = sample.value(QLatin1String("policy")).toObject();
        QVERIFY(!policy.contains(QLatin1String("order")));
        QVERIFY(!policy.contains(QLatin1String("encode")));
        QVERIFY(policy.contains(QLatin1String("allowance"))); // the host's ceiling is always known
    }
};

QTEST_GUILESS_MAIN(CodecRequestTest)
#include "CodecRequestTest.moc"
