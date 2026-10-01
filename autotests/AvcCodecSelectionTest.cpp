// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "AvcCodecSelection.h"
#include <QTest>
#include <thread>

using namespace KRdp;

class AvcCodecSelectionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void lateAvailabilityNeverExpandsClientFormats_data()
    {
        QTest::addColumn<uint32_t>("version");
        QTest::addColumn<uint32_t>("flags");
        QTest::addColumn<int>("preferred");
        QTest::newRow("8.1") << uint32_t(RDPGFX_CAPVERSION_81) << uint32_t(RDPGFX_CAPS_FLAG_AVC420_ENABLED) << int(VideoCodec::Avc420);
        QTest::newRow("10.0") << uint32_t(RDPGFX_CAPVERSION_10) << uint32_t(0) << int(VideoCodec::Avc444);
        QTest::newRow("10.2") << uint32_t(RDPGFX_CAPVERSION_102) << uint32_t(0) << int(VideoCodec::Avc444v2);
        QTest::newRow("10.7") << uint32_t(RDPGFX_CAPVERSION_107) << uint32_t(0) << int(VideoCodec::Avc444v2);
    }
    void lateAvailabilityNeverExpandsClientFormats()
    {
        QFETCH(uint32_t, version); QFETCH(uint32_t, flags); QFETCH(int, preferred);
        AvcCodecSelection selection;
        selection.setAvc444Available(false);
        QCOMPARE(selection.forSessions(), VideoCodec::Avc420);
        QVERIFY(!selection.negotiated());
        QVERIFY(selection.acceptCaps(version, flags)); // First confirmation is notified even if codec stays420.
        QCOMPARE(selection.negotiated(), std::optional(VideoCodec::Avc420));
        selection.setPreference(CodecPreference::Avc444);
        QCOMPARE(selection.forSessions(), VideoCodec::Avc420);
        selection.setAvc444Available(true);
        QCOMPARE(selection.forSessions(), VideoCodec(preferred));
        QVERIFY(!selection.setAvc444Available(true)); // No unnecessary refresh.
        selection.setPreference(CodecPreference::Avc420);
        QCOMPARE(selection.forSessions(), VideoCodec::Avc420);
        selection.setPreference(CodecPreference::Auto);
        QCOMPARE(selection.forSessions(), VideoCodec(preferred));
        selection.setAvc444Available(false);
        QCOMPARE(selection.forSessions(), VideoCodec::Avc420);
    }
    void privateCodecRetainsAuthorityAndFallsBackToCurrentPolicy()
    {
        AvcCodecSelection selection;
        selection.acceptCaps(RDPGFX_CAPVERSION_107, 0);
        selection.setPrivateCodec(VideoCodec::Hevc);
        QVERIFY(!selection.setPreference(CodecPreference::Avc420));
        QVERIFY(!selection.setAvc444Available(false));
        QVERIFY(!selection.acceptCaps(RDPGFX_CAPVERSION_81, RDPGFX_CAPS_FLAG_AVC420_ENABLED));
        QCOMPARE(selection.forSessions(), VideoCodec::Hevc);
        QCOMPARE(selection.setPrivateCodec({}), std::optional(VideoCodec::Avc420));
        selection.setAvc444Available(true);
        selection.setPreference(CodecPreference::Avc444);
        QCOMPARE(selection.forSessions(), VideoCodec::Avc420); // Original client still cannot decode444.
        selection.acceptCaps(RDPGFX_CAPVERSION_102, 0);
        QCOMPARE(selection.forSessions(), VideoCodec::Avc444v2);
    }
    void concurrentCapsAndPolicyKeepTheClientLimit()
    {
        AvcCodecSelection selection;
        selection.acceptCaps(RDPGFX_CAPVERSION_81, RDPGFX_CAPS_FLAG_AVC420_ENABLED);
        std::thread peer([&] {
            for (int i = 0; i < 2000; ++i) selection.acceptCaps(RDPGFX_CAPVERSION_81, RDPGFX_CAPS_FLAG_AVC420_ENABLED);
        });
        std::thread policy([&] {
            for (int i = 0; i < 2000; ++i) {
                selection.setPreference(i % 2 ? CodecPreference::Avc444 : CodecPreference::Auto);
                selection.setAvc444Available(i % 2);
            }
        });
        bool exceeded = false;
        for (int i = 0; i < 2000; ++i) exceeded |= selection.forSessions() != VideoCodec::Avc420;
        peer.join(); policy.join();
        QVERIFY(!exceeded);
        QCOMPARE(selection.negotiated(), std::optional(VideoCodec::Avc420));
    }
};
QTEST_GUILESS_MAIN(AvcCodecSelectionTest)
#include "AvcCodecSelectionTest.moc"
