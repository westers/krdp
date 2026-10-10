// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-062 S3: the host's per-codec software ceiling (`SoftwareAvc` / `SoftwareHevc` / `SoftwareAv1`): parsing, the
// derivation from the old SoftwareEncoding, the clamp by a user's own preference (it can only tighten), and what a
// connection's stream is seeded with. Pure; no encoder is opened.

#include "RdpConnection.h"
#include "Server.h"
#include "VideoCodecHost.h"

#include <QCoreApplication>
#include <QTest>

using namespace KRdp;
using CodecPolicy::CeilingSetting;
using CodecPolicy::Family;
using CodecPolicy::SoftwareEncoding;

class VideoCodecHostTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void ceilingValuesParseAndRefuseTheOtherFamiliesWord_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<int>("family");
        QTest::addColumn<int>("expected"); // -1 = refused
        const int a = int(Family::Avc), h = int(Family::Hevc), v = int(Family::Av1);
        QTest::newRow("empty is auto") << QString() << h << int(CeilingSetting::Auto);
        QTest::newRow("auto") << QStringLiteral("auto") << a << int(CeilingSetting::Auto);
        QTest::newRow("allowed avc") << QStringLiteral("allowed") << a << int(CeilingSetting::Allowed);
        QTest::newRow("last-resort avc") << QStringLiteral("last-resort") << a << int(CeilingSetting::Restricted);
        QTest::newRow("never avc refused: software H.264 is only ever a last resort") << QStringLiteral("never") << a << -1;
        QTest::newRow("never hevc") << QStringLiteral(" Never ") << h << int(CeilingSetting::Restricted);
        QTest::newRow("last-resort hevc refused") << QStringLiteral("last-resort") << h << -1;
        QTest::newRow("never av1") << QStringLiteral("never") << v << int(CeilingSetting::Restricted);
        QTest::newRow("junk") << QStringLiteral("sometimes") << v << -1;
    }
    void ceilingValuesParseAndRefuseTheOtherFamiliesWord()
    {
        QFETCH(QString, value);
        QFETCH(int, family);
        QFETCH(int, expected);
        const auto parsed = parseHostCeiling(value, Family(family));
        if (expected < 0) {
            QVERIFY(!parsed);
        } else {
            QVERIFY(parsed);
            QCOMPARE(int(*parsed), expected);
        }
    }

    void autoFollowsTheOldSoftwareEncodingSetting()
    {
        const auto autoAll = [](SoftwareEncoding mode) {
            return resolveHostCeiling(mode, CeilingSetting::Auto, CeilingSetting::Auto, CeilingSetting::Auto);
        };
        QCOMPARE(autoAll(SoftwareEncoding::Auto), (CodecPolicy::SoftwareAllowance{true, true, true}));
        QCOMPARE(autoAll(SoftwareEncoding::Prefer), (CodecPolicy::SoftwareAllowance{true, true, true}));
        QCOMPARE(autoAll(SoftwareEncoding::Never), (CodecPolicy::SoftwareAllowance{false, false, false})); // AVC: last resort
        // An explicit per-codec value wins over the old setting, in both directions.
        QCOMPARE(resolveHostCeiling(SoftwareEncoding::Never, CeilingSetting::Allowed, CeilingSetting::Allowed, CeilingSetting::Restricted),
                 (CodecPolicy::SoftwareAllowance{true, true, false}));
        QCOMPARE(resolveHostCeiling(SoftwareEncoding::Auto, CeilingSetting::Restricted, CeilingSetting::Restricted, CeilingSetting::Auto),
                 (CodecPolicy::SoftwareAllowance{false, false, true}));
    }

    void aUsersPreferenceCanOnlyTightenTheCeiling()
    {
        VideoCodecHost host;
        host.mode = SoftwareEncoding::Auto;
        host.ceiling = CodecPolicy::SoftwareAllowance{true, true, false};
        QCOMPARE(host.allowance(), (CodecPolicy::SoftwareAllowance{true, true, false}));
        // `prefer` (software allowed) cannot reopen what the host forbids (spec C5).
        QCOMPARE(host.allowance(SoftwareEncoding::Prefer), (CodecPolicy::SoftwareAllowance{true, true, false}));
        QCOMPARE(host.allowance(SoftwareEncoding::Auto), (CodecPolicy::SoftwareAllowance{true, true, false}));
        // `never` narrows further.
        QCOMPARE(host.allowance(SoftwareEncoding::Never), (CodecPolicy::SoftwareAllowance{false, false, false}));
        const auto user = host.withUser(SoftwareEncoding::Never);
        QCOMPARE(user.mode, SoftwareEncoding::Never);
        QCOMPARE(user.allowance(), (CodecPolicy::SoftwareAllowance{false, false, false}));
        QCOMPARE(host.withUser(std::nullopt).allowance(), host.allowance());
        // Without an explicit ceiling the old mode derives it.
        VideoCodecHost legacy;
        legacy.mode = SoftwareEncoding::Never;
        QCOMPARE(legacy.allowance(), (CodecPolicy::SoftwareAllowance{false, false, false}));
        QCOMPARE(legacy.allowance(SoftwareEncoding::Prefer), (CodecPolicy::SoftwareAllowance{false, false, false}));
    }

    void applySeedsAConnectionsStream()
    {
        Server server;
        RdpConnection connection(&server, -1);
        QCoreApplication::removePostedEvents(&connection, QEvent::MetaCall);
        VideoCodecHost host;
        host.probe.encoders.hevc = {false, true, false};
        host.probe.labels.software[size_t(Family::Hevc)] = {QStringLiteral("libx265"), {}, {}};
        host.ceiling = CodecPolicy::SoftwareAllowance{true, false, true};
        host.apply(*connection.videoStream(), SoftwareEncoding::Prefer);
        QCOMPARE(connection.videoStream()->softwareAllowance(), (CodecPolicy::SoftwareAllowance{true, false, true}));
        QCOMPARE(connection.videoStream()->softwareEncoding(), SoftwareEncoding::Prefer);
        QVERIFY(connection.videoStream()->encoderPolicy().hevc.software);
    }

    void publicEncoderListShowsEverythingTheProbeFoundWhateverTheCeilingSays()
    {
        EncoderSupport::Probe probe;
        probe.encoders.avc = {false, true, false};
        probe.encoders.hevc = {true, true, false};
        probe.labels.hardware[size_t(Family::Hevc)] = {QStringLiteral("nvenc"), QStringLiteral("0000:09:00.0"), QStringLiteral("NVIDIA GeForce RTX 2070")};
        probe.labels.software[size_t(Family::Hevc)] = {QStringLiteral("libx265"), {}, {}};
        probe.labels.software[size_t(Family::Avc)] = {QStringLiteral("libx264"), {}, {}};
        const auto list = publicVideoEncoders(probe);
        QCOMPARE(list.size(), 3);
        QCOMPARE(list.at(0).toObject().value(QStringLiteral("backend")).toString(), QStringLiteral("libx264"));
        const auto hardware = list.at(1).toObject();
        QCOMPARE(hardware.value(QStringLiteral("codec")).toString(), QStringLiteral("hevc"));
        QCOMPARE(hardware.value(QStringLiteral("device")).toString(), QStringLiteral("0000:09:00.0"));
        QVERIFY(hardware.value(QStringLiteral("hw")).toBool());
        QCOMPARE(list.at(2).toObject().value(QStringLiteral("backend")).toString(), QStringLiteral("libx265"));
    }
};

QTEST_GUILESS_MAIN(VideoCodecHostTest)
#include "VideoCodecHostTest.moc"
