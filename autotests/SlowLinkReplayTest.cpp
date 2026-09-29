// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX14: the real gate traces replayed through LinkEvidence and the codec policy, as
// VideoStream runs them every 1.5 s (judgeLink(), then stepCodecPolicy()).
//
// The traces (SlowLinkTraces.inc) come from the evidence of the failed gates:
// - the Stats-for-Nerds panel screenshots (one every 13-15 s): the server's own stats samples as the
//   client drew them: TCP capacity (capacityKbps, capacitySource tcp; "-" = no estimate), the round
//   trip, the ack delay, frames in flight and the window, and the running retransmit count;
// - the server journal's "Bandwidth measurement" lines (every ~2 s): the rate sent.
// Between screenshots every panel value is held; retransmits are spread evenly. The round trip in
// the panel is RDP's (the client's thread answers it), which is at least TCP's: it is fed as TCP's,
// the harder case. The send queue is what is in flight (frames x the mean frame size), the sender
// application-limited when that is under a window's worth. The client took what it took: the
// session was throttled throughout, and congested (AdaptiveQuality) whenever the round trip was
// inflated or the window full.

#include "CodecPolicy.h"
#include "LinkEvidence.h"

#include <QTest>

#include <span>

using namespace KRdp;
using namespace std::chrono_literals;

