// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX2 F1: the codec/backend policy behind `SoftwareEncoding` (src/CodecPolicy.h).

#include "CodecPolicy.h"

#include <QTest>

using namespace KRdp::CodecPolicy;
using namespace std::chrono_literals;

namespace
{
const Clock::time_point T0 = Clock::time_point(1h); // any non-epoch start

Encoders hal() // VAAPI H.264/HEVC/AV1 plus libx264
{
    Encoders e;
    e.avc = {true, true};
    e.hevc = {true, false};
    e.av1 = {true, false};
    return e;
}
Encoders sol() // no hardware at all, libx264 only
{
    Encoders e;
    e.avc = {false, true};
    return e;
}
Encoders softwareEverything() // KPipeWire with libx265/SVT-AV1 (WS-E), no GPU
{
    Encoders e;
    e.avc = {false, true};
    e.hevc = {false, true};
    e.av1 = {false, true};
    return e;
}
Input input(SoftwareEncoding mode, Encoders encoders, QList<Family> client = {Family::Hevc, Family::Av1})
{
    Input in;
    in.mode = mode;
    in.encoders = encoders;
    in.client = client;
    return in;
}
// Drives \a in for \a duration in 1.5 s ticks (the adaptive-quality interval).
Decision run(State &state, const Input &in, Clock::time_point &now, std::chrono::milliseconds duration)
{
    Decision last{};
    const auto end = now + duration;
    while (now < end) {
        now += 1500ms;
        const auto d = step(state, in, now);
        if (d.changed || last.reason.isEmpty()) last = d;
    }
    return last;
}
}

class CodecPolicyTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void parsesModes()
    {
        QCOMPARE(parseSoftwareEncoding(u"auto"), SoftwareEncoding::Auto);
        QCOMPARE(parseSoftwareEncoding(u" Never "), SoftwareEncoding::Never);
        QCOMPARE(parseSoftwareEncoding(u"PREFER"), SoftwareEncoding::Prefer);
        QVERIFY(!parseSoftwareEncoding(u"always"));
        QCOMPARE(QByteArray(softwareEncodingName(SoftwareEncoding::Prefer)), QByteArray("prefer"));
    }

    void thresholdsScaleWithPixels()
    {
        QCOMPARE(slowBelowKbps(ReferencePixels), 15000.0);
        QCOMPARE(fastAboveKbps(ReferencePixels), 25000.0);
        QCOMPARE(slowBelowKbps(2 * ReferencePixels), 30000.0); // two 1080p monitors
        QCOMPARE(slowBelowKbps(1), 1500.0); // floor: a tenth of 1080p
    }

    // Normal link, auto: the best codec that has a hardware encoder.
    void autoPicksBestHardwareCodec_data()
    {
        QTest::addColumn<QList<Family>>("client");
        QTest::addColumn<int>("family");
        QTest::newRow("hevc+av1") << QList<Family>{Family::Hevc, Family::Av1} << int(Family::Av1);
        QTest::newRow("hevc only") << QList<Family>{Family::Hevc} << int(Family::Hevc);
        QTest::newRow("avc only") << QList<Family>{} << int(Family::Avc);
    }
    void autoPicksBestHardwareCodec()
    {
        QFETCH(QList<Family>, client);
        QFETCH(int, family);
        State state;
        const auto d = step(state, input(SoftwareEncoding::Auto, hal(), client), T0);
        QVERIFY(d.changed);
        QCOMPARE(int(d.choice.family), family);
        QVERIFY(d.choice.hardware);
    }

    // Sol: no hardware encoder, the client asks for HEVC: AVC in software, never a mislabelled stream.
    void noHardwareFallsBackToSoftwareAvc()
    {
        for (const auto mode : {SoftwareEncoding::Auto, SoftwareEncoding::Never, SoftwareEncoding::Prefer}) {
            State state;
            const auto d = step(state, input(mode, sol()), T0);
            QCOMPARE(d.choice, (Choice{Family::Avc, false}));
        }
    }

    void autoOnNormalLinkIgnoresSoftwareOnlyCodecs()
    {
        State state;
        QCOMPARE(step(state, input(SoftwareEncoding::Auto, softwareEverything()), T0).choice, (Choice{Family::Avc, false}));
        State hw;
        auto e = softwareEverything();
        e.hevc.hardware = true;
        QCOMPARE(step(hw, input(SoftwareEncoding::Auto, e), T0).choice, (Choice{Family::Hevc, true}));
    }

    void neverUsesHardwareOnly()
    {
        auto e = softwareEverything();
        e.hevc.hardware = true;
        State state;
        auto in = input(SoftwareEncoding::Never, e);
        QCOMPARE(step(state, in, T0).choice, (Choice{Family::Hevc, true}));
        // Even on a slow link it never goes to software AV1.
        in.bandwidthKbps = 2000;
        in.congested = true;
        auto now = T0;
        run(state, in, now, 30s);
        QVERIFY(state.slowLink);
        QCOMPARE(*state.current, (Choice{Family::Hevc, true}));
    }

    void preferPicksBestCompressionEvenInSoftware()
    {
        State state;
        QCOMPARE(step(state, input(SoftwareEncoding::Prefer, softwareEverything()), T0).choice, (Choice{Family::Av1, false}));
        // Hardware for the same family wins over software.
        auto e = softwareEverything();
        e.av1.hardware = true;
        State hw;
        QCOMPARE(step(hw, input(SoftwareEncoding::Prefer, e), T0).choice, (Choice{Family::Av1, true}));
        // A codec the client does not decode is never picked.
        State limited;
        QCOMPARE(step(limited, input(SoftwareEncoding::Prefer, softwareEverything(), {Family::Hevc}), T0).choice, (Choice{Family::Hevc, false}));
    }

    // Slow = goodput under 15 Mbit/s (1080p) *and* congestion, held 5 s; back above 25 Mbit/s for 5 s.
    void slowLinkNeedsHoldAndHysteresis()
    {
        auto e = hal();
        e.av1 = {false, true}; // AV1 only in software; HEVC in hardware
        auto in = input(SoftwareEncoding::Auto, e);
        State state;
        auto now = T0;
        QCOMPARE(step(state, in, now).choice, (Choice{Family::Hevc, true}));

        // Low goodput without congestion is a still desktop, not a slow link.
        in.bandwidthKbps = 3000;
        in.congested = false;
        run(state, in, now, 20s);
        QVERIFY(!state.slowLink);

        // Congested and slow, but only for 3 s: nothing.
        in.congested = true;
        run(state, in, now, 3s);
        QVERIFY(!state.slowLink);
        in.congested = false;
        run(state, in, now, 1500ms);
        in.congested = true;
        run(state, in, now, 4500ms);
        QVERIFY(!state.slowLink); // the hold restarted
        const auto d = run(state, in, now, 3s);
        QVERIFY(state.slowLink);
        QVERIFY(d.changed);
        QCOMPARE(d.choice, (Choice{Family::Av1, false}));
        QVERIFY2(d.reason.contains(u"slow link"), qPrintable(d.reason));

        // Between the thresholds (20 Mbit/s): stays slow.
        in.congested = false;
        in.bandwidthKbps = 20000;
        run(state, in, now, 30s);
        QVERIFY(state.slowLink);
        // Above 25 Mbit/s for 5 s: back to the hardware codec.
        in.bandwidthKbps = 30000;
        const auto back = run(state, in, now, 9s);
        QVERIFY(!state.slowLink);
        QCOMPARE(*state.current, (Choice{Family::Hevc, true}));
        QVERIFY2(back.reason.contains(u"recovered"), qPrintable(back.reason));
    }

    void pinnedClientIgnoresTheLink()
    {
        auto in = input(SoftwareEncoding::Auto, softwareEverything());
        in.adaptive = false;
        in.bandwidthKbps = 1000;
        in.congested = true;
        State state;
        auto now = T0;
        step(state, in, now);
        run(state, in, now, 30s);
        QVERIFY(!state.slowLink);
        QCOMPARE(*state.current, (Choice{Family::Avc, false}));
    }

    // p95 over 70 % of the budget: the preset first (AV1 M10 -> M11; M12 = M11 is skipped), then
    // AV1(sw) -> HEVC(sw, at its fastest preset) -> the hardware codec (here AVC); each step at
    // least MinReconfigureInterval after the previous one.
    void cpuGuardStepsDown()
    {
        auto e = softwareEverything();
        e.avc.hardware = true;
        auto in = input(SoftwareEncoding::Prefer, e);
        State state;
        auto now = T0;
        auto d = step(state, in, now);
        QCOMPARE(d.choice, (Choice{Family::Av1, false}));
        QCOMPARE(d.settings.preset, Preset::Efficient);

        in.encodeLoadP95 = 0.65; // fine
        run(state, in, now, 30s);
        QCOMPARE(*state.current, (Choice{Family::Av1, false}));
        QCOMPARE(state.preset, Preset::Efficient);

        // Step 1: a faster preset, same codec.
        in.encodeLoadP95 = 0.9;
        now += 1500ms;
        d = step(state, in, now);
        QVERIFY(!d.changed);
        QVERIFY(d.settingsChanged);
        QCOMPARE(d.settings.preset, Preset::Balanced);
        QVERIFY2(d.settingsReason.contains(u"CPU guard") && d.settingsReason.contains(u"preset balanced"), qPrintable(d.settingsReason));

        // Still too slow: nothing within the interval...
        now += 1500ms;
        d = step(state, in, now);
        QVERIFY(!d.changed);
        QVERIFY(!d.settingsChanged);
        // ...then, AV1 Fastest being the same encoder as Balanced, the codec: HEVC at ultrafast.
        now += 9s;
        d = step(state, in, now);
        QVERIFY(d.changed);
        QCOMPARE(d.choice, (Choice{Family::Hevc, false}));
        QVERIFY2(d.reason.contains(u"CPU guard"), qPrintable(d.reason));
        QCOMPARE(d.settings.preset, Preset::Fastest);

        // HEVC has no faster preset left: the hardware codec, again after the interval.
        now += 1500ms;
        d = step(state, in, now);
        QVERIFY(!d.changed);
        QCOMPARE(d.choice, (Choice{Family::Hevc, false}));
        now += 10s;
        d = step(state, in, now);
        QVERIFY(d.changed);
        QCOMPARE(d.choice, (Choice{Family::Avc, true}));
        QCOMPARE(d.settings, (EncoderSettings{true, Preset::Efficient, 0, 0}));

        // Hardware has no guard; the blocked software backends stay blocked for CpuBlockFor.
        in.encodeLoadP95.reset();
        run(state, in, now, 60s);
        QCOMPARE(*state.current, (Choice{Family::Avc, true}));
        run(state, in, now, 90s);
        QCOMPARE(*state.current, (Choice{Family::Av1, false})); // tried again after the block
        QCOMPARE(state.preset, Preset::Efficient); // not a guard switch
    }

    // HEVC's ladder has three real presets: veryfast -> superfast -> ultrafast, then the codec.
    void presetStepsComeBeforeCodecSwitch()
    {
        auto in = input(SoftwareEncoding::Prefer, softwareEverything(), {Family::Hevc});
        State state;
        auto now = T0;
        QCOMPARE(step(state, in, now).choice, (Choice{Family::Hevc, false}));
        in.encodeLoadP95 = 0.8;
        QList<Preset> presets;
        Decision d;
        for (int i = 0; i < 40 && !d.changed; ++i) {
            now += 1500ms;
            d = step(state, in, now);
            if (d.settingsChanged && !d.changed) {
                presets.append(d.settings.preset);
                QVERIFY(d.settingsReason.contains(u"preset"));
                QVERIFY(presets.size() < 2 || now - state.lastSwitch >= 2 * MinReconfigureInterval);
            }
        }
        QCOMPARE(presets, (QList<Preset>{Preset::Balanced, Preset::Fastest}));
        QVERIFY(d.changed);
        QCOMPARE(d.choice, (Choice{Family::Avc, false}));
        QVERIFY2(d.reason.contains(u"CPU guard: software hevc"), qPrintable(d.reason));
    }

    // SVT-AV1 2.3 runs preset 12 as 11: a guard step to it would reopen for nothing.
    void noOpPresetStepIsSkipped()
    {
        QCOMPARE(nextPreset(Family::Av1, Preset::Efficient), std::optional<Preset>(Preset::Balanced));
        QCOMPARE(nextPreset(Family::Av1, Preset::Balanced), std::optional<Preset>());
        QCOMPARE(nextPreset(Family::Hevc, Preset::Efficient), std::optional<Preset>(Preset::Balanced));
        QCOMPARE(nextPreset(Family::Hevc, Preset::Balanced), std::optional<Preset>(Preset::Fastest));
        QCOMPARE(nextPreset(Family::Hevc, Preset::Fastest), std::optional<Preset>());
        QCOMPARE(nextPreset(Family::Avc, Preset::Efficient), std::optional<Preset>()); // libx264 stays ultrafast
        QCOMPARE(presetLevel(Family::Av1, Preset::Fastest), presetLevel(Family::Av1, Preset::Balanced));

        // In the policy: AV1 at Balanced over budget goes straight to the next codec.
        auto in = input(SoftwareEncoding::Prefer, softwareEverything());
        State state;
        auto now = T0;
        step(state, in, now);
        state.preset = Preset::Balanced;
        in.encodeLoadP95 = 0.9;
        now += 11s;
        const auto d = step(state, in, now);
        QVERIFY(d.changed);
        QCOMPARE(d.choice, (Choice{Family::Hevc, false}));
    }

    // No software HEVC/AV1 setting keeps up with 1080p60 full motion (PERF.md): 30 fps for them.
    void softwarePrivateCodecsAreCappedAt30()
    {
        State sw;
        auto d = step(sw, input(SoftwareEncoding::Prefer, softwareEverything()), T0);
        QCOMPARE(d.choice, (Choice{Family::Av1, false}));
        QCOMPARE(d.settings.maxFrameRate, 30);
        QVERIFY(d.settingsChanged);
        State hevc;
        QCOMPARE(step(hevc, input(SoftwareEncoding::Prefer, softwareEverything(), {Family::Hevc}), T0).settings.maxFrameRate, 30);
        // Hardware codecs and software H.264 keep the full rate.
        State hw;
        d = step(hw, input(SoftwareEncoding::Auto, hal()), T0);
        QCOMPARE(d.choice, (Choice{Family::Av1, true}));
        QCOMPARE(d.settings.maxFrameRate, 0);
        State avc;
        d = step(avc, input(SoftwareEncoding::Auto, sol()), T0);
        QCOMPARE(d.settings, (EncoderSettings{false, Preset::Efficient, 0, 0}));
        // Leaving software AV1 for a hardware codec lifts the cap.
        auto e = hal();
        e.av1 = {false, true};
        auto in = input(SoftwareEncoding::Prefer, e);
        State state;
        auto now = T0;
        QCOMPARE(step(state, in, now).settings.maxFrameRate, 30);
        in.mode = SoftwareEncoding::Auto;
        d = run(state, in, now, 12s);
        QVERIFY(d.changed);
        QCOMPARE(d.choice, (Choice{Family::Hevc, true}));
        QCOMPARE(d.settings.maxFrameRate, 0);
    }

    // The guard's last step: no faster preset and no other codec (software H.264 only).
    void cpuGuardLowersTheFrameRateLast()
    {
        auto in = input(SoftwareEncoding::Auto, sol());
        State state;
        auto now = T0;
        step(state, in, now);
        in.encodeLoadP95 = 0.9;
        QList<int> rates;
        for (int i = 0; i < 40; ++i) {
            now += 1500ms;
            const auto d = step(state, in, now);
            QVERIFY(!d.changed);
            if (d.settingsChanged) {
                rates.append(d.settings.maxFrameRate);
                QVERIFY2(d.settingsReason.contains(u"frame rate"), qPrintable(d.settingsReason));
            }
        }
        QCOMPARE(rates, (QList<int>{30, 15})); // never below MinFrameRate
        // The load drops well under the limit: back up, one step per interval.
        in.encodeLoadP95 = 0.2;
        rates.clear();
        for (int i = 0; i < 20; ++i) {
            now += 1500ms;
            const auto d = step(state, in, now);
            if (d.settingsChanged) rates.append(d.settings.maxFrameRate);
        }
        QCOMPARE(rates, (QList<int>{30, 0}));
    }

    // WS-E: a host without a GPU but with libx265/SVT-AV1 uses them on a slow link (auto), AV1
    // first, HEVC when the client lacks AV1 or the host lacks SVT-AV1.
    void softwareHevcAv1OnSlowLink_data()
    {
        QTest::addColumn<QList<Family>>("client");
        QTest::addColumn<bool>("av1Encoder");
        QTest::addColumn<int>("family");
        QTest::newRow("hevc+av1") << QList<Family>{Family::Hevc, Family::Av1} << true << int(Family::Av1);
        QTest::newRow("hevc only client") << QList<Family>{Family::Hevc} << true << int(Family::Hevc);
        QTest::newRow("no svt-av1") << QList<Family>{Family::Hevc, Family::Av1} << false << int(Family::Hevc);
        QTest::newRow("avc only client") << QList<Family>{} << true << int(Family::Avc);
    }
    void softwareHevcAv1OnSlowLink()
    {
        QFETCH(QList<Family>, client);
        QFETCH(bool, av1Encoder);
        QFETCH(int, family);
        auto e = softwareEverything();
        e.av1.software = av1Encoder;
        auto in = input(SoftwareEncoding::Auto, e, client);
        State state;
        auto now = T0;
        QCOMPARE(step(state, in, now).choice, (Choice{Family::Avc, false})); // normal link
        in.bandwidthKbps = 3000;
        in.congested = true;
        run(state, in, now, 15s);
        QVERIFY(state.slowLink);
        QCOMPARE(int(state.current->family), family);
        QVERIFY(!state.current->hardware);
        const bool privateCodec = Family(family) != Family::Avc;
        QCOMPARE(state.applied.maxFrameRate, privateCodec ? 30 : 0);
        QCOMPARE(state.applied.targetKbps, privateCodec ? 2550u : 0u);
    }

    // Slow link, software codec chosen for bandwidth: the target bitrate follows the link.
    void slowLinkDrivesTheTargetBitrate()
    {
        auto e = hal();
        e.av1 = {false, true};
        auto in = input(SoftwareEncoding::Auto, e);
        State state;
        auto now = T0;
        QCOMPARE(step(state, in, now).settings.targetKbps, 0u); // hardware HEVC: quality mode
        in.bandwidthKbps = 4000;
        in.congested = true;
        const auto d = run(state, in, now, 12s);
        QVERIFY(d.changed);
        QCOMPARE(d.choice, (Choice{Family::Av1, false}));
        QCOMPARE(state.applied.targetKbps, 3400u); // 85 % of the goodput, set with the switch
        QCOMPARE(state.applied.maxFrameRate, 30);

        // Congested at lower goodput: the target drops, at most once per interval.
        in.bandwidthKbps = 2000;
        now += 1500ms;
        auto s = step(state, in, now);
        QVERIFY(!s.settingsChanged); // within MinReconfigureInterval of the switch
        now += 10s;
        s = step(state, in, now);
        QVERIFY(s.settingsChanged);
        QCOMPARE(s.settings.targetKbps, 1700u);
        QVERIFY2(s.settingsReason.contains(u"target bitrate 1700"), qPrintable(s.settingsReason));

        // Not congested: grows 25 % per interval, never above the slow-link threshold.
        in.congested = false;
        in.bandwidthKbps = 1700; // demand-limited: the encoder only sent what it was allowed
        QList<quint32> targets;
        for (int i = 0; i < 100; ++i) {
            now += 1500ms;
            s = step(state, in, now);
            if (s.settingsChanged) targets.append(s.settings.targetKbps);
        }
        QVERIFY(targets.size() >= 3);
        QCOMPARE(targets.first(), 2125u);
        QCOMPARE(targets.last(), quint32(slowBelowKbps(ReferencePixels)));
        QVERIFY(state.slowLink); // 1.7 Mbit/s never proves a fast link

        // A small change (< 15 %) is not worth a reopen.
        in.congested = true;
        in.bandwidthKbps = quint32(slowBelowKbps(ReferencePixels) / TargetShareOfGoodput * 0.9);
        now += 11s;
        s = step(state, in, now);
        QVERIFY(!s.settingsChanged);

        // Link recovered: back to hardware HEVC in quality mode.
        in.congested = false;
        in.bandwidthKbps = 30000;
        run(state, in, now, 20s);
        QCOMPARE(*state.current, (Choice{Family::Hevc, true}));
        QCOMPARE(state.applied.targetKbps, 0u);
    }

    // Stall budget: a reopen (HEVC ~45 ms, AV1 ~150 ms) at most once per interval.
    void reopenStallFitsTheInterval()
    {
        QCOMPARE(reopenStall(Family::Hevc), 45ms);
        QCOMPARE(reopenStall(Family::Av1), 150ms);
        QCOMPARE(reopenStall(Family::Avc), 0ms);
        QVERIFY(reopenStall(Family::Av1) * 50 <= MinReconfigureInterval);
        QCOMPARE(std::chrono::duration_cast<std::chrono::seconds>(MinReconfigureInterval), std::chrono::duration_cast<std::chrono::seconds>(MinSwitchInterval));
    }

    void antiFlapInterval()
    {
        auto e = hal();
        e.av1 = {false, true};
        auto in = input(SoftwareEncoding::Auto, e);
        State state;
        auto now = T0;
        step(state, in, now);
        // Slow link at once (hold satisfied), 4.5 s after the initial choice.
        in.bandwidthKbps = 1000;
        in.congested = true;
        now += 1500ms;
        step(state, in, now);
        now += 1500ms;
        step(state, in, now);
        now += 5s;
        auto d = step(state, in, now);
        QVERIFY(state.slowLink);
        QVERIFY(!d.changed); // 8 s after the initial choice
        QVERIFY(d.reason.contains(u"waiting"));
        now += 2s;
        d = step(state, in, now);
        QVERIFY(d.changed);
        QCOMPARE(d.choice, (Choice{Family::Av1, false}));
        QVERIFY(now - state.lastSwitch < 1ms);
    }

    void loadWindowP95()
    {
        LoadWindow w;
        w.add(0.1);
        w.add(0.2);
        w.add(0.3);
        QVERIFY(!w.p95()); // too few samples
        w.add(0.4);
        QCOMPARE(*w.p95(), 0.4);
        for (int i = 0; i < 9; ++i) w.add(0.1);
        w.add(0.95); // 10 samples: nine 0.1, one 0.95
        QCOMPARE(*w.p95(), 0.95);
        w.add(0.1); // the oldest 0.1 goes; 0.95 stays in the window
        QCOMPARE(*w.p95(), 0.95);
        w.clear();
        QVERIFY(!w.p95());
    }
};

QTEST_GUILESS_MAIN(CodecPolicyTest)
#include "CodecPolicyTest.moc"
