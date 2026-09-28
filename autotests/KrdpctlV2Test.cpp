// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// KRDPCTL protocol v2 (~/dev/rdp/KRDPCTL-V2-CONTRACT.md): the requestId rules, the
// `capabilities` record, and the per-connection chroma timing (ChromaMerge).

#include <QJsonObject>
#include <QJsonArray>
#include <QTest>

#include "ChromaMerge.h"
#include "LayoutControl.h"

using namespace KRdp;
using namespace Qt::StringLiterals;

namespace
{
struct FakeSession {
    QList<ChromaPolicy> received;
    void setChromaPolicy(const ChromaPolicy &policy)
    {
        received.append(policy);
    }
};
}

class KrdpctlV2Test : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void requestIdIsTakenOffAndEchoed()
    {
        QJsonObject record{{u"type"_s, u"query"_s}, {u"v"_s, 1}, {u"requestId"_s, u"r7"_s}};
        const auto id = LayoutControl::takeRequestId(record);
        QVERIFY(!id.invalid);
        QCOMPARE(id.value, u"r7"_s);
        QVERIFY(!record.contains(u"requestId"_s)); // the v1 parsers see the v1 shape
        QCOMPARE(record.size(), 2);
        const auto reply = LayoutControl::withRequestId(LayoutControl::errorRecord({u"unsupported"_s, u"x"_s}), id.value);
        QCOMPARE(reply.value(u"requestId"_s).toString(), u"r7"_s);
        QCOMPARE(reply.value(u"type"_s).toString(), u"error"_s);
        // No requestId: processed, replies uncorrelated.
        QJsonObject plain{{u"type"_s, u"query"_s}};
        const auto none = LayoutControl::takeRequestId(plain);
        QVERIFY(!none.invalid && none.value.isEmpty());
        QVERIFY(!LayoutControl::withRequestId(QJsonObject{{u"type"_s, u"layout"_s}}, none.value).contains(u"requestId"_s));
    }

    void invalidRequestIdsAreRefused_data()
    {
        QTest::addColumn<QJsonValue>("requestId");
        QTest::addColumn<QJsonValue>("id");
        QTest::newRow("not a string") << QJsonValue(7) << QJsonValue();
        QTest::newRow("empty") << QJsonValue(QString()) << QJsonValue();
        QTest::newRow("bad characters") << QJsonValue(u"a b"_s) << QJsonValue();
        QTest::newRow("too long") << QJsonValue(QString(65, u'a')) << QJsonValue();
        QTest::newRow("differs from id") << QJsonValue(u"a"_s) << QJsonValue(u"b"_s);
    }
    void invalidRequestIdsAreRefused()
    {
        QFETCH(QJsonValue, requestId);
        QFETCH(QJsonValue, id);
        QJsonObject record{{u"type"_s, u"virtual-session"_s}, {u"requestId"_s, requestId}};
        if (!id.isUndefined() && !id.isNull()) record.insert(u"id"_s, id);
        QVERIFY(LayoutControl::takeRequestId(record).invalid);
        const auto refusal = LayoutControl::invalidRequestIdRecord();
        QCOMPARE(refusal.value(u"code"_s).toString(), u"invalid"_s);
        QVERIFY(!refusal.contains(u"requestId"_s));
    }

    void requestIdEqualToIdIsFine()
    {
        QJsonObject record{{u"type"_s, u"topology-query"_s}, {u"v"_s, 1}, {u"id"_s, u"q-1"_s}, {u"requestId"_s, u"q-1"_s}};
        const auto id = LayoutControl::takeRequestId(record);
        QVERIFY(!id.invalid);
        QCOMPARE(record.size(), 3); // topology-query's strict size check still holds
    }

    void capabilitiesRecordShape()
    {
        LayoutControl::ChannelCapabilities caps;
        caps.host = u"physical"_s;
        caps.layoutQuery = caps.layoutApply = true;
        const auto record = LayoutControl::capabilitiesRecord(caps);
        QCOMPARE(record.value(u"type"_s).toString(), u"capabilities"_s);
        QCOMPARE(record.value(u"protocol"_s).toInt(), LayoutControl::ChannelProtocol);
        QCOMPARE(record.value(u"protocol"_s).toInt(), 2);
        QCOMPARE(record.value(u"host"_s).toString(), u"physical"_s);
        QCOMPARE(record.value(u"layout"_s).toObject().value(u"apply"_s).toBool(), true);
        QCOMPARE(record.value(u"virtualSessions"_s).toObject().size(), 3);
        QCOMPARE(record.value(u"topology"_s).toObject().size(), 3);
        QVERIFY(!record.contains(u"requestId"_s)); // unsolicited
        QVERIFY(!record.contains(u"video"_s)); // optional group, brokers leave it out
    }

    // AUD-FIX2 F1: krdpserver advertises the codecs it can really encode, per backend.
    void capabilitiesVideoGroup()
    {
        LayoutControl::ChannelCapabilities caps;
        caps.host = u"physical"_s;
        caps.video = LayoutControl::VideoCapabilities{{{u"avc420"_s, false, true}, {u"hevc"_s, true, false}}, u"auto"_s};
        const auto video = LayoutControl::capabilitiesRecord(caps).value(u"video"_s).toObject();
        QCOMPARE(video.value(u"softwareEncoding"_s).toString(), u"auto"_s);
        const auto codecs = video.value(u"codecs"_s).toArray();
        QCOMPARE(codecs.size(), 2);
        QCOMPARE(codecs.at(0).toObject(), (QJsonObject{{u"name"_s, u"avc420"_s}, {u"hw"_s, false}, {u"sw"_s, true}}));
        QCOMPARE(codecs.at(1).toObject(), (QJsonObject{{u"name"_s, u"hevc"_s}, {u"hw"_s, true}, {u"sw"_s, false}}));
    }

    void codecRecordShape()
    {
        const auto reply = LayoutControl::withRequestId(LayoutControl::codecRecord(u"avc"_s, false, u"no usable encoder for hevc on this host"_s), u"r1"_s);
        QCOMPARE(reply.value(u"type"_s).toString(), u"codec"_s);
        QCOMPARE(reply.value(u"ok"_s).toBool(), true);
        QCOMPARE(reply.value(u"selected"_s).toString(), u"avc"_s);
        QCOMPARE(reply.value(u"backend"_s).toString(), u"software"_s);
        QCOMPARE(reply.value(u"requestId"_s).toString(), u"r1"_s);
        QVERIFY(reply.value(u"reason"_s).toString().contains(u"hevc"_s));
        const auto push = LayoutControl::codecRecord(u"av1"_s, true);
        QCOMPARE(push.value(u"backend"_s).toString(), u"hardware"_s);
        QVERIFY(!push.contains(u"reason"_s));
        QVERIFY(!push.contains(u"requestId"_s));
    }

    // A stock client never sends `chroma`: its connection keeps the krdpserverrc default.
    void withoutChromaTheConfiguredDefaultStays()
    {
        const ChromaPolicy configured{40, 300, 1000}; // as read from krdpserverrc
        ChromaPolicy connection = configured;         // SessionController: wrapper->m_chromaPolicy = m_chromaPolicyDefault
        QList<FakeSession *> sessions;
        FakeSession running;
        sessions.append(&running);
        // Nothing arrives; a malformed record changes nothing either.
        const auto malformed = ChromaMerge::apply(connection, std::nullopt, sessions);
        QCOMPARE(malformed.outcome, ChromaMerge::Outcome::Malformed);
        QCOMPARE(connection, configured);
        QVERIFY(running.received.isEmpty());
    }

    // KRDPCTL opens only after login, so `chroma` normally arrives while already streaming:
    // it takes effect on the running sessions at once, and on any built later.
    void chromaAfterStreamingTakesEffect()
    {
        ChromaPolicy connection{100, 150, 1500};
        FakeSession first, second;
        QList<FakeSession *> sessions{&first, &second};
        LayoutControl::ChromaRequest request;
        request.restMs = 400; // partial: the others keep the connection's values
        const auto result = ChromaMerge::apply(connection, request, sessions);
        QCOMPARE(result.outcome, ChromaMerge::Outcome::Applied);
        const ChromaPolicy expected{100, 400, 1500};
        QCOMPARE(connection, expected);
        QCOMPARE(first.received, QList<ChromaPolicy>{expected});
        QCOMPARE(second.received, QList<ChromaPolicy>{expected});
        // An out-of-range one is refused and touches nothing.
        LayoutControl::ChromaRequest bad;
        bad.motionGapMs = 2000; // > restMs
        QCOMPARE(ChromaMerge::apply(connection, bad, sessions).outcome, ChromaMerge::Outcome::OutOfRange);
        QCOMPARE(connection, expected);
        QCOMPARE(first.received.size(), 1);
    }
};

QTEST_GUILESS_MAIN(KrdpctlV2Test)
#include "KrdpctlV2Test.moc"