namespace
{
struct PanelRow {
    int tMs;
    int receivedKbps;
    int capacityKbps; ///< -1: none
    int rttTenthsMs;
    int ackMs;
    int inFlight;
    int window;
    int retransmits; ///< running total
};
struct SentSample {
    int tMs;
    int kbps;
};

#include "SlowLinkTraces.inc"

using Clock = CodecPolicy::Clock;
const Clock::time_point T0 = Clock::time_point(1h);
constexpr qint64 TwoMonitors = 2LL * 1920 * 1080;

struct Trace {
    std::span<const PanelRow> panel;
    std::span<const SentSample> sent;
    int tbfOnMs;
    int tbfOffMs;
    CodecPolicy::Family family; ///< what the client decoded in hardware
};

struct Outcome {
    std::optional<int> slowAtMs; ///< the first slow link
    std::optional<int> slowInTbfAtMs; ///< the first slow link at or after tbfOn
    std::optional<int> recoveredAtMs; ///< the first recovery at or after tbfOff
    int slowBeforeTbfMs = 0; ///< total time slow before the tbf
    int slowAfterRecoveryMs = 0; ///< total time slow after that recovery
    int clientLimitedBeforeTbf = 0; ///< intervals client-limited before the tbf
    int intervalsBeforeTbf = 0;
    int clientLimitedEvents = 0;
    int probes = 0;
    bool codecChanged = false;
    QStringList log;
};

/// \a capacityScale / \a rttScale: deterministic jitter per interval (1.0 = the trace as is).
Outcome replay(const Trace &trace, double capacityJitter = 0.0, double rttJitter = 0.0)
{
    Outcome out;
    LinkEvidence::State evidence;
    CodecPolicy::State policy;
    CodecPolicy::Input in;
    in.mode = CodecPolicy::SoftwareEncoding::Auto;
    in.encoders.avc = {true, true};
    in.encoders.hevc = {true, true};
    in.encoders.av1 = {true, true};
    in.client = {trace.family};
    in.pixels = TwoMonitors;
    in.qualityCap = 80;
    in.quality = 80;
    CodecPolicy::step(policy, in, T0);
    const auto first = *policy.current;
    const double slowBelow = CodecPolicy::slowBelowKbps(TwoMonitors);
    const int endMs = trace.panel.back().tMs + 13000;
    size_t row = 0;
    size_t sent = 0;
    int i = 0;
    for (int t = 1500; t <= endMs; t += 1500, ++i) {
        while (row + 1 < trace.panel.size() && trace.panel[row + 1].tMs <= t) ++row;
        while (sent + 1 < trace.sent.size() && trace.sent[sent + 1].tMs <= t) ++sent;
        const PanelRow &p = trace.panel[row];
        const PanelRow &next = row + 1 < trace.panel.size() ? trace.panel[row + 1] : p;
        const double shake = double((i * 7919 + 13) % 101) / 100.0 - 0.5; // -0.5..0.5
        const double shake2 = double((i * 104729 + 7) % 97) / 96.0 - 0.5;
        const int sentKbps = trace.sent[sent].tMs <= t ? trace.sent[sent].kbps : p.receivedKbps;
        const double rttMs = std::max(1.0, p.rttTenthsMs / 10.0 * (1.0 + 2.0 * rttJitter * shake2));
        const double fps = std::max(1, 25);
        const qint64 frameBytes = qint64(double(sentKbps) * 1000.0 / 8.0 / fps);
        // Retransmits between two screenshots, spread evenly (running total, interpolated).
        const double span = std::max(1, next.tMs - p.tMs);
        const double along = std::clamp(double(t - p.tMs) / span, 0.0, 1.0);
        const quint64 retransmits = quint64(p.retransmits + (next.retransmits - p.retransmits) * along);

        LinkEvidence::Signals sig;
        sig.at = T0 + std::chrono::milliseconds(t);
        sig.congested = rttMs >= 7.0 || p.inFlight >= p.window;
        sig.throttled = true;
        sig.sentKbps = quint32(sentKbps);
        // The panel's capacity is the estimate's maximum over the 10 s before the screenshot: it
        // stands for the intervals within 10 s before it, the previous one for the rest.
        const PanelRow &estimate = next.tMs > t && next.tMs - t <= 10000 ? next : p;
        if (estimate.capacityKbps >= 0) {
            sig.capacityKbps = quint32(std::max(1.0, estimate.capacityKbps * (1.0 + 2.0 * capacityJitter * shake)));
        }
        sig.slowBelowKbps = slowBelow;
        LinkEvidence::Socket socket;
        socket.minRttUs = 1900;
        socket.rttUs = qint64(rttMs * 1000.0);
        socket.totalRetransmits = retransmits;
        socket.queuedBytes = qint64(p.inFlight) * frameBytes;
        // No estimate: every delivery-rate sample of the last 10 s was application-limited.
        socket.appLimited = !sig.capacityKbps || socket.queuedBytes < LinkEvidence::SendQueueBackedUp;
        sig.socket = socket;
        const auto verdict = LinkEvidence::judge(evidence, sig);

        in.bandwidthKbps = quint32(sentKbps);
        in.congested = sig.congested;
        in.throttled = true;
        in.networkLimited = verdict.network;
        in.throttledByLink = verdict.throttledByLink;
        in.clientLimited = verdict.clientLimited;
        in.capacityKbps = sig.capacityKbps;
        in.linkSlow = verdict.slow;
        in.linkFast = verdict.fast;
        in.linkWhy = verdict.linkWhy;
        in.capacityNow = verdict.capacityNow;
        in.linkIdle = verdict.idle;
        const bool wasSlow = policy.slowLink;
        const auto decision = CodecPolicy::step(policy, in, sig.at);
        out.probes += decision.capacityProbe ? 1 : 0;
        out.codecChanged = out.codecChanged || *policy.current != first;
        if (verdict.clientLimitedChanged) {
            ++out.clientLimitedEvents;
            out.log << QStringLiteral("%1 s: %2 (%3)").arg(t / 1000.0).arg(verdict.clientLimited ? u"client-limited"_qs : u"no longer client-limited"_qs, verdict.why);
        }
        if (policy.slowLink != wasSlow) {
            out.log << QStringLiteral("%1 s: %2").arg(t / 1000.0).arg(decision.linkReason);
            if (policy.slowLink) {
                if (!out.slowAtMs) out.slowAtMs = t;
                if (t >= trace.tbfOnMs && !out.slowInTbfAtMs) out.slowInTbfAtMs = t;
            } else if (t >= trace.tbfOffMs && !out.recoveredAtMs) {
                out.recoveredAtMs = t;
            }
        }
        if (t < trace.tbfOnMs) {
            ++out.intervalsBeforeTbf;
            out.clientLimitedBeforeTbf += verdict.clientLimited ? 1 : 0;
            out.slowBeforeTbfMs += policy.slowLink ? 1500 : 0;
        }
        if (out.recoveredAtMs && t > *out.recoveredAtMs && policy.slowLink) {
            out.slowAfterRecoveryMs += 1500;
        }
        const auto limit = LinkEvidence::classify(policy.slowLink, verdict, false);
        if (qEnvironmentVariableIsSet("KRDP_SIM_TRACE")) {
            qInfo().noquote() << QStringLiteral("t=%1 sent=%2 cap=%3 rtt=%4 rtx=%5 cong=%6 q=%7 net=%8 slow=%9 fast=%10")
                                     .arg(t / 1000.0)
                                     .arg(sentKbps)
                                     .arg(sig.capacityKbps.value_or(0))
                                     .arg(rttMs)
                                     .arg(retransmits)
                                     .arg(sig.congested)
                                     .arg(socket.queuedBytes / 1024)
                                     .arg(verdict.network.value_or(false))
                                     .arg(policy.slowLink)
                                     .arg(verdict.fast)
                                 + QStringLiteral(" cl=%1 limit=%2").arg(verdict.clientLimited).arg(QLatin1String(LinkEvidence::limitName(limit)));
        }
    }
    return out;
}

Trace a312776()
{
    return {A312776GPanel, A312776GSent, A312776GTbfOnMs, A312776GTbfOffMs, CodecPolicy::Family::Hevc};
}
}

class SlowLinkReplayTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // The gate that failed (a312776, Buzz on Wi-Fi): unthrottled 4 min 50 s without a slow link and
    // client-limited; the 6 Mbit/s tbf slow within ~20 s; recovered within 90 s of its removal and
    // not slow again; HEVC in hardware throughout.
    void a312776WifiGate_data()
    {
        QTest::addColumn<double>("capacityJitter");
        QTest::addColumn<double>("rttJitter");
        QTest::newRow("as recorded") << 0.0 << 0.0;
        QTest::newRow("capacity +-25 %, rtt +-50 %") << 0.25 << 0.5;
        QTest::newRow("capacity +-40 %, rtt +-100 %") << 0.4 << 1.0;
    }
    void a312776WifiGate()
    {
        QFETCH(double, capacityJitter);
        QFETCH(double, rttJitter);
        const auto trace = a312776();
        const auto out = replay(trace, capacityJitter, rttJitter);
        const QString log = out.log.join(u"\n"_qs);
        qInfo().noquote() << log;
        QVERIFY2(out.slowBeforeTbfMs == 0, qPrintable(log));
        QVERIFY2(out.slowInTbfAtMs, qPrintable(log));
        const int enterS = (*out.slowInTbfAtMs - trace.tbfOnMs) / 1000;
        qInfo() << "slow" << enterS << "s after the tbf";
        QVERIFY2(enterS <= 21, qPrintable(log));
        QVERIFY2(out.recoveredAtMs, qPrintable(log));
        const int recoverS = (*out.recoveredAtMs - trace.tbfOffMs) / 1000;
        qInfo() << "recovered" << recoverS << "s after the tbf went";
        QVERIFY2(recoverS <= 90, qPrintable(log));
        QCOMPARE(out.slowAfterRecoveryMs, 0);
        QVERIFY2(out.clientLimitedBeforeTbf >= out.intervalsBeforeTbf / 2,
                 qPrintable(QStringLiteral("%1 of %2\n%3").arg(out.clientLimitedBeforeTbf).arg(out.intervalsBeforeTbf).arg(log)));
        QVERIFY(log.contains(u"client-limited: "_qs) || log.contains(u"s: client-limited"_qs));
        QVERIFY(!out.codecChanged);
    }

    // The same Wi-Fi session with no throttle at all: the unthrottled part replayed for 10 min.
    void a312776UnthrottledTenMinutes()
    {
        auto trace = a312776();
        std::vector<PanelRow> rows;
        std::vector<SentSample> sent;
        for (int lap = 0; lap < 3; ++lap) {
            const int offset = lap * trace.tbfOnMs;
            for (const auto &p : trace.panel) {
                if (p.tMs >= trace.tbfOnMs) break;
                auto copy = p;
                copy.tMs += offset;
                copy.retransmits += lap * 41;
                rows.push_back(copy);
            }
            for (const auto &s : trace.sent) {
                if (s.tMs < 0 || s.tMs >= trace.tbfOnMs) continue;
                sent.push_back({s.tMs + offset, s.kbps});
            }
        }
        trace.panel = rows;
        trace.sent = sent;
        trace.tbfOnMs = trace.tbfOffMs = 1 << 30;
        const auto out = replay(trace, 0.3, 0.8);
        QVERIFY2(!out.slowAtMs, qPrintable(out.log.join(u"\n"_qs)));
        QVERIFY(out.clientLimitedBeforeTbf >= out.intervalsBeforeTbf / 2);
    }

    // 573fa31's runs on cray (AV1 in hardware; the policy of the time declared slow 9-46 s in and
    // never recovered): the tbf is slow within ~20 s (run L), and wherever a slow link is declared
    // the way back takes at most 90 s after the tbf went.
    void f573Runs_data()
    {
        QTest::addColumn<int>("run");
        QTest::newRow("L") << 0;
        QTest::newRow("L2") << 1;
    }
    void f573Runs()
    {
        QFETCH(int, run);
        const Trace trace = run == 0 ? Trace{F573LPanel, F573LSent, F573LTbfOnMs, F573LTbfOffMs, CodecPolicy::Family::Av1}
                                     : Trace{F573L2Panel, F573L2Sent, F573L2TbfOnMs, F573L2TbfOffMs, CodecPolicy::Family::Av1};
        const auto out = replay(trace);
        const QString log = out.log.join(u"\n"_qs);
        qInfo().noquote() << log;
        QVERIFY2(out.slowBeforeTbfMs == 0, qPrintable(log));
        if (run == 0) {
            QVERIFY2(out.slowInTbfAtMs && *out.slowInTbfAtMs - trace.tbfOnMs <= 21000, qPrintable(log));
        }
        if (out.slowInTbfAtMs) {
            QVERIFY2(out.recoveredAtMs && *out.recoveredAtMs - trace.tbfOffMs <= 90000, qPrintable(log));
            QCOMPARE(out.slowAfterRecoveryMs, 0);
        }
    }
};

QTEST_GUILESS_MAIN(SlowLinkReplayTest)
#include "SlowLinkReplayTest.moc"
