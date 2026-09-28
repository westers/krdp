// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX2 F1: the codec/backend policy behind `SoftwareEncoding` (src/CodecPolicy.h).

#include "AdaptiveQuality.h"
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
    e.avc = {false, true, true};
    e.hevc = {false, true, true}; // libx265 changes its bitrate in place (AUD-SWENC)
    e.av1 = {false, true, false}; // SVT-AV1 reopens for every change
    return e;
}

/**
 * Adaptive quality (the real KRdp::AdaptiveQuality::step()) and the codec policy, as
 * VideoStream::updateAdaptiveQuality() runs them every 1.5 s: quality first, then the policy
 * with that quality. The link alternates between congested (RTT 60 ms over a 10 ms minimum)
 * and clear in \a pattern-long phases. Returns the number of quality changes; \a restartTimes
 * receives when the policy restarted the encoder, \a bitrateChanges each target change.
 */
struct AdaptiveRun {
    int qualityChanges = 0;
    QList<Clock::time_point> restartTimes;
    QList<std::pair<quint32, quint32>> bitrateChanges; ///< from, to
};
AdaptiveRun runAdaptiveQuality(State &state, Input in, Clock::time_point &now, std::chrono::seconds duration, QList<std::chrono::seconds> pattern)
{
    using namespace std::chrono;
    AdaptiveRun run;
    int quality = 80;
    Clock::time_point lastStepDown{};
    const auto start = now;
    const auto end = now + duration;
    while (now < end) {
        now += 1500ms;
        // Which phase: even = congested, odd = clear, cycling through the pattern.
        auto t = duration_cast<seconds>(now - start);
        seconds cycle{0};
        for (const auto p : pattern) cycle += p;
        t = seconds(t.count() % cycle.count());
        int phase = 0;
        for (; phase < pattern.size() && t >= pattern[phase]; ++phase) t -= pattern[phase];
        const bool congested = phase % 2 == 0;
        const auto result = KRdp::AdaptiveQuality::step({
            .current = quality,
            .cap = 80,
            .averageRtt = congested ? microseconds(60000) : microseconds(10000),
            .minimumRtt = microseconds(10000),
            .backlogged = false,
            .climbAllowed = now - lastStepDown >= KRdp::AdaptiveQuality::ClimbHoldAfterStepDown,
        });
        if (result.next < quality) lastStepDown = now;
        if (result.next != quality) ++run.qualityChanges;
        quality = result.next;
        in.quality = quint8(quality);
        in.congested = congested;
        const quint32 before = state.applied.targetKbps;
        const int restarts = state.encoderRestarts;
        const auto d = step(state, in, now);
        if (state.encoderRestarts != restarts) run.restartTimes.append(now);
        if (d.settingsChanged && d.settings.targetKbps != before) run.bitrateChanges.append({before, d.settings.targetKbps});
    }
    return run;
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
/**
 * AUD-FIX5 D2: the link as the policy saw it in the Sol S1 pass (2026-09-28, 1920x1080, SoftwareEncoding=auto,
 * evidence/2026-09-28-final-round/sol/s11-S1-journal.txt), reconstructed from the adaptive-quality
 * lines (logged only when the quality changes; a tick without a line was clear, except at the
 * floor) and the bandwidth measurements. The link got slow on its own at ~T+23 s (the send rate fell
 * from 15.5 to ~3 Mbit/s with fq_codel still in place), the 6 Mbit/s tbf came at T+44.0 s, the first
 * congestion under it at T+57 s, the tbf went at T+194 s; T+218 s on is the free link with
 * full-screen mandelbrot (goodput 10-52 Mbit/s, a congestion blip every 12-30 s).
 */
struct SolTick {
    int ms;
    int congested;
    int kbps;
    int quality;
};
// Generated from s11-S1-journal.txt: one entry per 1.5 s adaptive-quality tick from 23:47:52.100 CDT.
// {ms since the first tick, congested, goodput kbit/s, adaptive quality}
constexpr SolTick SolS1[] = {
    {0, 1, 1356, 65}, {1500, 1, 1384, 55}, {3000, 0, 1468, 55}, {4500, 0, 1468, 55}, {6000, 0, 2628, 55},
    {7500, 0, 2815, 60}, {9000, 0, 4263, 65}, {10500, 0, 4263, 70}, {12000, 0, 6573, 75}, {13500, 0, 10381, 75},
    {15000, 0, 12622, 75}, {16500, 0, 12622, 75}, {18000, 0, 12900, 75}, {19500, 0, 13611, 75}, {21000, 0, 14285, 75},
    {22500, 0, 14285, 75}, {24000, 1, 14338, 65}, {25500, 1, 10757, 55}, {27000, 0, 7049, 55}, {28500, 0, 7049, 55},
    {30000, 0, 4927, 55}, {31500, 1, 3861, 45}, {33000, 1, 3019, 35}, {34500, 1, 3019, 25}, {36000, 1, 2692, 15},
    {37500, 0, 2209, 15}, {39000, 0, 1963, 15}, {40500, 0, 1963, 15}, {42000, 0, 1964, 20}, {43500, 0, 2100, 25},
    {45000, 0, 2117, 30}, {46500, 0, 2117, 35}, {48000, 0, 2162, 40}, {49500, 0, 2127, 45}, {51000, 0, 2102, 50},
    {52500, 0, 2102, 55}, {54000, 0, 2445, 60}, {55500, 0, 3887, 65}, {57000, 1, 2660, 55}, {58500, 1, 2660, 45},
    {60000, 0, 2482, 45}, {61500, 0, 2405, 45}, {63000, 0, 2305, 45}, {64500, 0, 2305, 50}, {66000, 0, 2184, 55},
    {67500, 0, 2480, 60}, {69000, 0, 3941, 65}, {70500, 1, 3941, 55}, {72000, 1, 3353, 45}, {73500, 0, 2711, 45},
    {75000, 0, 2346, 45}, {76500, 0, 2346, 45}, {78000, 0, 2151, 50}, {79500, 0, 2237, 55}, {81000, 0, 2716, 60},
    {82500, 0, 2716, 65}, {84000, 1, 4003, 55}, {85500, 0, 3389, 55}, {87000, 0, 3252, 55}, {88500, 0, 3252, 55},
    {90000, 0, 3034, 60}, {91500, 0, 4247, 65}, {93000, 1, 4672, 55}, {94500, 1, 4672, 45}, {96000, 0, 3486, 45},
    {97500, 0, 2931, 45}, {99000, 0, 2503, 45}, {100500, 0, 2503, 50}, {102000, 0, 2303, 55}, {103500, 0, 2514, 60},
    {105000, 0, 3848, 65}, {106500, 1, 3848, 55}, {108000, 1, 4263, 45}, {109500, 0, 4217, 45}, {111000, 0, 4401, 45},
    {112500, 0, 4401, 45}, {114000, 0, 4333, 50}, {115500, 0, 3840, 55}, {117000, 0, 4047, 60}, {118500, 1, 4047, 50},
    {120000, 0, 4014, 50}, {121500, 0, 3415, 50}, {123000, 0, 3260, 50}, {124500, 0, 3260, 55}, {126000, 0, 3846, 60},
    {127500, 1, 3748, 50}, {129000, 0, 3273, 50}, {130500, 0, 3273, 50}, {132000, 0, 3214, 50}, {133500, 0, 3009, 55},
    {135000, 0, 3009, 60}, {136500, 1, 4137, 50}, {138000, 1, 3898, 40}, {139500, 0, 2979, 40}, {141000, 0, 2387, 40},
    {142500, 0, 2387, 40}, {144000, 0, 2155, 45}, {145500, 0, 2298, 50}, {147000, 0, 2298, 55}, {148500, 1, 3384, 45},
    {150000, 0, 3433, 45}, {151500, 0, 2962, 45}, {153000, 0, 2870, 45}, {154500, 0, 2870, 50}, {156000, 1, 3675, 40},
    {157500, 1, 3496, 30}, {159000, 0, 3132, 30}, {160500, 0, 3132, 30}, {162000, 0, 2761, 30}, {163500, 0, 2630, 35},
    {165000, 1, 2630, 25}, {166500, 0, 2632, 25}, {168000, 0, 2682, 25}, {169500, 0, 2797, 25}, {171000, 0, 2797, 30},
    {172500, 0, 2921, 35}, {174000, 0, 3121, 40}, {175500, 0, 3203, 45}, {177000, 0, 3203, 50}, {178500, 1, 3659, 40},
    {180000, 1, 4904, 30}, {181500, 0, 4228, 30}, {183000, 0, 4228, 30}, {184500, 0, 3817, 30}, {186000, 0, 3411, 35},
    {187500, 0, 3040, 40}, {189000, 0, 3040, 45}, {190500, 0, 3072, 50}, {192000, 1, 4247, 40}, {193500, 0, 3813, 40},
    {195000, 1, 3813, 30}, {196500, 1, 3569, 20}, {198000, 1, 3424, 10}, {199500, 1, 7903, 10}, {201000, 1, 7903, 10},
    {202500, 1, 5463, 10}, {204000, 0, 4203, 15}, {205500, 0, 4203, 20}, {207000, 0, 3685, 25}, {208500, 0, 3284, 30},
    {210000, 0, 3065, 35}, {211500, 0, 2809, 40}, {213000, 0, 2809, 45}, {214500, 0, 2722, 50}, {216000, 0, 3071, 55},
    {217500, 0, 4346, 60}, {219000, 0, 4346, 65}, {220500, 0, 16666, 70}, {222000, 0, 30047, 75}, {223500, 1, 39197, 65},
    {225000, 1, 39197, 55}, {226500, 0, 24982, 55}, {228000, 0, 15534, 55}, {229500, 0, 10769, 55}, {231000, 0, 10769, 60},
    {232500, 0, 12950, 65}, {234000, 0, 15653, 70}, {235500, 0, 23159, 75}, {237000, 0, 23159, 75}, {238500, 0, 28935, 75},
    {240000, 0, 30831, 75}, {241500, 0, 30932, 75}, {243000, 0, 30932, 75}, {244500, 0, 36985, 75}, {246000, 0, 32713, 75},
    {247500, 0, 31348, 75}, {249000, 0, 31348, 75}, {250500, 0, 32480, 75}, {252000, 0, 30804, 75}, {253500, 0, 29546, 75},
    {255000, 0, 29546, 75}, {256500, 0, 28983, 75}, {258000, 0, 29172, 75}, {259500, 0, 28459, 75}, {261000, 0, 28459, 75},
    {262500, 0, 30997, 75}, {264000, 0, 27919, 75}, {265500, 0, 29127, 75}, {267000, 0, 29127, 75}, {268500, 0, 30590, 75},
    {270000, 0, 30778, 75}, {271500, 0, 33351, 75}, {273000, 0, 33351, 75}, {274500, 0, 31055, 75}, {276000, 0, 33060, 75},
    {277500, 0, 33060, 75}, {279000, 0, 34069, 75}, {280500, 0, 32614, 75}, {282000, 0, 35907, 75}, {283500, 0, 35907, 75},
    {285000, 0, 35323, 75}, {286500, 0, 35744, 75}, {288000, 0, 35006, 75}, {289500, 0, 35006, 75}, {291000, 0, 36516, 75},
    {292500, 0, 32383, 75}, {294000, 0, 34393, 75}, {295500, 0, 34393, 75}, {297000, 0, 33579, 75}, {298500, 0, 33748, 75},
    {300000, 0, 42144, 75}, {301500, 0, 42144, 75}, {303000, 0, 37994, 75}, {304500, 0, 39280, 75}, {306000, 0, 36831, 75},
    {307500, 0, 36831, 75}, {309000, 0, 34179, 75}, {310500, 0, 38341, 75}, {312000, 0, 41088, 75}, {313500, 0, 41088, 75},
    {315000, 0, 38247, 75}, {316500, 0, 41595, 75}, {318000, 0, 43240, 75}, {319500, 0, 43240, 75}, {321000, 0, 43956, 75},
    {322500, 0, 40200, 75}, {324000, 1, 42029, 65}, {325500, 0, 42029, 65}, {327000, 0, 30822, 65}, {328500, 0, 26232, 65},
    {330000, 0, 26506, 70}, {331500, 0, 26506, 75}, {333000, 0, 32356, 75}, {334500, 0, 39761, 75}, {336000, 0, 37645, 75},
    {337500, 0, 37645, 75}, {339000, 0, 41471, 75}, {340500, 0, 43727, 75}, {342000, 1, 52465, 65}, {343500, 1, 52465, 55},
    {345000, 0, 34741, 55}, {346500, 0, 20449, 55}, {348000, 0, 13735, 55}, {349500, 0, 13735, 60}, {351000, 0, 13735, 65},
    {352500, 0, 20092, 70}, {354000, 1, 25802, 60}, {355500, 1, 25802, 50}, {357000, 0, 19060, 50}, {358500, 0, 12366, 50},
    {360000, 0, 9103, 50}, {361500, 0, 9103, 55}, {363000, 0, 7762, 60}, {364500, 0, 9959, 65}, {366000, 0, 13120, 70},
    {367500, 0, 13120, 75}, {369000, 0, 20965, 75}, {370500, 0, 28273, 75}, {372000, 0, 32931, 75}, {373500, 0, 32931, 75},
    {375000, 0, 35581, 75}, {376500, 0, 38559, 75}, {378000, 0, 37789, 75}, {379500, 0, 37789, 75}, {381000, 1, 42040, 65},
    {382500, 1, 36806, 55}, {384000, 0, 21212, 55}, {385500, 0, 21212, 55}, {387000, 0, 11311, 55}, {388500, 0, 6362, 60},
    {390000, 0, 3888, 65}, {391500, 0, 3888, 70}, {393000, 0, 2652, 75}, {394500, 0, 2032, 75},
};
constexpr int SolS1QualityCap = 75;

/// Replays SolS1 from \a fromMs to \a toMs into \a state (1080p, adaptive, own client with HEVC/AV1,
/// Sol's software encoders). Returns the time of the first switch to a slow-link codec, if any.
std::optional<int> replaySolS1(State &state, Clock::time_point &now, int fromMs, int toMs, QString *reason = nullptr)
{
    auto in = input(SoftwareEncoding::Auto, softwareEverything());
    in.qualityCap = SolS1QualityCap;
    for (const auto &tick : SolS1) {
        if (tick.ms < fromMs || tick.ms > toMs) continue;
        now = T0 + std::chrono::milliseconds(tick.ms);
        in.congested = tick.congested;
        in.bandwidthKbps = quint32(tick.kbps);
        in.quality = quint8(tick.quality);
        const auto d = step(state, in, now);
        if (state.slowLink) {
            if (reason) *reason = d.reason;
            return tick.ms;
        }
    }
    return std::nullopt;
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
        run(state, in, now, 270s);
        QCOMPARE(*state.current, (Choice{Family::Avc, true}));
        d = run(state, in, now, 60s);
        QCOMPARE(*state.current, (Choice{Family::Av1, false})); // tried again after the block
        // AUD-FIX5 D5: said as a retry, at the preset the guard needed last time.
        QVERIFY2(d.reason.startsWith(u"CPU guard: retrying software av1 after 3"), qPrintable(d.reason));
        QCOMPARE(state.preset, Preset::Fastest);
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

        // A small change (< 15 %) is not worth a reopen.
        in.bandwidthKbps = quint32(1700 / TargetShareOfGoodput * 0.9);
        now += 11s;
        s = step(state, in, now);
        QVERIFY(!s.settingsChanged);

        // AUD-FIX4 D2: not congested, the cap is probed up 50 % after every 10 s clear, up to the
        // slow-link threshold; 20 s clear there is sustained headroom: back to hardware HEVC. The
        // goodput stays demand-limited (the encoder sends what it may), which the old "above
        // 25 Mbit/s" rule could never see.
        in.congested = false;
        QList<quint32> caps;
        QList<Clock::time_point> capTimes;
        Decision back{};
        const auto clearFrom = now;
        for (int i = 0; i < 200 && state.slowLink; ++i) {
            now += 1500ms;
            in.bandwidthKbps = state.applied.targetKbps;
            const quint32 before = state.linkKbps;
            s = step(state, in, now);
            if (state.linkKbps != before && state.linkKbps != 0) {
                caps.append(state.linkKbps);
                capTimes.append(now);
            }
            if (s.changed) back = s;
        }
        const quint32 ceiling = quint32(slowBelowKbps(ReferencePixels));
        QVERIFY(caps.size() >= 4);
        QCOMPARE(caps.first(), 2550u); // 1700 x 1.5
        for (qsizetype i = 1; i < caps.size(); ++i) {
            QCOMPARE(caps[i], std::min(ceiling, quint32(caps[i - 1] * ProbeGrowth)));
            QVERIFY(capTimes[i] - capTimes[i - 1] >= ProbeHold);
        }
        QCOMPARE(caps.last(), ceiling);
        QVERIFY(!state.slowLink);
        QVERIFY(back.changed);
        QVERIFY2(back.reason.contains(u"without congestion"), qPrintable(back.reason));
        QVERIFY(now - capTimes.last() >= RecoverHold);
        // Bounded: per step ProbeHold sending at the cap, after AV1's raise interval.
        QVERIFY(now - clearFrom <= (ProbeHold + RestartBitrateRaiseInterval + 3s) * caps.size() + RecoverHold + 5s);
        QCOMPARE(*state.current, (Choice{Family::Hevc, true}));
        QCOMPARE(state.applied.targetKbps, 0u);
    }

    // AUD-FIX4 D2 (Sol, 2026-09-27 pass 3): a 6 Mbit/s throttle sends the stream to software
    // AV1; once the throttle is gone the policy must come back to AVC in bounded time (it stayed
    // on AV1 for the rest of the run: goodput 3-8 Mbit/s never passed "25 Mbit/s"), and then stay.
    void throttleRemovedReturnsToAvc()
    {
        auto in = input(SoftwareEncoding::Auto, softwareEverything());
        in.quality = 75;
        State state;
        auto now = T0;
        QCOMPARE(step(state, in, now).choice, (Choice{Family::Avc, false})); // no hardware: libx264
        // The link: `capacity` kbit/s. AVC in quality mode on motion wants 20 Mbit/s; software
        // AV1 sends its target. Goodput is what got through; more demand than capacity is
        // congestion (RTT up, the window full).
        quint32 capacity = 6000;
        QList<std::pair<Clock::time_point, Choice>> switches;
        const auto tick = [&] {
            now += 1500ms;
            const quint32 demand = softwarePrivate(*state.current) ? state.applied.targetKbps : 20000u;
            in.bandwidthKbps = std::min(demand, capacity);
            in.congested = demand > capacity;
            const auto d = step(state, in, now);
            if (d.changed) switches.append({now, d.choice});
            return d;
        };
        while (now - T0 < 60s) tick();
        QVERIFY(state.slowLink);
        QCOMPARE(*state.current, (Choice{Family::Av1, false}));
        QCOMPARE(switches.size(), 1);

        // Throttle removed.
        capacity = 100000;
        const auto removed = now;
        Decision back{};
        while (softwarePrivate(*state.current) && now - removed < 300s) {
            const auto d = tick();
            if (d.changed) back = d;
        }
        const auto took = now - removed;
        qInfo() << "back to avc after" << std::chrono::duration_cast<std::chrono::seconds>(took).count() << "s:" << back.reason;
        QCOMPARE(*state.current, (Choice{Family::Avc, false}));
        QVERIFY2(back.reason.contains(u"link recovered"), qPrintable(back.reason));
        // Bounded: a probe that failed under the throttle waits up to 8x ProbeHold, then three
        // steps from ~5 Mbit/s to 15 (each after AV1's raise interval), then RecoverHold.
        QVERIFY(took <= ProbeHold * (1 << ProbeMaxDoublings) + (ProbeHold + RestartBitrateRaiseInterval + 3s) * 3 + RecoverHold + 10s);
        // And it stays: 10 min of a good link, no more switches.
        const auto settled = switches.size();
        while (now - removed < 900s) tick();
        QCOMPARE(switches.size(), settled);
        QVERIFY(!state.slowLink);
    }

    // AUD-FIX4 D2: a link that really is slow never "recovers" (the probe keeps running into
    // congestion, and backs off), and when a recovery does not hold the next one waits longer;
    // switches never come closer than MinSwitchInterval.
    void recoveryDoesNotFlap()
    {
        // \a avcKbps / \a av1Kbps: what the link carries for each stream (AVC in quality mode on
        // motion wants 20 Mbit/s, software AV1 sends its target).
        const auto simulate = [](quint32 avcKbps, quint32 av1Kbps, std::chrono::seconds duration, int *failedProbes = nullptr) {
            auto in = input(SoftwareEncoding::Auto, softwareEverything());
            in.quality = 75;
            State state;
            auto now = T0;
            step(state, in, now);
            QList<std::pair<Clock::time_point, Choice>> switches;
            int congestedTicks = 0;
            int probes = 0;
            while (now - T0 < duration) {
                now += 1500ms;
                const bool av1 = softwarePrivate(*state.current);
                const quint32 demand = av1 ? state.applied.targetKbps : 20000u;
                const quint32 capacity = av1 ? av1Kbps : avcKbps;
                in.bandwidthKbps = std::min(demand, capacity);
                in.congested = demand > capacity;
                if (in.congested && av1) ++congestedTicks;
                const auto before = state.lastProbeAt;
                const auto d = step(state, in, now);
                if (state.lastProbeAt != before && state.lastProbeAt == now) ++probes;
                if (d.changed) switches.append({now, d.choice});
            }
            if (failedProbes) *failedProbes = congestedTicks;
            qInfo() << "avc" << avcKbps << "av1" << av1Kbps << ":" << switches.size() << "switches," << probes << "probes," << congestedTicks << "congested AV1 ticks in"
                    << duration.count() << "s";
            return switches;
        };
        // 5 and 14.8 Mbit/s: AV1 once, and there it stays; the probes that run into the link
        // back off, so AV1 is congested only a small part of the time.
        for (const quint32 kbps : {5000u, 14800u}) {
            int congested = 0;
            const auto switches = simulate(kbps, kbps, 1800s, &congested);
            QCOMPARE(switches.size(), 1);
            QCOMPARE(switches.first().second, (Choice{Family::Av1, false}));
            QVERIFY(congested < 1800 / 1.5 * 0.15);
        }
        // 16 Mbit/s is not slow for AVC (over 15): no switch at all.
        auto switches = simulate(16000, 16000, 1800s);
        QCOMPARE(switches.size(), 0);
        // A link that carries AV1 at the ceiling cleanly but reads slow and congested for AVC:
        // every recovery fails, and each AV1 stretch before the next try is longer (back-off).
        switches = simulate(12000, 100000, 3600s);
        QVERIFY(switches.size() >= 4);
        for (qsizetype i = 1; i < switches.size(); ++i) {
            QVERIFY(switches[i].first - switches[i - 1].first >= MinSwitchInterval);
        }
        QList<Clock::duration> av1Stays;
        for (qsizetype i = 1; i < switches.size(); ++i) {
            if (switches[i - 1].second.family == Family::Av1 && switches[i].second.family == Family::Avc) {
                av1Stays.append(switches[i].first - switches[i - 1].first);
            }
        }
        QVERIFY(av1Stays.size() >= 3);
        QVERIFY(av1Stays[1] > av1Stays[0]);
        QVERIFY(av1Stays[2] > av1Stays[1]);
        QVERIFY(switches.size() <= 3600 / 120); // the back-off: a try every ~5 minutes at most
    }

    // AUD-FIX4 D3 (Sol pass 3): Mandelbrot on software AV1 ran krdpserver at 180 % of a core
    // (p95 314 %) and delivered 13-23 of 30 fps, but the guard divided by all 16 threads and
    // never fired. The sample now estimates the encode time over the encoder's own threads.
    void cpuGuardSeesTheSolMandelbrot()
    {
        const int threads = softwareEncoderThreads(Family::Av1, 16);
        QCOMPARE(threads, 8);
        QCOMPARE(softwareEncoderThreads(Family::Avc, 16), 16);
        QCOMPARE(softwareEncoderThreads(Family::Hevc, 4), 2);
        QCOMPARE(softwareEncoderThreads(Family::Av1, 16, 4), 4); // KPIPEWIRE_SW_ENCODER_THREADS
        // One 1.5 s interval: 180 % CPU, 17 fps delivered.
        const double cpuMs = 1.80 * 1500;
        const int frames = 25;
        const double oldSample = cpuMs / frames / 16 / (1000.0 / 30);
        QVERIFY(oldSample < CpuGuardLimit); // why it never fired: ~0.2
        const auto mandelbrot = encodeLoadSample(cpuMs, frames, 1.5, 30, Family::Av1, threads);
        QVERIFY(mandelbrot);
        qInfo() << "mandelbrot sample" << *mandelbrot << "(was" << oldSample << ")";
        QVERIFY(*mandelbrot > CpuGuardLimit);
        // Its p95 (314 %) is far over, too.
        QVERIFY(*encodeLoadSample(3.14 * 1500, frames, 1.5, 30, Family::Av1, threads) > 1.5);
        // testsrc2 on AV1 M10 (157 %, 28 fps) kept up in pass 3: under the limit.
        QVERIFY(*encodeLoadSample(1.57 * 1500, 42, 1.5, 30, Family::Av1, threads) < CpuGuardLimit);
        // PERF.md, SVT-AV1 M10 video on Sol: 189 % at a full 30 fps, mean latency 18.2 ms.
        QVERIFY(std::abs(estimatedEncodeMs(1.89 * 1500, 45, Family::Av1, threads) - 18.2) < 3.0);
        // libx264 at 90 % (pass 1, AVC in software): nowhere near.
        QVERIFY(*encodeLoadSample(0.90 * 1500, 45, 1.5, 30, Family::Avc, 16) < 0.2);
        // A still desktop: too few frames to judge, or cheap ones with a background CPU.
        QVERIFY(!encodeLoadSample(0.05 * 1500, 3, 1.5, 30, Family::Av1, threads));
        QVERIFY(*encodeLoadSample(0.05 * 1500, 6, 1.5, 30, Family::Av1, threads) < 0.35);
        // Delivered frames short of the cap while the encoder is busy count on their own.
        const double busy = *encodeLoadSample(1.20 * 1500, 30, 1.5, 30, Family::Av1, threads); // 20 fps
        QVERIFY(busy > CpuGuardLimit);
        QVERIFY(*encodeLoadSample(1.20 * 1500, 45, 1.5, 30, Family::Av1, threads) < CpuGuardLimit); // the same CPU at 30 fps
    }

    // AUD-FIX4 D3: encode times like the Mandelbrot case (p95 about 50 ms at 30 fps) step the
    // preset down within the guard's window.
    void cpuGuardStepsDownOnMandelbrotEncodeTimes()
    {
        auto in = input(SoftwareEncoding::Prefer, softwareEverything(), {Family::Av1});
        State state;
        auto now = T0;
        QCOMPARE(step(state, in, now).choice, (Choice{Family::Av1, false}));
        const int threads = softwareEncoderThreads(Family::Av1, 16);
        const auto intervalSample = [&](double encodeMs, int frames) {
            const double cpuMs = encodeMs * softwareEncoderParallelism(Family::Av1, threads) * frames;
            return *encodeLoadSample(cpuMs, frames, 1.5, 30, Family::Av1, threads);
        };
        LoadWindow window;
        for (int i = 0; i < 8; ++i) { // 12 s of a desktop: 10 ms per frame
            now += 1500ms;
            window.add(intervalSample(10, 45));
            in.encodeLoadP95 = window.p95();
            QVERIFY(!step(state, in, now).settingsChanged);
        }
        QCOMPARE(state.preset, Preset::Efficient);
        const auto motion = now;
        Decision stepped{};
        for (int i = 0; i < LoadWindow::Size && state.preset == Preset::Efficient; ++i) {
            now += 1500ms;
            // Mandelbrot: 40-50 ms per frame, 20-25 fps delivered.
            window.add(intervalSample(i % 2 ? 50 : 40, i % 2 ? 30 : 37));
            in.encodeLoadP95 = window.p95();
            stepped = step(state, in, now);
        }
        QCOMPARE(state.preset, Preset::Balanced); // SVT M10 -> M11
        QVERIFY(now - motion <= 1500ms * LoadWindow::Size);
        QVERIFY2(stepped.settingsReason.contains(u"CPU guard"), qPrintable(stepped.settingsReason));
    }

    // AUD-FIX4 D4 (Sol pass 3): adaptive quality swinging every ~6 s (549 <-> 5231 kbit/s) reopened
    // SVT-AV1 26 times in 4.5 min. Non-live restarts are now >= 10 s apart (rises >= 20 s), need a
    // 30 % change, and the changes that come while one waits merge into one.
    void av1RestartsAreSpacedOnASwingingQuality()
    {
        auto in = input(SoftwareEncoding::Prefer, softwareEverything(), {Family::Av1});
        State state;
        auto now = T0;
        step(state, in, now);
        QList<Clock::time_point> restarts;
        QList<std::pair<quint32, quint32>> changes;
        int qualitySwings = 0;
        quint8 last = 0;
        for (auto t = 0s; t < 300s;) {
            now += 1500ms;
            t = std::chrono::duration_cast<std::chrono::seconds>(now - T0);
            const quint8 quality = (t.count() / 6) % 2 ? 35 : 75;
            if (quality != last) ++qualitySwings;
            last = quality;
            in.quality = quality;
            const quint32 before = state.applied.targetKbps;
            const int count = state.encoderRestarts;
            const auto d = step(state, in, now);
            if (state.encoderRestarts != count) {
                restarts.append(now);
                changes.append({before, d.settings.targetKbps});
            }
        }
        qInfo() << restarts.size() << "AV1 restarts for" << qualitySwings << "quality swings in 300 s";
        QVERIFY(qualitySwings >= 45);
        QVERIFY(!restarts.isEmpty());
        QVERIFY(restarts.size() <= 300 / 10);
        for (qsizetype i = 1; i < restarts.size(); ++i) {
            QVERIFY(restarts[i] - restarts[i - 1] >= MinReconfigureInterval);
            if (changes[i].second > changes[i].first) {
                QVERIFY(restarts[i] - restarts[i - 1] >= RestartBitrateRaiseInterval);
            }
        }
        for (const auto &[from, to] : changes) {
            QVERIFY(std::abs(double(to) - from) / from >= RestartBitrateMinChange);
        }
    }

    // AUD-FIX4 D5: the reply says why AVC, accurately.
    void avcReasonIsAccurate()
    {
        const QList<Family> both{Family::Hevc, Family::Av1};
        QCOMPARE(avcChoiceReason(softwareEverything(), SoftwareEncoding::Auto, true, both),
                 QStringLiteral("hardware encoder not available for hevc/av1; software not selected (link not slow)"));
        QCOMPARE(avcChoiceReason(sol(), SoftwareEncoding::Auto, true, both), QStringLiteral("no encoder for hevc/av1 on this host"));
        QVERIFY(avcChoiceReason(softwareEverything(), SoftwareEncoding::Never, true, both).contains(u"software encoding is off"));
        QVERIFY(avcChoiceReason(softwareEverything(), SoftwareEncoding::Auto, false, both).contains(u"fixed its codec"));
        auto mixed = sol();
        mixed.av1 = {false, true};
        QCOMPARE(avcChoiceReason(mixed, SoftwareEncoding::Auto, true, both),
                 QStringLiteral("no encoder for hevc on this host; hardware encoder not available for av1; software not selected (link not slow)"));
        // And the policy agrees: Sol with software HEVC/AV1 on a normal link answers AVC.
        auto in = input(SoftwareEncoding::Auto, softwareEverything());
        State state;
        QCOMPARE(step(state, in, T0).choice.family, Family::Avc);
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

    // AUD-SWENC: adaptive quality's steps become target bitrates on software HEVC/AV1.
    void qualityMapsToBitrate()
    {
        const quint32 at80 = qualityKbps(80, ReferencePixels);
        QVERIFY2(at80 > 6000 && at80 < 6500, qPrintable(QString::number(at80))); // 0.1 bit/pixel at 1080p30
        QVERIFY(std::abs(double(qualityKbps(100, ReferencePixels)) - 2.0 * at80) <= 2); // +20 points = twice the bits
        QVERIFY(std::abs(double(qualityKbps(60, ReferencePixels)) - at80 / 2.0) <= 2);
        QVERIFY(std::abs(double(qualityKbps(80, 2 * ReferencePixels)) - 2.0 * at80) <= 2); // two monitors
        QCOMPARE(qualityKbps(250, ReferencePixels), qualityKbps(100, ReferencePixels)); // quality is 0..100
        QCOMPARE(qualityKbps(10, 1), MinTargetKbps); // never below the floor
        quint32 previous = 0;
        for (int q = 0; q <= 100; q += 5) {
            const quint32 kbps = qualityKbps(quint8(q), ReferencePixels);
            QVERIFY(kbps >= previous);
            previous = kbps;
        }
        // One adaptive-quality step down (10) is a 29 % cut, one step up (5) +19 %.
        QVERIFY(double(qualityKbps(70, ReferencePixels)) / at80 < 0.72);
        QVERIFY(double(qualityKbps(85, ReferencePixels)) / at80 > 1.18);

        // In the policy: software HEVC/AV1 always run in bitrate mode at the quality's bitrate,
        // capped by a slow link; hardware and software H.264 stay in quality mode.
        State sw;
        auto in = input(SoftwareEncoding::Prefer, softwareEverything(), {Family::Hevc});
        in.quality = 50;
        QCOMPARE(step(sw, in, T0).settings.targetKbps, qualityKbps(50, ReferencePixels));
        State hw;
        QCOMPARE(step(hw, input(SoftwareEncoding::Auto, hal()), T0).settings.targetKbps, 0u);
        State avc;
        QCOMPARE(step(avc, input(SoftwareEncoding::Auto, sol()), T0).settings.targetKbps, 0u);
        QVERIFY(restartsEncoder(Family::Av1, softwareEverything().av1, {false, Preset::Efficient, 3000, 30}, {false, Preset::Efficient, 2000, 30}));
        QVERIFY(!restartsEncoder(Family::Hevc, softwareEverything().hevc, {false, Preset::Efficient, 3000, 30}, {false, Preset::Efficient, 2000, 30}));
        QVERIFY(restartsEncoder(Family::Hevc, softwareEverything().hevc, {false, Preset::Efficient, 3000, 30}, {false, Preset::Balanced, 3000, 30}));
        QVERIFY(restartsEncoder(Family::Hevc, softwareEverything().hevc, {false, Preset::Efficient, 0, 30}, {false, Preset::Efficient, 3000, 30}));
        QVERIFY(!restartsEncoder(Family::Hevc, softwareEverything().hevc, {false, Preset::Efficient, 3000, 30}, {false, Preset::Efficient, 3000, 15}));
    }

    // With libx265 (live bitrate), every adaptive-quality change reaches the encoder as a live
    // bitrate change, and none restarts it.
    void adaptiveQualityNeverRestartsHevc()
    {
        auto in = input(SoftwareEncoding::Prefer, softwareEverything(), {Family::Hevc});
        State state;
        auto now = T0;
        QCOMPARE(step(state, in, now).choice, (Choice{Family::Hevc, false}));
        using namespace std::chrono_literals;
        const auto run = runAdaptiveQuality(state, in, now, 300s, {6s, 20s, 3s, 12s});
        qInfo() << "hevc:" << run.qualityChanges << "quality changes," << run.bitrateChanges.size() << "live bitrate changes," << state.encoderRestarts << "restarts";
        QVERIFY(run.qualityChanges >= 30);
        QCOMPARE(state.encoderRestarts, 0);
        QVERIFY(run.restartTimes.isEmpty());
        QCOMPARE(run.bitrateChanges.size(), run.qualityChanges); // each one applied at once
        QCOMPARE(*state.current, (Choice{Family::Hevc, false})); // never left the codec
    }

    // Without a live bitrate change (SVT-AV1 reopens), the same run restarts the encoder only
    // rarely: at least RestartBitrateInterval apart and only for a >= 30 % step.
    void adaptiveQualityRestartsAv1RateLimited()
    {
        auto in = input(SoftwareEncoding::Prefer, softwareEverything(), {Family::Av1});
        State state;
        auto now = T0;
        QCOMPARE(step(state, in, now).choice, (Choice{Family::Av1, false}));
        using namespace std::chrono_literals;
        const auto run = runAdaptiveQuality(state, in, now, 300s, {6s, 20s, 3s, 12s});
        qInfo() << "av1:" << run.qualityChanges << "quality changes," << state.encoderRestarts << "restarts";
        QVERIFY(run.qualityChanges >= 30);
        QVERIFY(state.encoderRestarts > 0); // it still follows the quality
        QVERIFY(state.encoderRestarts <= 300 / 5);
        QVERIFY(state.encoderRestarts * 2 < run.qualityChanges);
        for (qsizetype i = 1; i < run.restartTimes.size(); ++i) {
            QVERIFY(run.restartTimes[i] - run.restartTimes[i - 1] >= RestartBitrateInterval);
        }
        for (const auto &[from, to] : run.bitrateChanges) {
            QVERIFY(std::abs(double(to) - from) / from >= RestartBitrateMinChange);
        }
        // (Before AUD-SWENC each of these quality changes was a CRF change: a reopen each.)
        // A KPipeWire without the live libx265 path: HEVC is rate-limited the same way.
        auto old = softwareEverything();
        old.hevc.liveBitrate = false;
        auto hevcIn = input(SoftwareEncoding::Prefer, old, {Family::Hevc});
        State hevc;
        auto t = T0;
        step(hevc, hevcIn, t);
        const auto oldRun = runAdaptiveQuality(hevc, hevcIn, t, 300s, {6s, 20s, 3s, 12s});
        QVERIFY(hevc.encoderRestarts <= 300 / 5);
        QVERIFY(hevc.encoderRestarts * 2 < oldRun.qualityChanges);
    }

    // The CPU guard steps a preset back up after a sustained low load, one real level at a time.
    void presetStepsBackUpAfterSustainedLowLoad_data()
    {
        QTest::addColumn<int>("family");
        QTest::addColumn<QList<Preset>>("ladder");
        QTest::newRow("hevc") << int(Family::Hevc) << QList<Preset>{Preset::Balanced, Preset::Efficient};
        QTest::newRow("av1") << int(Family::Av1) << QList<Preset>{Preset::Efficient}; // Fastest = Balanced in SVT-AV1 2.3
    }
    void presetStepsBackUpAfterSustainedLowLoad()
    {
        QFETCH(int, family);
        QFETCH(QList<Preset>, ladder);
        auto in = input(SoftwareEncoding::Prefer, softwareEverything(), {Family(family)});
        State state;
        auto now = T0;
        QCOMPARE(step(state, in, now).choice, (Choice{Family(family), false}));
        state.preset = Preset::Fastest; // where the guard left it
        in.encodeLoadP95 = 0.35;
        QList<Preset> presets;
        QList<Clock::time_point> when;
        const auto lowSince = now;
        for (int i = 0; i < 100; ++i) {
            now += 1500ms;
            const auto d = step(state, in, now);
            QVERIFY(!d.changed);
            if (d.settingsChanged && d.settings.preset != Preset::Fastest && (presets.isEmpty() || presets.last() != d.settings.preset)) {
                presets.append(d.settings.preset);
                when.append(now);
                QVERIFY2(d.settingsReason.contains(u"preset"), qPrintable(d.settingsReason));
                QVERIFY(d.restartsEncoder);
            }
        }
        QCOMPARE(presets, ladder);
        QVERIFY(when.first() - lowSince >= PresetRecoverHold);
        for (qsizetype i = 1; i < when.size(); ++i) {
            QVERIFY(when[i] - when[i - 1] >= PresetRecoverHold); // one level per sustained window
        }
        // A load between the thresholds (no guard step down, no step up) changes nothing.
        State middle;
        auto t = T0;
        step(middle, in, t);
        middle.preset = Preset::Fastest;
        middle.applied.preset = Preset::Fastest;
        in.encodeLoadP95 = 0.55;
        for (int i = 0; i < 100; ++i) {
            t += 1500ms;
            QVERIFY(!step(middle, in, t).settingsChanged);
        }
        QCOMPARE(middle.preset, Preset::Fastest);
    }

    // A load only the faster preset can carry: raising the preset overloads it, the guard steps
    // it down again, and so on. The flap guard (PresetFlapWindow, doubling) keeps that rare.
    void presetRecoveryDoesNotFlap()
    {
        auto in = input(SoftwareEncoding::Prefer, softwareEverything(), {Family::Hevc});
        State state;
        auto now = T0;
        step(state, in, now);
        state.preset = Preset::Balanced;
        state.applied.preset = Preset::Balanced;
        QList<Clock::time_point> changes;
        for (int i = 0; i < 400; ++i) { // 10 minutes
            now += 1500ms;
            // x265 veryfast costs about twice superfast here: 0.38 -> 0.76, over the 0.70 limit.
            in.encodeLoadP95 = state.preset == Preset::Efficient ? 0.76 : 0.38;
            const auto d = step(state, in, now);
            QVERIFY(!d.changed);
            QVERIFY(state.preset != Preset::Fastest); // superfast carries it; never further down
            if (d.settingsChanged) changes.append(now);
        }
        qInfo() << changes.size() << "preset changes in 10 minutes";
        QVERIFY(changes.size() >= 2); // it did try the slower preset
        QVERIFY(changes.size() <= 6); // about 30 without the flap guard (up after 30 s, down 10 s later)
        QCOMPARE(state.preset, Preset::Balanced);
    }

    // On the way back up the frame rate comes first (the guard's last step down), then the preset.
    void frameRateRecoversBeforePreset()
    {
        auto in = input(SoftwareEncoding::Prefer, softwareEverything(), {Family::Hevc});
        State state;
        auto now = T0;
        step(state, in, now);
        state.preset = Preset::Fastest;
        state.applied.preset = Preset::Fastest;
        state.guardFrameRate = 15;
        state.applied.maxFrameRate = 15;
        in.encodeLoadP95 = 0.2;
        QStringList order;
        for (int i = 0; i < 120; ++i) {
            now += 1500ms;
            const auto d = step(state, in, now);
            if (d.settingsChanged && d.settingsReason.contains(u"frame rate")) order << QStringLiteral("rate");
            if (d.settingsChanged && d.settingsReason.contains(u"preset")) order << QStringLiteral("preset");
        }
        // 15 -> 30 fps is visible; 30 -> uncapped is not (software HEVC stays at 30 fps) but must
        // come first too: the preset waits for the guard's frame-rate step to be undone.
        QCOMPARE(order, (QStringList{QStringLiteral("rate"), QStringLiteral("preset"), QStringLiteral("preset")}));
        QCOMPARE(state.preset, Preset::Efficient);
        QVERIFY(!state.guardFrameRate);
    }

    // AUD-FIX5 D2: the Sol S1 pass replayed (SolS1). Under the 6 Mbit/s tbf congestion came in
    // 1.5-3 s runs every 9-14 s (the D1 in-flight cap drains the backlog), so "5 s unbroken" never
    // fired and the link was never judged slow. The window must see it within ~15 s of the first
    // congestion.
    void slowLinkFromTheSolThrottle()
    {
        const auto firstCongestedFrom = [](int fromMs) {
            for (const auto &tick : SolS1) {
                if (tick.ms >= fromMs && tick.congested) return tick.ms;
            }
            return -1;
        };
        // From just before the tbf.
        State state;
        auto now = T0;
        QString reason;
        const auto at = replaySolS1(state, now, 43000, 194000, &reason);
        QVERIFY(at);
        const int first = firstCongestedFrom(44000);
        qInfo() << "tbf pass: first congestion at T+" << first / 1000.0 << "s, slow at T+" << *at / 1000.0 << "s:" << reason;
        QVERIFY(*at - first <= 15000);
        QVERIFY2(reason.startsWith(u"slow link (congested in"), qPrintable(reason));
        QCOMPARE(*state.current, (Choice{Family::Av1, false}));
        QCOMPARE(state.applied.maxFrameRate, 30);
        QVERIFY(state.applied.targetKbps > 0 && state.applied.targetKbps < 6000);

        // From the end of the stream's warm-up: the link that went slow before the tbf (send
        // rate down to ~3 Mbit/s at T+23 s) is caught as well.
        State early;
        auto t = T0;
        const auto earlyAt = replaySolS1(early, t, 20000, 194000);
        QVERIFY(earlyAt);
        const int earlyFirst = firstCongestedFrom(20000);
        qInfo() << "from warm-up: first congestion at T+" << earlyFirst / 1000.0 << "s, slow at T+" << *earlyAt / 1000.0 << "s";
        QVERIFY(*earlyAt - earlyFirst <= 15000);
    }

    // AUD-FIX5 D2: ...and back once the throttle is gone (the AUD-FIX4 recovery, from the state
    // the replay left): AV1 at the probed cap, the link then carries anything.
    void solThrottleRemovedReturnsToAvc()
    {
        State state;
        auto now = T0;
        QVERIFY(replaySolS1(state, now, 43000, 194000));
        auto in = input(SoftwareEncoding::Auto, softwareEverything());
        in.quality = 75;
        in.qualityCap = SolS1QualityCap;
        const auto removed = now;
        Decision back{};
        while (softwarePrivate(*state.current) && now - removed < 300s) {
            now += 1500ms;
            const quint32 demand = softwarePrivate(*state.current) ? state.applied.targetKbps : 20000u;
            in.bandwidthKbps = demand;
            in.congested = false;
            const auto d = step(state, in, now);
            if (d.changed) back = d;
        }
        const auto took = std::chrono::duration_cast<std::chrono::seconds>(now - removed);
        qInfo() << "back to avc after" << took.count() << "s:" << back.reason;
        QCOMPARE(*state.current, (Choice{Family::Avc, false}));
        QVERIFY2(back.reason.contains(u"link recovered"), qPrintable(back.reason));
        QVERIFY(took <= (ProbeHold + RestartBitrateRaiseInterval + 3s) * 4 + RecoverHold + 10s);
    }

    // AUD-FIX5 D2: a normal link with a congestion blip now and then is not slow, even when its
    // goodput is demand-limited under the threshold. The real adaptive quality climbs back to its
    // cap between blips; and the Sol free-link stretch (mandelbrot, T+218 s on) stays normal.
    void normalLinkWithShortCongestionIsNotSlow()
    {
        using namespace std::chrono;
        State sol;
        auto t = T0;
        QVERIFY(!replaySolS1(sol, t, 218000, 1000000));
        QCOMPARE(*sol.current, (Choice{Family::Avc, false}));

        // \a congestedAt(tick): whether tick n is congested; \a kbpsAt(tick): the goodput.
        const auto simulate = [](const auto &congestedAt, const auto &kbpsAt, int ticks) -> std::optional<int> {
            auto in = input(SoftwareEncoding::Auto, softwareEverything());
            in.qualityCap = 75;
            State state;
            auto now = T0;
            int quality = 75;
            Clock::time_point lastStepDown{};
            for (int n = 0; n < ticks; ++n) {
                now += 1500ms;
                const bool congested = congestedAt(n);
                const auto result = KRdp::AdaptiveQuality::step({
                    .current = quality,
                    .cap = 75,
                    .averageRtt = congested ? microseconds(60000) : microseconds(10000),
                    .minimumRtt = microseconds(10000),
                    .backlogged = false,
                    .climbAllowed = now - lastStepDown >= KRdp::AdaptiveQuality::ClimbHoldAfterStepDown,
                });
                if (result.next < quality) lastStepDown = now;
                quality = result.next;
                in.quality = quint8(quality);
                in.congested = congested;
                in.bandwidthKbps = quint32(kbpsAt(n));
                const auto d = step(state, in, now);
                if (state.slowLink) {
                    qInfo() << "slow at tick" << n << ":" << d.reason;
                    return n;
                }
            }
            return std::nullopt;
        };
        // 30 min: a one-tick blip every 30 s and a two-tick one every 60 s, in between (so a blip
        // every 15 s on average); 4-12 Mbit/s (a light desktop never fills a LAN).
        const auto blips = [](int n) {
            return n % 20 == 5 || n % 40 == 15 || n % 40 == 16;
        };
        const auto light = [](int n) {
            return 4000 + (n * 7919) % 8000;
        };
        QVERIFY(!simulate(blips, light, 1200));
        // The same link at 40 Mbit/s of motion: never slow either.
        QVERIFY(!simulate(blips, [](int) { return 40000; }, 1200));
        // The Sol pattern with the same adaptive quality: two congested ticks every 8 (12 s), 2-4.5
        // Mbit/s. Slow within 15 s of the first congestion (tick 0).
        const auto saturated = [](int n) {
            return n % 8 < 2;
        };
        const auto slow = simulate(saturated, [](int n) { return 2000 + (n * 7919) % 2500; }, 200);
        QVERIFY(slow);
        QVERIFY(*slow * 1500 <= 15000);
    }

    // AUD-FIX5 D5 (Sol S2, `prefer`): after the guard's AV1 -> HEVC -> AVC fallback, AV1's block ran
    // out after 120 s and the policy went round again twice (reason "encoders or client codecs
    // changed"). Now a retry says so, and under sustained heavy load the retries back off: 5, 10,
    // 20, 40, 60 min, the failed retry falling straight through to AVC.
    void guardRetryBacksOff()
    {
        auto in = input(SoftwareEncoding::Prefer, softwareEverything());
        State state;
        auto now = T0;
        QCOMPARE(step(state, in, now).choice, (Choice{Family::Av1, false}));
        struct Switch {
            Clock::time_point at;
            Choice to;
            QString reason;
        };
        QList<Switch> switches;
        // Full-screen motion for 3 h: no software HEVC/AV1 preset keeps up; libx264 does (0.55).
        while (now - T0 < 3h) {
            now += 1500ms;
            in.encodeLoadP95 = softwarePrivate(*state.current) ? 1.3 : 0.55;
            const auto d = step(state, in, now);
            if (d.changed) switches.append({now, d.choice, d.reason});
        }
        QList<Clock::time_point> retries;
        for (const auto &s : switches) {
            QVERIFY2(!s.reason.contains(u"encoders or client codecs changed"), qPrintable(s.reason));
            if (s.reason.startsWith(u"CPU guard: retrying")) {
                retries.append(s.at);
                QVERIFY2(s.reason.contains(u"retrying software av1 after"), qPrintable(s.reason)); // the best codec, not HEVC
            }
        }
        qInfo() << switches.size() << "switches," << retries.size() << "retries in 3 h";
        for (const auto &s : switches) {
            qInfo().noquote() << "  +" << std::chrono::duration_cast<std::chrono::seconds>(s.at - T0).count() << "s" << familyName(s.to.family) << ":" << s.reason;
        }
        // No more than one retry in any 10 min.
        for (qsizetype i = 1; i < retries.size(); ++i) {
            QVERIFY(retries[i] - retries[i - 1] >= 10min);
        }
        // The gaps grow: 5 min (+ the fallback), then about 10, 20, 40, 60 (the cap).
        QVERIFY(retries.size() >= 4);
        QVERIFY(retries[1] - retries[0] > retries[0] - T0);
        QVERIFY(retries[2] - retries[1] > retries[1] - retries[0]);
        QVERIFY(retries.last() - retries[retries.size() - 2] <= CpuBlockMax + 2min);
        // Every failed retry goes AV1 -> AVC directly (HEVC is held back too): two switches each.
        QVERIFY(switches.size() <= 2 + 2 * retries.size() + 1);
    }

    // D5: the back-off starts over once the content gets lighter: the running encoder stays under
    // PresetRecoverBelow for GuardForgiveAfter.
    void guardBackOffStartsOverWhenTheLoadDrops()
    {
        auto in = input(SoftwareEncoding::Prefer, softwareEverything());
        State state;
        auto now = T0;
        step(state, in, now);
        const auto heavy = [&](Clock::duration duration) {
            const auto end = now + duration;
            while (now < end) {
                now += 1500ms;
                in.encodeLoadP95 = softwarePrivate(*state.current) ? 1.3 : 0.55;
                step(state, in, now);
            }
        };
        heavy(8min); // fallback, one failed retry at 5 min
        QCOMPARE(state.guardRejections[size_t(Family::Av1)], 2);
        QCOMPARE(*state.current, (Choice{Family::Avc, false}));
        // Light content on libx264 for 6 min.
        const auto end = now + 6min;
        while (now < end) {
            now += 1500ms;
            in.encodeLoadP95 = 0.2;
            step(state, in, now);
        }
        QCOMPARE(state.guardRejections[size_t(Family::Av1)], 0);
        QCOMPARE(state.guardRejections[size_t(Family::Hevc)], 0);
        // The next AV1 retry (its 10 min block ran out meanwhile) that fails is blocked 5 min, not 20.
        heavy(3min);
        QCOMPARE(state.guardRejections[size_t(Family::Av1)], 1);
        const auto blockedFor = state.softwareBlockedUntil[size_t(Family::Av1)] - state.guardBlockedAt[size_t(Family::Av1)];
        QVERIFY(blockedFor == Clock::duration(CpuBlockFor));
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
