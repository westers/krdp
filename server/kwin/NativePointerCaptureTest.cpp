// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QTest>
#include <QQuickWindow>
#include "KWinPointerCapture.h"
#include <QDBusInterface>
#include <QDBusReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGui/qguiapplication_platform.h>
#include <qpa/qplatformwindow_p.h>
#include <QtWaylandClient/QWaylandClientExtension>
#include "qwayland-fake-input.h"
#include "qwayland-pointer-constraints-unstable-v1.h"
#include <linux/input-event-codes.h>
class MotionCounter : public QObject {
public:
    int moves = 0;
protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::MouseMove) ++moves;
        return false;
    }
};
class Input : public QWaylandClientExtensionTemplate<Input>, public QtWayland::org_kde_kwin_fake_input {
public: Input() : QWaylandClientExtensionTemplate<Input>(4) {}
};
class Constraints : public QWaylandClientExtensionTemplate<Constraints>, public QtWayland::zwp_pointer_constraints_v1 {
public: Constraints() : QWaylandClientExtensionTemplate<Constraints>(1) {}
};
class Lock : public QtWayland::zwp_locked_pointer_v1 {
public: bool active=false; explicit Lock(::zwp_locked_pointer_v1 *p) : zwp_locked_pointer_v1(p) {}
    ~Lock() { destroy(); }
protected: void zwp_locked_pointer_v1_locked() override { active=true; }
    void zwp_locked_pointer_v1_unlocked() override { active=false; }
};
class NativePointerCaptureTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void startupSnapshotFreeRecaptureAndMenuRequests() {
        QVERIFY(qEnvironmentVariableIsSet("FARSIDE_NATIVE_HOST_CAPTURE_TEST"));
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("wayland"));
        Input input; Constraints constraints; QQuickWindow game;
        QTRY_VERIFY(input.isActive() && constraints.isActive());
        input.authenticate(QStringLiteral("Farside test"),QStringLiteral("Isolated pointer capture gate"));
        game.setGeometry(100,100,900,600); game.show(); QTRY_VERIFY(game.isExposed());
        input.pointer_motion_absolute(wl_fixed_from_double(400),wl_fixed_from_double(300));
        input.button(BTN_LEFT,1); input.button(BTN_LEFT,0); QTRY_VERIFY(game.isActive());
        const auto app=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        const auto native=game.nativeInterface<QNativeInterface::Private::QWaylandWindow>();
        auto makeLock=[&] { return std::make_unique<Lock>(constraints.lock_pointer(native->surface(),app->pointer(),nullptr,
            QtWayland::zwp_pointer_constraints_v1::lifetime_persistent)); };
        auto lock=makeLock(); QTRY_VERIFY(lock->active);
        // Load after the application's lock already exists: no future event required.
        QDBusInterface plugins(QStringLiteral("org.kde.KWin"),QStringLiteral("/Plugins"),QStringLiteral("org.kde.KWin.Plugins"));
        QDBusReply<bool> loaded=plugins.call(QStringLiteral("LoadPlugin"),QStringLiteral("farside-pointer-capture"));
        QVERIFY2(loaded.isValid() && loaded.value(),qPrintable(loaded.error().message()));
        QDBusInterface bridge(QStringLiteral("org.kde.KWin"),QStringLiteral("/org/kde/KWin/FarsidePointerCapture"),QStringLiteral("org.farside.PointerCapture1"));
        auto call=[&](const QString &method,const QVariantList &args=QVariantList{}) {
            const QDBusReply<QString> result=bridge.callWithArgumentList(QDBus::Block,method,args);
            if (!result.isValid()) qWarning().noquote()<<result.error().message();
            return QJsonDocument::fromJson(result.value().toUtf8()).object();
        };
        auto state=call(QStringLiteral("Snapshot"));
        QVERIFY(state.value(QStringLiteral("requested")).toBool()); QVERIFY(state.value(QStringLiteral("locked")).toBool());
        const auto epoch=state.value(QStringLiteral("epoch")).toString(); QVERIFY(!epoch.isEmpty());
        state=call(QStringLiteral("SetPolicy"),{epoch,QStringLiteral("2"),false});
        QVERIFY(!state.contains(QStringLiteral("error"))); QTRY_VERIFY(!lock->active);
        state=call(QStringLiteral("Snapshot"));
        QVERIFY(state.value(QStringLiteral("requested")).toBool()); // App request survives our suspension.
        QVERIFY(!state.value(QStringLiteral("locked")).toBool()); QVERIFY(!state.value(QStringLiteral("permitted")).toBool());
        state=call(QStringLiteral("SetPolicy"),{epoch,QStringLiteral("2"),true});
        QVERIFY(!state.contains(QStringLiteral("error"))); QTRY_VERIFY(lock->active);
        lock.reset(); QTRY_VERIFY(!call(QStringLiteral("Snapshot")).value(QStringLiteral("requested")).toBool());
        lock=makeLock(); QTRY_VERIFY(lock->active);
        QVERIFY(call(QStringLiteral("Snapshot")).value(QStringLiteral("requested")).toBool());
        state=call(QStringLiteral("SetPolicy"),{epoch,QStringLiteral("1"),false});
        QCOMPARE(state.value(QStringLiteral("error")).toString(),QStringLiteral("stale-generation"));
        QVERIFY(lock->active);
        state=call(QStringLiteral("SetPolicy"),{QStringLiteral("wrong"),QStringLiteral("2"),false});
        QVERIFY(state.contains(QStringLiteral("error"))); QVERIFY(lock->active);
        call(QStringLiteral("SetPolicy"),{epoch,QStringLiteral("2"),false}); QTRY_VERIFY(!lock->active);
        state=call(QStringLiteral("Release"),{epoch,QStringLiteral("1")}); QVERIFY(state.contains(QStringLiteral("error")));
        call(QStringLiteral("Release"),{epoch,QStringLiteral("2")}); QTRY_VERIFY(lock->active);
    }
    void workerLeaseTracksGrantAndRestoresAfterDisconnect() {
        Input input; Constraints constraints; QQuickWindow game;
        QTRY_VERIFY(input.isActive() && constraints.isActive());
        input.authenticate(QStringLiteral("Farside test"), QStringLiteral("Isolated worker lease gate"));
        game.setGeometry(100,100,900,600); game.show(); QTRY_VERIFY(game.isExposed());
        input.pointer_motion_absolute(wl_fixed_from_double(400),wl_fixed_from_double(300));
        input.button(BTN_LEFT,1); input.button(BTN_LEFT,0); QTRY_VERIFY(game.isActive());
        const auto app=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        const auto native=game.nativeInterface<QNativeInterface::Private::QWaylandWindow>();
        Lock lock(constraints.lock_pointer(native->surface(), app->pointer(), nullptr,
            QtWayland::zwp_pointer_constraints_v1::lifetime_persistent));
        QTRY_VERIFY(lock.active);
        KRdp::KWinPointerCapture worker;
        QJsonObject latest;
        connect(&worker, &KRdp::KWinPointerCapture::stateChanged, this, [&](auto state) { latest=state; });
        worker.setControl({7,true});
        QTRY_VERIFY(latest.value(QStringLiteral("supported")).toBool() && latest.value(QStringLiteral("requested")).toBool());
        QCOMPARE(latest.value(QStringLiteral("generation")).toString(),QStringLiteral("7"));
        const auto epoch=latest.value(QStringLiteral("epoch"));
        auto request=[&](bool enabled,QString id) { worker.request({{QStringLiteral("v"),1},{QStringLiteral("epoch"),epoch},
            {QStringLiteral("generation"),QStringLiteral("7")},{QStringLiteral("enabled"),enabled},{QStringLiteral("id"),id}}); };
        request(false,QStringLiteral("free"));
        QTRY_COMPARE(latest.value(QStringLiteral("id")).toString(),QStringLiteral("free"));
        QTRY_VERIFY(!lock.active);
        QVERIFY(latest.value(QStringLiteral("requested")).toBool());
        request(true,QStringLiteral("capture"));
        QTRY_COMPARE(latest.value(QStringLiteral("id")).toString(),QStringLiteral("capture"));
        QTRY_VERIFY(lock.active);
        QVERIFY(latest.value(QStringLiteral("permitted")).toBool());
        request(false,QStringLiteral("free-again")); QTRY_VERIFY(!lock.active);
        worker.setControl({8,false});
        QTRY_VERIFY(lock.active);
        QTRY_COMPARE(latest.value(QStringLiteral("generation")).toString(),QStringLiteral("8"));
        QTRY_VERIFY(!latest.value(QStringLiteral("leased")).toBool());
        worker.stop();
        // A stalled/dead worker cannot leave the local desk permanently free.
        QDBusInterface bridge(QStringLiteral("org.kde.KWin"),QStringLiteral("/org/kde/KWin/FarsidePointerCapture"),QStringLiteral("org.farside.PointerCapture1"));
        QDBusReply<QString> held=bridge.call(QStringLiteral("SetPolicy"),epoch.toString(),QStringLiteral("9"),false);
        QVERIFY(held.isValid()); QTRY_VERIFY(!lock.active);
        QTRY_VERIFY_WITH_TIMEOUT(lock.active, 7000);
    }

    void releasedGameDoesNotReceiveMotionButDesktopAndMenuDo() {
        Input input; Constraints constraints; QQuickWindow game, desktop;
        MotionCounter gameEvents, desktopEvents;
        game.installEventFilter(&gameEvents); desktop.installEventFilter(&desktopEvents);
        QTRY_VERIFY(input.isActive() && constraints.isActive());
        input.authenticate(QStringLiteral("Farside test"), QStringLiteral("Isolated released motion gate"));
        desktop.showMaximized(); QTRY_VERIFY(desktop.isExposed());
        game.setGeometry(100,100,900,600); game.show(); QTRY_VERIFY(game.isExposed());
        auto move = [&](const QPoint &pos) { input.pointer_motion_absolute(wl_fixed_from_double(pos.x()), wl_fixed_from_double(pos.y())); };
        // Wayland clients cannot choose/report global window positions. These
        // points are inside the centered game and the surrounding desktop on
        // the fixture's explicitly sized 1600x800 output.
        const QPoint inside(400,300);
        move(inside); input.button(BTN_LEFT,1); input.button(BTN_LEFT,0); QTRY_VERIFY(game.isActive());
        const auto app=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        const auto native=game.nativeInterface<QNativeInterface::Private::QWaylandWindow>();
        auto lock=std::make_unique<Lock>(constraints.lock_pointer(native->surface(),app->pointer(),nullptr,
            QtWayland::zwp_pointer_constraints_v1::lifetime_persistent));
        QTRY_VERIFY(lock->active);
        QDBusInterface bridge(QStringLiteral("org.kde.KWin"),QStringLiteral("/org/kde/KWin/FarsidePointerCapture"),QStringLiteral("org.farside.PointerCapture1"));
        auto call=[&](const QString &method,const QVariantList &args=QVariantList{}) {
            const QDBusReply<QString> result=bridge.callWithArgumentList(QDBus::Block,method,args);
            return QJsonDocument::fromJson(result.value().toUtf8()).object();
        };
        const auto epoch=call(QStringLiteral("Snapshot")).value(QStringLiteral("epoch")).toString();
        QVERIFY(!epoch.isEmpty());
        QVERIFY(!call(QStringLiteral("SetPolicy"),{epoch,QStringLiteral("10"),false}).contains(QStringLiteral("error")));
        QTRY_VERIFY(!lock->active); QTest::qWait(100);
        const int releasedMoves=gameEvents.moves;
        move(inside+QPoint(30,0)); move(inside+QPoint(60,0)); QTest::qWait(100);
        QCOMPARE(gameEvents.moves,releasedMoves);
        // Hovering the other window must work even while the game is active.
        const QPoint outside(100,750);
        move(outside); QTest::qWait(100);
        const int desktopMoves=desktopEvents.moves;
        move(outside+QPoint(30,0)); QTRY_VERIFY(desktopEvents.moves>desktopMoves);
        QVERIFY(game.isActive());
        // Returning to the game cannot start leaking motion again.
        move(inside); QTest::qWait(100);
        const int reenteredMoves=gameEvents.moves;
        move(inside+QPoint(90,0)); QTest::qWait(100);
        QCOMPARE(gameEvents.moves,reenteredMoves);
        // Opening a real menu removes the app's lock request, so motion resumes.
        lock.reset(); QTRY_VERIFY(!call(QStringLiteral("Snapshot")).value(QStringLiteral("requested")).toBool());
        const int menuMoves=gameEvents.moves;
        move(inside+QPoint(120,0)); QTRY_VERIFY(gameEvents.moves>menuMoves);
        call(QStringLiteral("Release"),{epoch,QStringLiteral("10")});
    }

};
QTEST_MAIN(NativePointerCaptureTest)
#include "NativePointerCaptureTest.moc"
