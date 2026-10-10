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

namespace
{
using EncoderSupport::Inputs;
using EncoderSupport::NvidiaEncoder;
using CodecPolicy::Family;

NvidiaEncoder nvenc(const QString &pci, const QString &name, int ordinal, Family family, bool usable, const QString &failure = {})
{
    return {pci, name, ordinal, family, usable, usable ? QSize(8192, 8192) : QSize(), failure};
}

Inputs halToday() // AMD VCN answers every codec; the 4090 is not asked
{
    Inputs in;
    in.vaapi = {true, true, true, u"/dev/dri/renderD128"_s};
    in.liveBitrate = {true, true, false};
    return in;
}

Inputs solTuring() // no VA-API encoder; an RTX 2070: H.264 and HEVC NVENC, no AV1
{
    Inputs in;
    in.vaapi = {};
    const QString pci = u"0000:09:00.0"_s;
    const QString name = u"NVIDIA GeForce RTX 2070"_s;
    in.nvidia = {nvenc(pci, name, 0, Family::Avc, true), nvenc(pci, name, 0, Family::Hevc, true), nvenc(pci, name, 0, Family::Av1, false, u"unsupported"_s)};
    in.liveBitrate = {true, true, false};
    return in;
}
}

class EncoderSupportTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // --- Fake hosts: no probe of the machine at all (safe anywhere, including a host with a broken NVIDIA stack) ---

    void fakeHalKeepsAmdAndNeverUsesTheNvidiaGpu()
    {
        auto in = halToday();
        // Even if the NVIDIA inventory were consulted and full, VA-API covers every codec, so NVENC adds nothing.
        in.nvidia = {nvenc(u"0000:04:00.0"_s, u"NVIDIA GeForce RTX 4090"_s, 0, Family::Avc, true), nvenc(u"0000:04:00.0"_s, u"NVIDIA GeForce RTX 4090"_s, 0, Family::Hevc, true)};
        const auto probe = EncoderSupport::assemble(in);
        QCOMPARE(probe.avcHardwareVia, u"vaapi"_s);
        QCOMPARE(probe.hevcHardwareVia, u"vaapi"_s);
        QCOMPARE(probe.av1HardwareVia, u"vaapi"_s);
        QVERIFY(probe.avc444Hardware);
        QCOMPARE(EncoderSupport::describe(probe), u"avc hw+sw, hevc hw+sw, av1 hw+sw, avc444 hw on /dev/dri/renderD128"_s);
    }

    void fakeSolReportsNvencAvcAndHevcAndSoftwareAv1()
    {
        const auto probe = EncoderSupport::assemble(solTuring());
        QVERIFY(sameBackends(probe.encoders.avc, true, true));
        QVERIFY(sameBackends(probe.encoders.hevc, true, true));
        QVERIFY(sameBackends(probe.encoders.av1, false, true)); // Turing: no AV1 NVENC, and it is never advertised
        QCOMPARE(probe.avcHardwareVia, u"nvenc"_s);
        QCOMPARE(probe.hevcHardwareVia, u"nvenc"_s);
        QVERIFY(probe.av1HardwareVia.isEmpty());
        QVERIFY(!probe.avc444Hardware); // AVC444 stays on VA-API
        QCOMPARE(EncoderSupport::describe(probe), u"avc hw(nvenc)+sw, hevc hw(nvenc)+sw, av1 sw, avc444 none on NVENC NVIDIA GeForce RTX 2070 0000:09:00.0"_s);
        const auto video = EncoderSupport::videoCapabilities(probe, CodecPolicy::SoftwareEncoding::Auto);
        QCOMPARE(video.codecs.first(), (LayoutControl::VideoCodecOffer{u"avc420"_s, true, true}));
        QCOMPARE(video.codecs.size(), 3); // avc420, hevc, av1 (software)
        // The policy now picks hardware AVC where it used to have only libx264.
        CodecPolicy::Input input;
        input.encoders = probe.encoders;
        CodecPolicy::State state;
        QCOMPARE(CodecPolicy::step(state, input, CodecPolicy::Clock::now()).choice, (CodecPolicy::Choice{Family::Avc, true}));
    }

    void fakeAdaGpuSuppliesHardwareAv1WhereVaapiHasNone()
    {
        // A host whose compositor GPU cannot encode (no VA-API) and an Ada or newer NVIDIA GPU (proven on Hal's RTX 4090):
        // all three codecs are NVENC hardware, AV1 included, and the label carries the device.
        Inputs in;
        in.vaapi = {};
        const QString pci = u"0000:04:00.0"_s;
        const QString name = u"NVIDIA GeForce RTX 4090"_s;
        in.nvidia = {nvenc(pci, name, 0, Family::Avc, true), nvenc(pci, name, 0, Family::Hevc, true), nvenc(pci, name, 0, Family::Av1, true)};
        in.liveBitrate = {true, true, false};
        const auto probe = EncoderSupport::assemble(in);
        QVERIFY(sameBackends(probe.encoders.av1, true, true));
        QCOMPARE(probe.av1HardwareVia, u"nvenc"_s);
        QCOMPARE(probe.labels.hardware[2].device, pci);
        QVERIFY(!probe.avc444Hardware); // AVC444 stays on VA-API (N5 is not built)
        QCOMPARE(EncoderSupport::describe(probe), u"avc hw(nvenc)+sw, hevc hw(nvenc)+sw, av1 hw(nvenc)+sw, avc444 none on NVENC NVIDIA GeForce RTX 4090 0000:04:00.0"_s);
        // The same NVIDIA GPU found unusable for AV1 (driver says unsupported) keeps AV1 in software, like Turing.
        in.nvidia.last() = nvenc(pci, name, 0, Family::Av1, false, u"unsupported"_s);
        QVERIFY(sameBackends(EncoderSupport::assemble(in).encoders.av1, false, true));
    }

    void fakeNvidiaDriverBrokenMeansSoftwareOnly()
    {
        // Hal before its reboot: kernel 595.91.07, libraries 595.99.02. The probe is "unavailable", never a failure.
        Inputs in;
        in.nvidiaNote = u"NVIDIA driver/library version mismatch (kernel module 595.91.07, libcuda 595.99.02)"_s;
        const auto probe = EncoderSupport::assemble(in);
        QVERIFY(sameBackends(probe.encoders.avc, false, true));
        QVERIFY(sameBackends(probe.encoders.hevc, false, true));
        QVERIFY(sameBackends(probe.encoders.av1, false, true));
        QVERIFY(probe.nvidia.isEmpty());
        QVERIFY(probe.nvidiaNote.contains(u"mismatch"_s));
        QVERIFY(probe.renderNode.isEmpty());
        QCOMPARE(EncoderSupport::describe(probe), u"avc sw, hevc sw, av1 sw, avc444 none"_s);
        // Alongside a working VA-API the broken NVIDIA stack changes nothing at all.
        auto hal = halToday();
        hal.nvidiaNote = in.nvidiaNote;
        QCOMPARE(EncoderSupport::describe(EncoderSupport::assemble(hal)), EncoderSupport::describe(EncoderSupport::assemble(halToday())));
    }

    void fakeNvidiaOnlyHostWithSessionLimitedGpu()
    {
        Inputs in = solTuring();
        in.nvidia = {nvenc(u"0000:09:00.0"_s, u"NVIDIA GeForce RTX 2070"_s, 0, Family::Avc, false, u"session-limit"_s),
                     nvenc(u"0000:09:00.0"_s, u"NVIDIA GeForce RTX 2070"_s, 0, Family::Hevc, true)};
        const auto probe = EncoderSupport::assemble(in);
        QVERIFY(sameBackends(probe.encoders.avc, false, true)); // a codec that did not open is not advertised
        QVERIFY(sameBackends(probe.encoders.hevc, true, true));
    }

    void fakeOlderKpipewireWithoutNvencHardwareBit()
    {
        auto in = solTuring();
        in.kpipewireHardware = {false, false, false}; // KPipeWire cannot be asked for hardware: nothing is claimed
        const auto probe = EncoderSupport::assemble(in);
        QVERIFY(sameBackends(probe.encoders.avc, false, true));
        QVERIFY(sameBackends(probe.encoders.hevc, false, true));
    }

    void fakeForcedSoftwareIgnoresNvidia()
    {
        auto in = solTuring();
        in.forcedSoftware = true;
        const auto probe = EncoderSupport::assemble(in);
        QVERIFY(sameBackends(probe.encoders.avc, false, true));
        QVERIFY(sameBackends(probe.encoders.hevc, false, true));
        QVERIFY(probe.avcHardwareVia.isEmpty());
    }

    void fakeOverrideKeepsItsMeaningOnNvenc()
    {
        auto in = solTuring();
        in.overrideSpec = u"avc=sw,hevc=none,av1=hw"_s;
        const auto probe = EncoderSupport::assemble(in);
        QVERIFY(sameBackends(probe.encoders.avc, false, true));
        QVERIFY(sameBackends(probe.encoders.hevc, false, false));
        QVERIFY(sameBackends(probe.encoders.av1, true, false));
        QVERIFY(EncoderSupport::describe(probe).startsWith(u"avc sw, hevc none, av1 hw, avc444 none"_s));
        in.overrideSpec = u"avc=bogus"_s; // invalid: ignored, the probe stands
        QVERIFY(sameBackends(EncoderSupport::assemble(in).encoders.avc, true, true));
    }

    // --- Probes of this machine (open VA-API / NVIDIA devices): test hosts only ---

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
        qputenv("FARSIDE_FORCE_SOFTWARE_ENCODING", "1");
        qunsetenv("KPIPEWIRE_FORCE_ENCODER");
        qunsetenv("FARSIDE_ENCODERS");
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
        QVERIFY(EncoderSupport::describe(probe).contains(u"FARSIDE_FORCE_SOFTWARE_ENCODING"_s));

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
        qunsetenv("FARSIDE_FORCE_SOFTWARE_ENCODING");
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
        qputenv("FARSIDE_FORCE_SOFTWARE_ENCODING", "1"); // keep the host's GPU out of it
        qputenv("FARSIDE_ENCODERS", "avc=sw,hevc=hw");
        const auto probe = EncoderSupport::probeUncached();
        QVERIFY(sameBackends(probe.encoders.avc, false, true));
        QVERIFY(sameBackends(probe.encoders.hevc, true, false));
        qunsetenv("FARSIDE_ENCODERS");
        qunsetenv("FARSIDE_FORCE_SOFTWARE_ENCODING");
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
