// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX13: telling a slow link from a slow client by the socket (src/LinkEvidence.h).

#include "LinkEvidence.h"

#include <QTest>

using namespace KRdp::LinkEvidence;

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
        auto v = judge(state, slowClient(lan()));
        QCOMPARE(v.network, std::optional<bool>(false));
        QVERIFY(v.headroom);
        QVERIFY(!v.clientLimited);
        v = judge(state, slowClient(lan()));
        QVERIFY(v.clientLimited);
        QVERIFY(v.clientLimitedChanged);
        QVERIFY2(v.why.contains(u"38800"), qPrintable(v.why));
        QCOMPARE(classify(false, true, v, false), Limit::Client);
        // Without a capacity estimate the application-limited sender is headroom enough.
        v = judge(state, slowClient(lan(), std::nullopt));
        QVERIFY(v.clientLimited);
        QVERIFY(!v.clientLimitedChanged);
    }

    // A tbf: every socket signal on its own is network evidence and ends client-limited at once.
    void networkEvidence_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("retransmits") << 0;
        QTest::newRow("tcp rtt inflated") << 1;
        QTest::newRow("capacity under the threshold") << 2;
        QTest::newRow("send queue growing") << 3;
    }
    void networkEvidence()
    {
        QFETCH(int, kind);
        State state;
        judge(state, slowClient(lan()));
        QVERIFY(judge(state, slowClient(lan())).clientLimited);
        auto socket = lan();
        auto in = slowClient(socket);
        switch (kind) {
        case 0:
            socket.totalRetransmits = 5;
            break;
        case 1:
            socket.rttUs = 261000;
            break;
        case 2:
            in.capacityKbps = 5800;
            break;
        case 3:
            socket.queuedBytes = 100 * 1024;
            in.socket = socket;
            judge(state, in); // backed up once: not yet evidence
            socket.queuedBytes = 180 * 1024;
            break;
        }
        in.socket = socket;
        const auto v = judge(state, in);
        QCOMPARE(v.network, std::optional<bool>(true));
        QVERIFY(!v.headroom);
        QVERIFY(!v.clientLimited);
        QVERIFY(!v.why.isEmpty());
        QCOMPARE(classify(false, true, v, false), Limit::Link);
    }

    // Small signals are not evidence: one retransmit, a Wi-Fi-sized RTT wobble, a keyframe in the queue.
    void noiseIsNotEvidence()
    {
        State state;
        auto socket = lan();
        judge(state, slowClient(socket));
        socket.totalRetransmits = 1;
        socket.rttUs = 15000; // 7.5x a 2 ms minimum, but only 13 ms over it
        socket.queuedBytes = 200 * 1024; // backed up once, not twice
        const auto v = judge(state, slowClient(socket));
        QCOMPARE(v.network, std::optional<bool>(false));
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
        socket.busyUs = 2500000;
        socket.rwndLimitedUs = 1200000; // 80 % of the busy time
        socket.queuedBytes = 300 * 1024;
        auto v = judge(state, slowClient(socket, 4000));
        QVERIFY(v.rwndLimited);
        QCOMPARE(v.network, std::optional<bool>(false));
        QVERIFY(v.headroom);
        // Retransmits still count whoever limits the window.
        socket.busyUs = 4000000;
        socket.rwndLimitedUs = 2400000;
        socket.totalRetransmits = 4;
        v = judge(state, slowClient(socket, 4000));
        QCOMPARE(v.network, std::optional<bool>(true));
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
            QVERIFY(!v.clientLimited);
            QCOMPARE(classify(false, true, v, false), Limit::None);
        }
    }

    // Client-limited needs the client to be behind (or paced); it ends after as many calm intervals.
    void hysteresisAndPriority()
    {
        State state;
        judge(state, slowClient(lan()));
        auto v = judge(state, slowClient(lan()));
        QVERIFY(v.clientLimited);
        auto calm = slowClient(lan());
        calm.congested = false;
        calm.throttled = false;
        v = judge(state, calm);
        QVERIFY(v.clientLimited); // one calm interval is not enough
        v = judge(state, calm);
        QVERIFY(!v.clientLimited);
        QVERIFY(v.clientLimitedChanged);
        QCOMPARE(classify(false, false, v, false), Limit::None);
        QCOMPARE(classify(true, false, v, true), Limit::Link); // a slow link first
        QCOMPARE(classify(false, false, v, true), Limit::Encoder);
        QCOMPARE(QByteArray(limitName(Limit::Client)), QByteArray("client"));
        QCOMPARE(QByteArray(limitName(Limit::None)), QByteArray("none"));
    }

    // A throttle the link engaged stays the link's while the paced stream fits (no congestion, the
    // sender application-limited), until TCP measures the path above the slow threshold again.
    void throttleCause()
    {
        State state;
        auto socket = lan();
        auto tbf = slowClient(socket, 5800);
        socket.totalRetransmits = 6;
        socket.rttUs = 261000;
        tbf.socket = socket;
        auto v = judge(state, tbf);
        QVERIFY(v.throttledByLink);
        QCOMPARE(classify(false, true, v, false), Limit::Link);
        auto fits = slowClient(lan(), std::nullopt);
        fits.congested = false;
        auto calm = lan();
        calm.totalRetransmits = 6;
        fits.socket = calm;
        for (int i = 0; i < 4; ++i) {
            v = judge(state, fits);
            QCOMPARE(v.network, std::optional<bool>(false));
            QVERIFY(v.headroom); // application-limited
            QVERIFY(v.throttledByLink);
            QVERIFY(!v.clientLimited);
            QCOMPARE(classify(false, false, v, false), Limit::Link);
        }
        fits.capacityKbps = 38800; // the tbf went away
        v = judge(state, fits);
        QVERIFY(!v.throttledByLink);
        QCOMPARE(v.throttleCause, Cause::None);
        // The client's own pressure makes it the client's throttle.
        judge(state, slowClient(calm));
        v = judge(state, slowClient(calm));
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
};

QTEST_GUILESS_MAIN(LinkEvidenceTest)
#include "LinkEvidenceTest.moc"
