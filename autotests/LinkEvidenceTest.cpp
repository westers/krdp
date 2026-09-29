// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX13/AUD-FIX14: telling a slow link from a slow client by the socket, over windows
// (src/LinkEvidence.h).

#include "LinkEvidence.h"

#include <QTest>

using namespace KRdp::LinkEvidence;
using namespace std::chrono_literals;

namespace
{
Socket lan()
{
    Socket s;
    s.rttUs = 2500;
    s.minRttUs = 2000;
    s.queuedBytes = 0;
    s.appLimited = true;
    return s;
}
Signals slowClient(const Socket &socket, std::optional<quint32> capacity = 38800)
{
    Signals in;
    in.congested = true; // RDP RTT inflated, the window full
    in.throttled = true;
    in.sentKbps = 8300;
    in.capacityKbps = capacity;
    in.socket = socket;
    in.slowBelowKbps = 33000;
    return in;
}
/// A deterministic jitter in [0, 1) for interval \a i.
double jitter(int i)
{
    return double((i * 7919 + 13) % 101) / 101.0;
}
}

class LinkEvidenceTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // cray's L2 baseline: 38.8 Mbit/s TCP capacity, 8.3 sent, 0 retransmits, LAN RTT: headroom,
    // and client-limited after ClientLimitedAfter intervals.
    void slowClientOnFatLink()
    {
        State state;
        Verdict v;
        for (int i = 0; i < ClientLimitedAfter - 1; ++i) {
            v = judge(state, slowClient(lan()));
            QCOMPARE(v.network, std::optional<bool>(false));
            QVERIFY(v.headroom);
            QVERIFY(!v.clientLimited);
        }
        v = judge(state, slowClient(lan()));
        QVERIFY(v.clientLimited);
        QVERIFY(v.clientLimitedChanged);
        QVERIFY2(v.why.contains(u"38800"), qPrintable(v.why));
        QCOMPARE(classify(false, v, false), Limit::Client);
        // A while without any capacity sample: the application-limited sender is headroom enough.
        for (int i = 0; i < 10; ++i) {
            v = judge(state, slowClient(lan(), std::nullopt));
            QVERIFY(v.clientLimited);
        }
        QVERIFY(!v.clientLimitedChanged);
    }

    // AUD-FIX14 (cray a312776, Buzz on 5 GHz Wi-Fi and busy): TCP's RTT swinging 23-61 ms over a
    // 3 ms minimum, retransmits in bursts, the capacity estimate 17-51 Mbit/s, 6-15 Mbit/s sent,
    // the client behind: never link-bound, never slow, and client-limited.
    void wifiNoiseIsNotEvidence()
    {
        State state;
        quint64 retransmits = 0;
        int clientLimited = 0;
        for (int i = 0; i < 400; ++i) {
            Signals in;
            in.congested = jitter(i) < 0.8;
            in.throttled = true;
            in.slowBelowKbps = 30000;
            in.sentKbps = quint32(6000 + 9000 * jitter(i + 3));
            in.capacityKbps = i % 9 == 4 ? std::nullopt : std::optional<quint32>(quint32(17000 + 34000 * jitter(i + 7)));
            Socket s;
            s.minRttUs = 3000;
            s.rttUs = qint64(23000 + 38000 * jitter(i + 11));
            retransmits += i % 5 == 0 ? 4 : (i % 3 == 0 ? 2 : 0);
            s.totalRetransmits = retransmits;
            s.queuedBytes = qint64(20000 + 90000 * jitter(i + 5)); // a frame or two in flight
            s.appLimited = jitter(i + 1) < 0.6;
            in.socket = s;
            const auto v = judge(state, in);
            QVERIFY2(!v.slow && !*v.network, qPrintable(QStringLiteral("interval %1: %2").arg(i).arg(v.linkWhy)));
            QCOMPARE(classify(false, v, false) == Limit::Link, false);
            clientLimited += v.clientLimited ? 1 : 0;
        }
        QVERIFY2(clientLimited > 300, qPrintable(QString::number(clientLimited)));
    }

    // A 6 Mbit/s tbf: the capacity estimate at the throttle, about what is sent: slow once the
    // window is full, and client-limited ends at once.
    void capacityUnderThresholdIsSlow()
    {
        State state;
        for (int i = 0; i < 12; ++i) judge(state, slowClient(lan()));
        QVERIFY(judge(state, slowClient(lan())).clientLimited);
        std::optional<int> slowAt;
        for (int i = 0; i < 20 && !slowAt; ++i) {
            auto in = slowClient(lan(), quint32(5700 + 600 * jitter(i)));
            in.sentKbps = quint32(5200 + 1300 * jitter(i + 1));
            const auto v = judge(state, in);
            if (v.slow) {
                slowAt = i;
                QCOMPARE(v.network, std::optional<bool>(true));
                QVERIFY(!v.clientLimited);
                QVERIFY(v.throttledByLink);
                QCOMPARE(classify(false, v, false), Limit::Link);
                QVERIFY2(v.linkWhy.contains(u"TCP capacity"), qPrintable(v.linkWhy));
            }
        }
        QVERIFY(slowAt);
        // The window (SlowWindow) has to be mostly throttled samples first.
        QVERIFY2(*slowAt >= 3 && *slowAt <= 8, qPrintable(QString::number(*slowAt)));
    }

    // A client-paced stream filling a throttled path only in bursts: 1.4x the sent rate is link-bound
    // only with the RTT inflated and retransmits alongside (they corroborate, never decide).
    void corroborationTipsABorderlineWindow()
    {
        for (const bool corroborated : {false, true}) {
            State state;
            bool slow = false;
            quint64 retransmits = 0;
            for (int i = 0; i < 20; ++i) {
                auto socket = lan();
                if (corroborated) {
                    socket.rttUs = 80000;
                    retransmits += 3;
                    socket.totalRetransmits = retransmits;
                }
                auto in = slowClient(socket, 6000);
                in.sentKbps = 4300;
                slow = slow || judge(state, in).slow;
            }
            QCOMPARE(slow, corroborated);
        }
        // Retransmits and RTT alone, with the capacity well above the sent rate: nothing.
        State state;
        quint64 retransmits = 0;
        for (int i = 0; i < 20; ++i) {
            auto socket = lan();
            socket.rttUs = 80000;
            retransmits += 5;
            socket.totalRetransmits = retransmits;
            const auto v = judge(state, slowClient(socket, 20000));
            QVERIFY(!v.slow);
            QCOMPARE(v.network, std::optional<bool>(false));
        }
    }

    // Without any capacity sample, a send queue that keeps growing under congestion is the link's.
    void queueWithoutCapacity()
    {
        for (const bool congested : {false, true}) {
            State state;
            bool slow = false;
            qint64 queued = 64 * 1024;
            for (int i = 0; i < 12; ++i) {
                auto socket = lan();
                queued += 16 * 1024;
                socket.queuedBytes = queued;
                socket.appLimited = false;
                auto in = slowClient(socket, std::nullopt);
                in.congested = congested;
                slow = slow || judge(state, in).slow;
            }
            QCOMPARE(slow, congested);
        }
    }

    // The way back: the median capacity over RecoverWindow at the recovery threshold (the slow
    // threshold, or RecoverGrowth x what the path carried while it was link-bound); gaps and a
    // few low samples do not matter, RTT spikes and retransmits do not stop it.
    void recoveryFollowsTheMedianCapacity()
    {
        State state;
        for (int i = 0; i < 12; ++i) {
            auto in = slowClient(lan(), 6000);
            in.sentKbps = 5800;
            judge(state, in);
        }
        QCOMPARE(state.boundKbps, 6000u);
        std::optional<int> fastAt;
        quint64 retransmits = 0;
        for (int i = 0; i < 30 && !fastAt; ++i) {
            auto socket = lan();
            socket.rttUs = 45000;
            retransmits += 2;
            socket.totalRetransmits = retransmits;
            // Wi-Fi: 28-33 Mbit/s measured (under the 33 Mbit/s threshold), a gap every third.
            auto in = slowClient(socket, i % 3 == 2 ? std::nullopt : std::optional<quint32>(quint32(28000 + 5000 * jitter(i))));
            in.sentKbps = 9000;
            const auto v = judge(state, in);
            QVERIFY(!(v.slow && v.fast));
            if (v.fast) {
                fastAt = i;
                QCOMPARE(v.recoverKbps, 2.0 * state.boundKbps);
            }
        }
        QVERIFY(fastAt);
        QVERIFY2(*fastAt >= 5 && *fastAt <= 11, qPrintable(QString::number(*fastAt)));
        // Still throttled: no way back.
        State throttled;
        for (int i = 0; i < 60; ++i) {
            auto in = slowClient(lan(), quint32(5500 + 3000 * jitter(i)));
            in.sentKbps = quint32(3000 + 2500 * jitter(i + 2));
            QVERIFY(!judge(throttled, in).fast);
        }
    }

    // The receiver's window limiting the sender is the client's limit: its queue and a low
    // delivery rate do not count against the network.
    void receiveWindowIsTheClients()
    {
        State state;
        auto socket = lan();
        socket.busyUs = 1000000;
        socket.rwndLimitedUs = 0;
        socket.queuedBytes = 100 * 1024;
        socket.appLimited = false;
        judge(state, slowClient(socket, 4000));
        Verdict v;
        for (int i = 0; i < 12; ++i) {
            socket.busyUs = *socket.busyUs + 1500000;
            socket.rwndLimitedUs = *socket.rwndLimitedUs + 1200000; // 80 % of the busy time
            socket.queuedBytes += 30 * 1024;
            v = judge(state, slowClient(socket, 4000));
            QVERIFY(v.rwndLimited);
            QCOMPARE(v.network, std::optional<bool>(false));
            QVERIFY(v.headroom);
        }
        QVERIFY(v.clientLimited);
    }

    // No socket figures: unknown, never client-limited; callers keep the old rule.
    void noSocket()
    {
        State state;
        Signals in;
        in.congested = true;
        in.throttled = true;
        for (int i = 0; i < 4; ++i) {
            const auto v = judge(state, in);
            QVERIFY(!v.network);
            QVERIFY(!v.slow);
            QVERIFY(!v.clientLimited);
            QCOMPARE(classify(false, v, false), Limit::None);
        }
    }

    // Client-limited needs the client to be behind (or paced); it ends after as many calm intervals.
    void hysteresisAndPriority()
    {
        State state;
        Verdict v;
        for (int i = 0; i < ClientLimitedAfter; ++i) v = judge(state, slowClient(lan()));
        QVERIFY(v.clientLimited);
        auto calm = slowClient(lan());
        calm.congested = false;
        calm.throttled = false;
        for (int i = 0; i < ClientLimitedAfter - 1; ++i) {
            v = judge(state, calm);
            QVERIFY(v.clientLimited); // not enough calm intervals yet
        }
        v = judge(state, calm);
        QVERIFY(!v.clientLimited);
        QVERIFY(v.clientLimitedChanged);
        QCOMPARE(classify(false, v, false), Limit::None);
        QCOMPARE(classify(true, v, true), Limit::Link); // a slow link first
        QCOMPARE(classify(false, v, true), Limit::Encoder);
        QCOMPARE(QByteArray(limitName(Limit::Client)), QByteArray("client"));
        QCOMPARE(QByteArray(limitName(Limit::None)), QByteArray("none"));
    }

    // A throttle the link engaged stays the link's while the paced stream fits (no congestion, the
    // sender application-limited), until TCP measures the path with room again.
    void throttleCause()
    {
        State state;
        auto tbf = slowClient(lan(), 5800);
        tbf.sentKbps = 5600;
        Verdict v;
        for (int i = 0; i < 8; ++i) v = judge(state, tbf);
        QVERIFY(v.throttledByLink);
        QCOMPARE(classify(false, v, false), Limit::Link);
        auto fits = slowClient(lan(), 5800);
        fits.sentKbps = 5000;
        fits.congested = false;
        v = judge(state, fits);
        QVERIFY(v.throttledByLink);
        QVERIFY(!v.clientLimited);
        fits.capacityKbps = 38800; // the tbf went away
        for (int i = 0; i < 8; ++i) v = judge(state, fits);
        QCOMPARE(v.network, std::optional<bool>(false));
        QVERIFY(!v.throttledByLink);
        QCOMPARE(v.throttleCause, Cause::None);
        // The client's own pressure makes it the client's throttle.
        for (int i = 0; i < ClientLimitedAfter; ++i) v = judge(state, slowClient(lan()));
        QCOMPARE(v.throttleCause, Cause::Client);
        QVERIFY(v.clientLimited);
    }

    void rttInflation()
    {
        Socket s;
        s.minRttUs = 2000;
        s.rttUs = 21000;
        QVERIFY(!rttInflated(s)); // 19 ms over
        s.rttUs = 23000;
        QVERIFY(rttInflated(s));
        s.minRttUs = 40000;
        s.rttUs = 70000; // +30 ms but under 2x
        QVERIFY(!rttInflated(s));
        s.minRttUs = 0;
        QVERIFY(!rttInflated(s));
    }

    void percentiles()
    {
        QCOMPARE(percentile({5, 1, 3}, 0.5), 3u);
        QCOMPARE(percentile({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, 0.65), 7u);
        QCOMPARE(percentile({4}, 0.99), 4u);
        QCOMPARE(percentile({4, 9}, 0.0), 4u);
    }
};

QTEST_GUILESS_MAIN(LinkEvidenceTest)
#include "LinkEvidenceTest.moc"
