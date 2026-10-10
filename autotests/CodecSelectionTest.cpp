// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-062 S1: per-connection codec selection (src/CodecSelection.h, CodecPolicy::select with an
// ordered request). Table-driven from the decision tables and worked examples of
// docs/superpowers/specs/2026-10-09-per-connection-codec-selection-design.md (4.1-4.3, 6.3, 10).
// Pure: fakes only, no device is opened.

#include "CodecPolicy.h"

#include <QTest>

using namespace KRdp::CodecPolicy;
using namespace std::chrono_literals;

namespace
{
const Clock::time_point T0 = Clock::time_point(1h);

// Host fixtures (spec 2.3). Software encoders exist for every codec (libx264, libx265, SVT-AV1).
Encoders hal() // AMD VCN: AVC/HEVC/AV1 hardware
{
    Encoders e;
    e.avc = {true, true, true};
    e.hevc = {true, true, true};
    e.av1 = {true, true, false};
    return e;
}
Encoders sol() // RTX 2070: HEVC NVENC; AVC and AV1 software only
{
    Encoders e;
    e.avc = {false, true, true};
    e.hevc = {true, true, true};
    e.av1 = {false, true, false};
    return e;
}
Encoders cray() { return hal(); }
Encoders ace() // Intel: AVC/HEVC hardware, AV1 software
{
    Encoders e = hal();
    e.av1 = {false, true, false};
    return e;
}
Encoders noEncoders() { return {}; }

struct Host {
    const char *name;
    Encoders (*make)();
};
const Host Hosts[] = {{"hal", hal}, {"sol", sol}, {"cray", cray}, {"ace", ace}};

// Client fixture: Buzz decodes HEVC in hardware (VA-API), AV1 and AVC in software only (spec C2).
Request buzz(const QList<Family> &order, Mode encode = Mode::Any, Mode decode = Mode::Any)
{
    Request r;
    r.order = order;
    r.encode = encode;
    r.decode = decode;
    r.decoders[size_t(Family::Avc)] = {false, true};
    r.decoders[size_t(Family::Hevc)] = {true, true};
    r.decoders[size_t(Family::Av1)] = {false, true};
    return r;
}
const QList<Family> Default{Family::Hevc, Family::Av1, Family::Avc};
const QList<Family> Av1First{Family::Av1, Family::Hevc, Family::Avc};

Plan planOf(const Request &r, const Encoders &e, const HostLimits &h = {})
{
    return KRdp::CodecPolicy::plan(r, e, h);
}
bool hasReason(const Plan &p, Family f, SkipReason why)
{
    for (const Skip &s : p.skipped) {
        if (s.family == f) return s.reasons.contains(why);
    }
    return false;
}
}

class CodecSelectionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ---- worked examples (spec 4.3) -------------------------------------------------------------
    void defaultsGiveHevcHardwareOnEveryHost_data()
    {
        QTest::addColumn<int>("host");
        for (size_t i = 0; i < std::size(Hosts); ++i) QTest::newRow(Hosts[i].name) << int(i);
    }
    void defaultsGiveHevcHardwareOnEveryHost()
    {
        QFETCH(int, host);
        const Plan p = planOf(buzz(Default), Hosts[host].make());
        QVERIFY(!p.useBaseline());
        QCOMPARE(p.choice(), (Candidate{Family::Hevc, true}));
        QVERIFY(p.skipped.isEmpty());
    }

    void goalCaseAv1SoftwareDecode_data()
    {
        QTest::addColumn<int>("host");
        QTest::addColumn<bool>("av1Hardware");
        QTest::newRow("hal") << 0 << true; // AV1 on the VCN, decoded in software
        QTest::newRow("sol") << 1 << false; // SVT-AV1
        QTest::newRow("cray") << 2 << true;
        QTest::newRow("ace") << 3 << false;
    }
    void goalCaseAv1SoftwareDecode()
    {
        QFETCH(int, host);
        QFETCH(bool, av1Hardware);
        const Plan p = planOf(buzz(Av1First, Mode::Any, Mode::Software), Hosts[host].make());
        QCOMPARE(p.choice(), (Candidate{Family::Av1, av1Hardware}));
        QVERIFY(!p.useBaseline());
    }
    void goalCaseOnAceFallsToHevcWhenTheCpuGuardHoldsAv1()
    {
        HostLimits h;
        h.softwareHeld[size_t(Family::Av1)] = true;
        const Plan p = planOf(buzz(Av1First, Mode::Any, Mode::Software), ace(), h);
        QCOMPARE(p.choice(), (Candidate{Family::Hevc, true}));
        QVERIFY(hasReason(p, Family::Av1, SkipReason::CpuGuard));
    }

    void goalCaseHostForbidsSoftwareAv1()
    {
        HostLimits h;
        h.allowance.av1 = false;
        const Request r = buzz(Av1First, Mode::Any, Mode::Software);
        // Hardware AV1 stays possible (Hal, cray): the allowance concerns software only.
        QCOMPARE(planOf(r, hal(), h).choice(), (Candidate{Family::Av1, true}));
        QCOMPARE(planOf(r, cray(), h).choice(), (Candidate{Family::Av1, true}));
        for (const auto &make : {sol, ace}) {
            const Plan p = planOf(r, make(), h);
            QCOMPARE(p.choice(), (Candidate{Family::Hevc, true}));
            QVERIFY(hasReason(p, Family::Av1, SkipReason::SoftwareNotAllowed));
            QVERIFY(hasReason(p, Family::Av1, SkipReason::NoHardwareEncoder));
        }
    }

    void hardwareBothEnds_data()
    {
        QTest::addColumn<int>("host");
        QTest::addColumn<bool>("av1NoHardwareEncoder");
        QTest::newRow("hal") << 0 << false;
        QTest::newRow("sol") << 1 << true;
        QTest::newRow("cray") << 2 << false;
        QTest::newRow("ace") << 3 << true;
    }
    void hardwareBothEnds()
    {
        QFETCH(int, host);
        QFETCH(bool, av1NoHardwareEncoder);
        const Plan p = planOf(buzz(Av1First, Mode::Hardware, Mode::Hardware), Hosts[host].make());
        QCOMPARE(p.choice(), (Candidate{Family::Hevc, true}));
        QVERIFY(hasReason(p, Family::Av1, SkipReason::ClientCannotDecodeInHardware));
        QCOMPARE(hasReason(p, Family::Av1, SkipReason::NoHardwareEncoder), av1NoHardwareEncoder);
    }

    void avcOnlyHardwareBothEndsIsTheBaseline_data()
    {
        QTest::addColumn<int>("host");
        QTest::addColumn<bool>("baselineHardware");
        QTest::newRow("hal") << 0 << true;
        QTest::newRow("sol") << 1 << false; // no AVC hardware on Sol (spec C3)
        QTest::newRow("cray") << 2 << true;
        QTest::newRow("ace") << 3 << true;
    }
    void avcOnlyHardwareBothEndsIsTheBaseline()
    {
        QFETCH(int, host);
        QFETCH(bool, baselineHardware);
        const Plan p = planOf(buzz({Family::Avc}, Mode::Hardware, Mode::Hardware), Hosts[host].make());
        QVERIFY(p.useBaseline());
        QCOMPARE(p.choice(), (Candidate{Family::Avc, baselineHardware}));
        QVERIFY(hasReason(p, Family::Avc, SkipReason::ClientCannotDecodeInHardware));
        QCOMPARE(hasReason(p, Family::Avc, SkipReason::NoHardwareEncoder), !baselineHardware);
    }

    void softwareEncodeAnyDecode_data()
    {
        QTest::addColumn<int>("host");
        for (size_t i = 0; i < std::size(Hosts); ++i) QTest::newRow(Hosts[i].name) << int(i);
    }
    void softwareEncodeAnyDecode()
    {
        QFETCH(int, host);
        const Plan p = planOf(buzz(Default, Mode::Software, Mode::Any), Hosts[host].make());
        QCOMPARE(p.choice(), (Candidate{Family::Hevc, false}));
        for (const Candidate &c : p.candidates) QVERIFY(!c.hardware);
    }

    // ---- decision tables 4.1 and 4.2 -----------------------------------------------------------------
    void encodeTable_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("hw");
        QTest::addColumn<bool>("sw");
        QTest::addColumn<bool>("allowed");
        QTest::addColumn<int>("outcome"); // 0 unavailable, 1 hardware, 2 software
        QTest::addColumn<int>("reason"); // SkipReason or -1
        const int none = -1;
        const auto R = [](SkipReason r) { return int(r); };
        QTest::newRow("hw/hw") << int(Mode::Hardware) << true << true << true << 1 << none;
        QTest::newRow("hw/no-hw") << int(Mode::Hardware) << false << true << true << 0 << R(SkipReason::NoHardwareEncoder);
        QTest::newRow("sw/yes/allowed") << int(Mode::Software) << true << true << true << 2 << none;
        QTest::newRow("sw/never") << int(Mode::Software) << true << true << false << 0 << R(SkipReason::SoftwareNotAllowed);
        QTest::newRow("sw/absent") << int(Mode::Software) << true << false << true << 0 << R(SkipReason::NoSoftwareEncoder);
        QTest::newRow("any/hw") << int(Mode::Any) << true << true << true << 1 << none;
        QTest::newRow("any/sw-only") << int(Mode::Any) << false << true << true << 2 << none;
        QTest::newRow("any/sw-never") << int(Mode::Any) << false << true << false << 0 << R(SkipReason::SoftwareNotAllowed);
        QTest::newRow("any/nothing") << int(Mode::Any) << false << false << true << 0 << R(SkipReason::NoEncoder);
    }
    void encodeTable()
    {
        QFETCH(int, mode);
        QFETCH(bool, hw);
        QFETCH(bool, sw);
        QFETCH(bool, allowed);
        QFETCH(int, outcome);
        QFETCH(int, reason);
        Encoders e;
        e.hevc = {hw, sw, true};
        HostLimits h;
        h.allowance.hevc = allowed;
        const Plan p = planOf(buzz({Family::Hevc, Family::Avc}, Mode(mode)), e, h);
        const bool hevcFirst = !p.candidates.isEmpty() && p.candidates.first().family == Family::Hevc;
        if (outcome == 0) {
            QVERIFY(!hevcFirst);
            QVERIFY(hasReason(p, Family::Hevc, SkipReason(reason)));
        } else {
            QVERIFY(hevcFirst);
            QCOMPARE(p.candidates.first().hardware, outcome == 1);
        }
    }

    void decodeTable_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("hw");
        QTest::addColumn<bool>("sw");
        QTest::addColumn<int>("reason"); // -1 = decodable
        QTest::newRow("hw/yes") << int(Mode::Hardware) << true << true << -1;
        QTest::newRow("hw/no") << int(Mode::Hardware) << false << true << int(SkipReason::ClientCannotDecodeInHardware);
        QTest::newRow("sw/yes") << int(Mode::Software) << false << true << -1;
        QTest::newRow("sw/no") << int(Mode::Software) << true << false << int(SkipReason::ClientCannotDecode);
        QTest::newRow("any/hw") << int(Mode::Any) << true << false << -1;
        QTest::newRow("any/sw") << int(Mode::Any) << false << true << -1;
        QTest::newRow("any/none") << int(Mode::Any) << false << false << int(SkipReason::ClientCannotDecode);
    }
    void decodeTable()
    {
        QFETCH(int, mode);
        QFETCH(bool, hw);
        QFETCH(bool, sw);
        QFETCH(int, reason);
        Request r = buzz({Family::Av1, Family::Avc}, Mode::Any, Mode(mode));
        r.decoders[size_t(Family::Av1)] = {hw, sw};
        const Plan p = planOf(r, hal());
        if (reason < 0) {
            QCOMPARE(p.choice().family, Family::Av1);
        } else {
            QVERIFY(hasReason(p, Family::Av1, SkipReason(reason)));
        }
    }

    void codecMajorOrderInsideAny()
    {
        // HEVC hardware, HEVC software, AV1 hardware, AV1 software, AVC (spec Q3).
        const Plan p = planOf(buzz(Default), hal());
        const QList<Candidate> expected{{Family::Hevc, true}, {Family::Hevc, false}, {Family::Av1, true}, {Family::Av1, false}, {Family::Avc, true}, {Family::Avc, false}};
        QCOMPARE(p.candidates, expected);
    }
    void orderIsHonoured()
    {
        QCOMPARE(planOf(buzz(Av1First), hal()).choice(), (Candidate{Family::Av1, true}));
        QCOMPARE(planOf(buzz({Family::Avc, Family::Hevc}), hal()).choice(), (Candidate{Family::Avc, true}));
        // Avc is appended last when missing.
        QCOMPARE(normalizedOrder({Family::Av1}), (QList<Family>{Family::Av1, Family::Avc}));
    }

    // ---- edge cases (spec 10) ------------------------------------------------------------------------
    void nothingSatisfiableIsAvcBaseline()
    {
        const Plan p = planOf(buzz(Default, Mode::Hardware), noEncoders());
        QVERIFY(p.useBaseline());
        QCOMPARE(p.choice(), (Candidate{Family::Avc, false}));
        QVERIFY(hasReason(p, Family::Hevc, SkipReason::NoHardwareEncoder));
    }
    void hardwareOnlyWithoutHardwareOnSol()
    {
        const Plan p = planOf(buzz({Family::Av1, Family::Avc}, Mode::Hardware), sol());
        QVERIFY(p.useBaseline());
        QCOMPARE(p.choice(), (Candidate{Family::Avc, false})); // truthful: the baseline is software
    }
    void hostForbidsAllSoftwareLeavesAvcAsLastResort()
    {
        HostLimits h;
        h.allowance = allowanceFromSoftwareNever(true);
        const Plan p = planOf(buzz(Default, Mode::Software), sol(), h);
        QVERIFY(p.useBaseline()); // software AVC only as the baseline
        QCOMPARE(p.choice(), (Candidate{Family::Avc, false}));
        QVERIFY(hasReason(p, Family::Hevc, SkipReason::SoftwareNotAllowed));
        QVERIFY(hasReason(p, Family::Avc, SkipReason::SoftwareNotAllowed));
        // The same host, encode Any: hardware HEVC still works.
        QCOMPARE(planOf(buzz(Default), sol(), h).choice(), (Candidate{Family::Hevc, true}));
    }
    void hardwareThatCannotDoTheSizeCountsAsUnavailable()
    {
        QVERIFY(hardwareFits(Family::Avc, 4096, 2160));
        QVERIFY(!hardwareFits(Family::Avc, 5120, 1440));
        QVERIFY(hardwareFits(Family::Hevc, 5120, 1440));
        HostLimits h;
        h.hardwareUnfit[size_t(Family::Avc)] = true;
        Plan p = planOf(buzz({Family::Avc}, Mode::Hardware), hal(), h);
        QVERIFY(p.useBaseline());
        QCOMPARE(p.choice(), (Candidate{Family::Avc, false}));
        QVERIFY(hasReason(p, Family::Avc, SkipReason::SizeLimit));
        // Any: software AVC takes over as a candidate.
        p = planOf(buzz({Family::Avc}, Mode::Any), hal(), h);
        QVERIFY(!p.useBaseline());
        QCOMPARE(p.choice(), (Candidate{Family::Avc, false}));
    }
    void avc444FollowsAvcEncodeMode()
    {
        QVERIFY(avc444Allowed(Mode::Any, true));
        QVERIFY(avc444Allowed(Mode::Hardware, true));
        QVERIFY(!avc444Allowed(Mode::Software, false));
        QVERIFY(!avc444Allowed(Mode::Software, true));
        QVERIFY(!avc444Allowed(Mode::Any, false)); // software H.264 is 4:2:0
    }
    void modeParsing()
    {
        QCOMPARE(parseMode(u" Hardware "), std::optional<Mode>(Mode::Hardware));
        QVERIFY(!parseMode(u"gpu"));
        QCOMPARE(skippedText({{Family::Av1, {SkipReason::SoftwareNotAllowed, SkipReason::NoHardwareEncoder}}}), QStringLiteral("av1 (softwareNotAllowed+noHardwareEncoder)"));
    }

    // ---- select(): slow link, CPU guard, flips stay inside the request (spec 6.3) -------------------
    void slowLinkPicksTheBestCompressingCandidateOfTheList()
    {
        Input in;
        in.encoders = hal();
        in.request = buzz({Family::Hevc, Family::Avc}); // AV1 is not in the list
        State st;
        st.slowLink = true;
        const Selection s = selectDetailed(in, st, T0);
        QCOMPARE(s.choice, (Choice{Family::Hevc, true}));
        in.request = buzz(Default);
        QCOMPARE(selectDetailed(in, st, T0).choice, (Choice{Family::Av1, true})); // best compression of the list
    }
    void hardwareOnlyIsNeverFlippedToSoftware()
    {
        Input in;
        in.encoders = sol(); // hevc hw, av1 only software
        in.request = buzz(Av1First, Mode::Hardware);
        State st;
        st.slowLink = true;
        QCOMPARE(selectDetailed(in, st, T0).choice, (Choice{Family::Hevc, true}));
        // Even when the guard has blocked software everywhere.
        for (auto &b : st.softwareBlockedUntil) b = T0 + 1h;
        QCOMPARE(selectDetailed(in, st, T0).choice, (Choice{Family::Hevc, true}));
    }
    void cpuGuardMovesToTheNextCandidate()
    {
        Input in;
        in.encoders = sol();
        in.request = buzz(Av1First, Mode::Any, Mode::Software);
        State st;
        QCOMPARE(selectDetailed(in, st, T0).choice, (Choice{Family::Av1, false}));
        st.softwareBlockedUntil[size_t(Family::Av1)] = T0 + 5min;
        const Selection s = selectDetailed(in, st, T0);
        QCOMPARE(s.choice, (Choice{Family::Hevc, true}));
        QVERIFY(!s.baseline);
        QCOMPARE(selectDetailed(in, st, T0 + 6min).choice, (Choice{Family::Av1, false})); // block over: back to the first
    }
    void stepReportsSkippedAndBaseline()
    {
        Input in;
        in.encoders = sol();
        in.request = buzz({Family::Av1, Family::Avc}, Mode::Hardware, Mode::Hardware);
        in.adaptive = false;
        State st;
        const Decision d = step(st, in, T0);
        QVERIFY(d.changed);
        QVERIFY(d.baseline);
        QCOMPARE(d.choice, (Choice{Family::Avc, false}));
        QVERIFY(!d.skipped.isEmpty());
    }
    void nonAdaptiveRequestNeverCompresses()
    {
        Input in;
        in.encoders = hal();
        in.request = buzz(Default);
        in.request->adaptive = false;
        State st;
        st.slowLink = true;
        QCOMPARE(selectDetailed(in, st, T0).choice, (Choice{Family::Hevc, true}));
    }
    void consoleViewersAndStockClientsGetAvc()
    {
        // The brokers call setPrivateCodecPolicy({}) for shared viewers: an empty private list is AVC only.
        Input in;
        in.encoders = hal();
        in.request = requestFromRecord({}, {}, true);
        QCOMPARE(selectDetailed(in, State{}, T0).choice, (Choice{Family::Avc, true}));
    }

    // ---- old clients' records: the order is always honoured (OPT-063), SoftwareEncoding only limits software ----
    void recordDefaultOrderGivesHevcOnHalAndCray()
    {
        // Today (C1) Hal and cray answered AV1 to a client that listed [hevc, av1]; the order wins.
        for (const auto &make : {hal, cray}) {
            Input in;
            in.encoders = make();
            in.request = requestFromRecord({Family::Hevc, Family::Av1}, {}, true);
            QCOMPARE(selectDetailed(in, State{}, T0).choice, (Choice{Family::Hevc, true}));
        }
    }
    void recordOrderIsHonouredWhateverTheHostModeSays()
    {
        // Prefer used to mean "best compression first" and Auto "hardware only on a good link": neither
        // decides any more. Sol has hardware HEVC and software AV1: [av1, hevc] gives AV1 in software in
        // every mode that allows software, [hevc, av1] gives hardware HEVC.
        for (const SoftwareEncoding mode : {SoftwareEncoding::Auto, SoftwareEncoding::Prefer}) {
            Input in;
            in.encoders = sol();
            in.mode = mode;
            in.host.allowance = allowanceFor(mode);
            in.request = requestFromRecord({Family::Av1, Family::Hevc}, {}, true);
            QCOMPARE(selectDetailed(in, State{}, T0).choice, (Choice{Family::Av1, false}));
            in.request = requestFromRecord({Family::Hevc, Family::Av1}, {}, true);
            QCOMPARE(selectDetailed(in, State{}, T0).choice, (Choice{Family::Hevc, true}));
        }
    }
    void recordSoftwareNeverStaysHardwareOnly()
    {
        Input in;
        in.encoders = sol();
        in.mode = SoftwareEncoding::Never;
        in.host.allowance = allowanceFor(SoftwareEncoding::Never);
        in.request = requestFromRecord({Family::Av1}, {}, true);
        State st;
        st.slowLink = true;
        in.adaptive = true;
        QCOMPARE(selectDetailed(in, st, T0).choice, (Choice{Family::Avc, false}));
    }
    void recordRequestsNeverLeaveTheirListExceptForTheBaseline()
    {
        int cases = 0;
        for (int bits = 0; bits < (1 << 9); ++bits) {
            Encoders e;
            e.avc = {bool(bits & 1), bool(bits & 2), true};
            e.hevc = {bool(bits & 4), bool(bits & 8), true};
            e.av1 = {bool(bits & 16), bool(bits & 32), false};
            const QList<Family> clients[] = {{}, {Family::Hevc}, {Family::Av1}, {Family::Av1, Family::Hevc}};
            for (const SoftwareEncoding mode : {SoftwareEncoding::Auto, SoftwareEncoding::Never, SoftwareEncoding::Prefer}) {
                for (const bool slow : {false, true}) {
                    for (const bool blocked : {false, true}) {
                        for (const auto &client : clients) {
                            Input in;
                            in.mode = mode;
                            in.encoders = e;
                            State st;
                            st.slowLink = slow;
                            if (blocked) st.softwareBlockedUntil[size_t(Family::Av1)] = T0 + 5min;
                            in.request = requestFromRecord(client, {}, true);
                            in.host.allowance = allowanceFor(mode);
                            const Selection sel = selectDetailed(in, st, T0);
                            QVERIFY(sel.choice.family == Family::Avc || client.contains(sel.choice.family));
                            if (sel.choice.family != Family::Avc) {
                                // the first listed codec the host can encode (hardware, or software when allowed and not held)
                                const Backends &b = e.of(sel.choice.family);
                                QVERIFY(sel.choice.hardware ? b.hardware : b.software);
                                QVERIFY(!sel.baseline);
                            }
                            ++cases;
                        }
                    }
                }
            }
        }
        QVERIFY(cases > 10000);
    }
    void aFixedCodecIsNotAGuardStepForTheBaseline()
    {
        // OPT-063: the CPU guard may only move to a codec the client listed. [av1] alone has none.
        QVERIFY(!mayLeave(requestFromRecord({Family::Av1}, {}, false), Family::Avc));
        QVERIFY(mayLeave(requestFromRecord({Family::Av1, Family::Hevc}, {}, false), Family::Hevc));
        QVERIFY(mayLeave(requestFromRecord({Family::Av1, Family::Avc}, {}, false), Family::Avc)); // the client listed AVC itself
    }
};

QTEST_APPLESS_MAIN(CodecSelectionTest)
#include "CodecSelectionTest.moc"
