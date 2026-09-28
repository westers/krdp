// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX2 F1 / WS-E: the encoder probe. With hardware forced off (KRDP_FORCE_SOFTWARE_ENCODING,
// the switch the SoftwareEncodeSessionTest also uses) H.264 must still be available in
// software, HEVC/AV1 exactly when KPipeWire reports a software backend for them (libx265,
// libsvtav1), and KPipeWire must be pointed at the software H.264 encoder. The real probe must
// never claim a codec KPipeWire cannot produce.

#include "EncoderSupport.h"

#include <PipeWireEncodedStream>
#include <QTest>

extern "C" {
#include <libavcodec/avcodec.h>
}

using namespace KRdp;
using namespace Qt::StringLiterals;
using CodecPolicy::Backends;

namespace
{
/// What the linked KPipeWire itself says about the software backend of \a encoder.
bool kpipewireSoftware(PipeWireEncodedStream::Encoder encoder)
{
    if constexpr (requires { PipeWireEncodedStream::availableEncoderBackends(encoder); }) {
        return bool(PipeWireEncodedStream::availableEncoderBackends(encoder) & PipeWireEncodedStream::EncoderBackend::Software);
    }
    return false;
}
bool kpipewireHardware(PipeWireEncodedStream::Encoder encoder)
{
    if constexpr (requires { PipeWireEncodedStream::availableEncoderBackends(encoder); }) {
        return bool(PipeWireEncodedStream::availableEncoderBackends(encoder) & PipeWireEncodedStream::EncoderBackend::Hardware);
    }
    return true; // an older KPipeWire: only the trial open decides
}
/// Whether the linked KPipeWire changes the software bitrate of \a encoder in place (AUD-SWENC).
bool kpipewireLiveBitrate(PipeWireEncodedStream::Encoder encoder)
{
    if constexpr (requires { PipeWireEncodedStream::softwareBitrateChangeIsLive(encoder); }) {
        return PipeWireEncodedStream::softwareBitrateChangeIsLive(encoder);
    }
    return false;
}
bool sameBackends(const Backends &b, bool hardware, bool software)
{
    return b.hardware == hardware && b.software == software;
}
}

class EncoderSupportTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void overrideParses()
    {
        CodecPolicy::Encoders e;
        QVERIFY(EncoderSupport::applyOverride(e, u"avc=hw+sw, hevc=hw ,av1=sw"_s));
        QCOMPARE(e.avc, (Backends{true, true}));
        QCOMPARE(e.hevc, (Backends{true, false}));
        QCOMPARE(e.av1, (Backends{false, true}));
        QVERIFY(EncoderSupport::applyOverride(e, u"hevc=none"_s));
        QCOMPARE(e.hevc, Backends{});
        QCOMPARE(e.avc, (Backends{true, true})); // untouched
        const auto before = e;
        QVERIFY(!EncoderSupport::applyOverride(e, u"avc=gpu"_s));
        QVERIFY(!EncoderSupport::applyOverride(e, u"vp9=hw"_s));
        QVERIFY(!EncoderSupport::applyOverride(e, u"avc"_s));
        QCOMPARE(e, before);
        // The live-bitrate flag describes the encoder, not the backend choice: an override keeps it.
        e.hevc.liveBitrate = true;
        QVERIFY(EncoderSupport::applyOverride(e, u"hevc=sw"_s));
        QCOMPARE(e.hevc, (Backends{false, true, true}));
    }

    void hardwareForcedOff()
    {
        qputenv("KRDP_FORCE_SOFTWARE_ENCODING", "1");
        qunsetenv("KPIPEWIRE_FORCE_ENCODER");
        qunsetenv("KRDP_ENCODERS");
        const auto probe = EncoderSupport::probeUncached();
        QCOMPARE(probe.encoders.avc.hardware, false);
        // Software HEVC/AV1 are still offered: hardware off is what they are for.
        QVERIFY(sameBackends(probe.encoders.hevc, false, kpipewireSoftware(PipeWireEncodedStream::HEVCMain)));
        QVERIFY(sameBackends(probe.encoders.av1, false, kpipewireSoftware(PipeWireEncodedStream::AV1Main)));
        // libx265 changes its bitrate in place (adaptive quality without reopens); SVT-AV1 cannot.
        QCOMPARE(probe.encoders.hevc.liveBitrate, probe.encoders.hevc.software && kpipewireLiveBitrate(PipeWireEncodedStream::HEVCMain));
        QVERIFY(!probe.encoders.av1.liveBitrate);
        QCOMPARE(probe.encoders.hevc.software, avcodec_find_encoder_by_name("libx265") != nullptr);
        QCOMPARE(probe.encoders.av1.software, avcodec_find_encoder_by_name("libsvtav1") || avcodec_find_encoder_by_name("libaom-av1"));
        QVERIFY(!probe.avc444Hardware);
        QVERIFY(probe.renderNode.isEmpty());
        const bool softwareH264 = avcodec_find_encoder_by_name("libx264") || avcodec_find_encoder_by_name("libopenh264");
        QCOMPARE(probe.encoders.avc.software, softwareH264);
        QVERIFY(EncoderSupport::describe(probe).contains(u"KRDP_FORCE_SOFTWARE_ENCODING"_s));

        EncoderSupport::applyProcessOverrides();
        const QByteArray forced = qgetenv("KPIPEWIRE_FORCE_ENCODER");
        QVERIFY(forced == "libx264" || forced == "libopenh264");
        // An explicit KPipeWire choice is left alone.
        qputenv("KPIPEWIRE_FORCE_ENCODER", "libopenh264");
        EncoderSupport::applyProcessOverrides();
        QCOMPARE(qgetenv("KPIPEWIRE_FORCE_ENCODER"), QByteArray("libopenh264"));

        // The policy then never picks a hardware encoder: software AVC on a normal link, and
        // under `prefer` the best software codec the client decodes.
        CodecPolicy::Input in;
        in.encoders = probe.encoders;
        in.client = {CodecPolicy::Family::Hevc, CodecPolicy::Family::Av1};
        for (const auto mode : {CodecPolicy::SoftwareEncoding::Auto, CodecPolicy::SoftwareEncoding::Never}) {
            in.mode = mode;
            CodecPolicy::State fresh;
            QCOMPARE(CodecPolicy::step(fresh, in, CodecPolicy::Clock::now()).choice, (CodecPolicy::Choice{CodecPolicy::Family::Avc, false}));
        }
        in.mode = CodecPolicy::SoftwareEncoding::Prefer;
        CodecPolicy::State prefer;
        const auto choice = CodecPolicy::step(prefer, in, CodecPolicy::Clock::now()).choice;
        QVERIFY(!choice.hardware);
        const auto best = probe.encoders.av1.software ? CodecPolicy::Family::Av1 : probe.encoders.hevc.software ? CodecPolicy::Family::Hevc : CodecPolicy::Family::Avc;
        QCOMPARE(choice.family, best);
        qunsetenv("KRDP_FORCE_SOFTWARE_ENCODING");
        qunsetenv("KPIPEWIRE_FORCE_ENCODER");
    }

    void videoCapabilitiesListOnlyUsableCodecs()
    {
        EncoderSupport::Probe sol;
        sol.encoders.avc = {false, true};
        auto video = EncoderSupport::videoCapabilities(sol, CodecPolicy::SoftwareEncoding::Auto);
        QCOMPARE(video.softwareEncoding, u"auto"_s);
        QCOMPARE(video.codecs.size(), 1); // no hevc/av1 without an encoder, no avc444 without hardware
        QCOMPARE(video.codecs.first(), (LayoutControl::VideoCodecOffer{u"avc420"_s, false, true}));

        EncoderSupport::Probe future; // hardware HEVC, software-only AV1
        future.encoders.avc = {true, true};
        future.encoders.hevc = {true, false};
        future.encoders.av1 = {false, true};
        future.avc444Hardware = true;
        video = EncoderSupport::videoCapabilities(future, CodecPolicy::SoftwareEncoding::Prefer);
        QCOMPARE(video.codecs.size(), 4);
        QCOMPARE(video.codecs.at(1), (LayoutControl::VideoCodecOffer{u"avc444"_s, true, false}));
        QCOMPARE(video.codecs.at(3), (LayoutControl::VideoCodecOffer{u"av1"_s, false, true}));
        // `never` never uses the software-only AV1; H.264 software stays (last resort).
        video = EncoderSupport::videoCapabilities(future, CodecPolicy::SoftwareEncoding::Never);
        QCOMPARE(video.codecs.size(), 3);
        QCOMPARE(video.codecs.first(), (LayoutControl::VideoCodecOffer{u"avc420"_s, true, true}));
        QCOMPARE(video.codecs.last().name, u"hevc"_s);
    }

    void environmentOverrideWins()
    {
        qputenv("KRDP_FORCE_SOFTWARE_ENCODING", "1"); // keep the host's GPU out of it
        qputenv("KRDP_ENCODERS", "avc=sw,hevc=hw");
        const auto probe = EncoderSupport::probeUncached();
        QVERIFY(sameBackends(probe.encoders.avc, false, true));
        QVERIFY(sameBackends(probe.encoders.hevc, true, false));
        qunsetenv("KRDP_ENCODERS");
        qunsetenv("KRDP_FORCE_SOFTWARE_ENCODING");
    }

    // On this host, whatever it has: never more than KPipeWire can be asked for.
    void realProbeIsConsistent()
    {
        const auto probe = EncoderSupport::probeUncached();
        qInfo().noquote() << "this host:" << EncoderSupport::describe(probe);
        // Software HEVC/AV1 exactly as KPipeWire reports them (not its suggestedEncoders(),
        // which lists software-only HEVC/AV1 too); hardware never without KPipeWire's Hardware bit.
        QCOMPARE(probe.encoders.hevc.software, kpipewireSoftware(PipeWireEncodedStream::HEVCMain));
        QCOMPARE(probe.encoders.av1.software, kpipewireSoftware(PipeWireEncodedStream::AV1Main));
        QVERIFY(!probe.encoders.hevc.hardware || kpipewireHardware(PipeWireEncodedStream::HEVCMain));
        QVERIFY(!probe.encoders.av1.hardware || kpipewireHardware(PipeWireEncodedStream::AV1Main));
        if (probe.encoders.hevc.hardware || probe.encoders.av1.hardware) {
            QVERIFY(probe.encoders.avc.hardware); // same VAAPI device
        }
        QCOMPARE(probe.avc444Hardware && !probe.encoders.avc.hardware, false);
        QCOMPARE(probe.renderNode.isEmpty(), !probe.encoders.avc.hardware);
    }
};

QTEST_GUILESS_MAIN(EncoderSupportTest)
#include "EncoderSupportTest.moc"
