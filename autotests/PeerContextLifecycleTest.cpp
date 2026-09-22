#include <QTest>
#include "PeerContext_p.h"
#include <freerdp/peer.h>
#include <winpr/wtsapi.h>
namespace {
HANDLE opened = nullptr;
int closes = 0, recursiveFrees = 0;
}
extern "C" HANDLE WINAPI __wrap_WTSOpenServerA(LPSTR) { return opened; }
extern "C" VOID WINAPI __wrap_WTSCloseServer(HANDLE handle) { if (handle != opened) qFatal("Unexpected channel manager close"); ++closes; }
extern "C" void __wrap_freerdp_peer_context_free(freerdp_peer *) { ++recursiveFrees; }
class PeerContextLifecycleTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void init() { opened = nullptr; closes = recursiveFrees = 0; }
    void earlyFailureCleanup() {
        KRdp::PeerContext context{};
        freePeerContext(nullptr, nullptr);
        freePeerContext(nullptr, &context._p);
        context.virtualChannelManager = INVALID_HANDLE_VALUE;
        freePeerContext(nullptr, &context._p);
        QCOMPARE(closes, 0);
        QVERIFY(!context.virtualChannelManager);
    }
    void channelFailureNeverFreesOwnedPeer() {
        for (HANDLE failure : {HANDLE(nullptr), INVALID_HANDLE_VALUE}) {
            opened = failure;
            KRdp::PeerContext context{};
            freerdp_peer peer{}; peer.context = &context._p;
            QVERIFY(!newPeerContext(&peer, &context._p));
            QCOMPARE(recursiveFrees, 0);
            freePeerContext(&peer, &context._p);
            QCOMPARE(closes, 0);
        }
    }
    void successfulChannelClosesOnce() {
        int token = 0;
        opened = &token;
        KRdp::PeerContext context{};
        freerdp_peer peer{}; peer.context = &context._p;
        QVERIFY(newPeerContext(&peer, &context._p));
        freePeerContext(&peer, &context._p);
        freePeerContext(&peer, &context._p);
        QCOMPARE(closes, 1);
        QCOMPARE(recursiveFrees, 0);
    }
};
QTEST_GUILESS_MAIN(PeerContextLifecycleTest)
#include "PeerContextLifecycleTest.moc"
