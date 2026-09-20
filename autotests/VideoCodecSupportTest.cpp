#include <QTest>
#include "VideoCodecSupport.h"

using namespace KRdp;
using namespace KRdp::VideoCodecSupport;

class VideoCodecSupportTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void parsesPreferences()
    {
        QVERIFY(parseCodecPreference(u"auto") == CodecPreference::Auto);
        QVERIFY(parseCodecPreference(u" AVC420 ") == CodecPreference::Avc420);
        QVERIFY(parseCodecPreference(u"Avc444") == CodecPreference::Avc444);
        QVERIFY(!parseCodecPreference(u"avc444v2").has_value()); // the version is negotiated, not configured
        QVERIFY(!parseCodecPreference(u"").has_value());
        QCOMPARE(QString::fromLatin1(preferenceName(CodecPreference::Avc444)), QStringLiteral("avc444"));
    }
    void capsTable_data()
    {
        QTest::addColumn<uint32_t>("version"); QTest::addColumn<uint32_t>("flags"); QTest::addColumn<int>("pref"); QTest::addColumn<int>("codec");
        QTest::newRow("107 auto") << uint32_t(RDPGFX_CAPVERSION_107) << uint32_t(0) << int(CodecPreference::Auto) << int(VideoCodec::Avc444v2);
        QTest::newRow("102 auto") << uint32_t(RDPGFX_CAPVERSION_102) << uint32_t(0) << int(CodecPreference::Auto) << int(VideoCodec::Avc444v2);
        QTest::newRow("101 auto -> v1") << uint32_t(RDPGFX_CAPVERSION_101) << uint32_t(0) << int(CodecPreference::Auto) << int(VideoCodec::Avc444);
        QTest::newRow("10 auto -> v1") << uint32_t(RDPGFX_CAPVERSION_10) << uint32_t(0) << int(CodecPreference::Auto) << int(VideoCodec::Avc444);
        QTest::newRow("81 auto -> 420") << uint32_t(RDPGFX_CAPVERSION_81) << uint32_t(RDPGFX_CAPS_FLAG_AVC420_ENABLED) << int(CodecPreference::Auto) << int(VideoCodec::Avc420);
        QTest::newRow("107 AVC_DISABLED -> 420") << uint32_t(RDPGFX_CAPVERSION_107) << uint32_t(RDPGFX_CAPS_FLAG_AVC_DISABLED) << int(CodecPreference::Auto) << int(VideoCodec::Avc420);
        QTest::newRow("107 forced 420") << uint32_t(RDPGFX_CAPVERSION_107) << uint32_t(0) << int(CodecPreference::Avc420) << int(VideoCodec::Avc420);
        QTest::newRow("107 avc444 pref") << uint32_t(RDPGFX_CAPVERSION_107) << uint32_t(0) << int(CodecPreference::Avc444) << int(VideoCodec::Avc444v2);
        QTest::newRow("81 avc444 pref -> 420 (warned by VideoStream)") << uint32_t(RDPGFX_CAPVERSION_81) << uint32_t(RDPGFX_CAPS_FLAG_AVC420_ENABLED) << int(CodecPreference::Avc444) << int(VideoCodec::Avc420);
    }
    void capsTable()
    {
        QFETCH(uint32_t, version); QFETCH(uint32_t, flags); QFETCH(int, pref); QFETCH(int, codec);
        QCOMPARE(int(codecFor(version, flags, CodecPreference(pref))), codec);
    }
    void namesAndIds()
    {
        QCOMPARE(rdpgfxCodecId(VideoCodec::Avc420), uint16_t(0x000B));
        QCOMPARE(rdpgfxCodecId(VideoCodec::Avc444), uint16_t(0x000E));
        QCOMPARE(rdpgfxCodecId(VideoCodec::Avc444v2), uint16_t(0x000F));
        QCOMPARE(QString::fromLatin1(codecName(VideoCodec::Avc444v2)), QStringLiteral("avc444v2"));
        QVERIFY(isAvc444(VideoCodec::Avc444)); QVERIFY(!isAvc444(VideoCodec::Avc420));
    }
    void expectedBeforeCaps()
    {
        QVERIFY(expectedCodec(CodecPreference::Auto) == VideoCodec::Avc444v2);
        QVERIFY(expectedCodec(CodecPreference::Avc444) == VideoCodec::Avc444v2);
        QVERIFY(expectedCodec(CodecPreference::Avc420) == VideoCodec::Avc420);
    }
    // OPT-045b (design §10 A10.1-A10.3): the pure range/ordering rule ChromaPolicy::isValid()
    // implements, mirrored by the private KPipeWire's H264VAAPIAvc444Encoder and relied on by
    // SessionController::onControlChroma()'s KRDPCTL `chroma` merge/validation (not itself unit-
    // testable here without a live connection - see LayoutControlTest for the wire parsing half).
    void chromaPolicyDefaultsAreValid()
    {
        QVERIFY(ChromaPolicy{}.isValid());
        QCOMPARE(ChromaPolicy{}.motionGapMs, 100);
        QCOMPARE(ChromaPolicy{}.restMs, 150);
        QCOMPARE(ChromaPolicy{}.maxGapMs, 1500);
    }
    void chromaPolicyValidity_data()
    {
        QTest::addColumn<int>("motionGapMs"); QTest::addColumn<int>("restMs"); QTest::addColumn<int>("maxGapMs"); QTest::addColumn<bool>("valid");
        QTest::newRow("lower bound, all equal") << 16 << 16 << 16 << true;
        QTest::newRow("upper bound, all equal") << 5000 << 5000 << 5000 << true;
        QTest::newRow("strictly increasing") << 100 << 150 << 1500 << true;
        QTest::newRow("motionGap below minimum") << 15 << 150 << 1500 << false;
        QTest::newRow("maxGap above maximum") << 100 << 150 << 5001 << false;
        QTest::newRow("restMs below minimum") << 100 << 0 << 1500 << false;
        QTest::newRow("motionGap > restMs") << 200 << 150 << 1500 << false;
        QTest::newRow("restMs > maxGap") << 100 << 2000 << 1500 << false;
        QTest::newRow("motionGap > maxGap (implies restMs > maxGap too)") << 2000 << 2000 << 1500 << false;
    }
    void chromaPolicyValidity()
    {
        QFETCH(int, motionGapMs); QFETCH(int, restMs); QFETCH(int, maxGapMs); QFETCH(bool, valid);
        const ChromaPolicy policy{motionGapMs, restMs, maxGapMs};
        QCOMPARE(policy.isValid(), valid);
    }
};
QTEST_GUILESS_MAIN(VideoCodecSupportTest)
#include "VideoCodecSupportTest.moc"
