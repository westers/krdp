// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: GPL-3.0-or-later
#include "KWinPointerCapture.h"
#include "PointerCaptureProtocol.h"
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
namespace KRdp {
namespace {
constexpr auto Service = "org.kde.KWin";
constexpr auto Path = "/org/kde/KWin/FarsidePointerCapture";
constexpr auto Interface = "org.farside.PointerCapture1";
QDBusPendingCall call(const QString &method, const QVariantList &args = {}) {
    QDBusInterface bridge(QString::fromLatin1(Service), QString::fromLatin1(Path), QString::fromLatin1(Interface), QDBusConnection::sessionBus());
    bridge.setTimeout(2000);
    return bridge.asyncCallWithArgumentList(method, args);
}
}
KWinPointerCapture::KWinPointerCapture(QObject *parent) : QObject(parent) {
    QDBusConnection::sessionBus().connect(QString::fromLatin1(Service), QString::fromLatin1(Path), QString::fromLatin1(Interface),
        QStringLiteral("StateChanged"), this, SLOT(nativeState(QString)));
    auto *watch = new QDBusServiceWatcher(QString::fromLatin1(Service), QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watch, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] {
        ++m_serial; m_epoch.clear(); m_leaseEpoch.clear(); m_leaseGeneration.clear();
        unavailable(QStringLiteral("Compositor changed; capture was released.")); initialize();
    });
    QTimer::singleShot(0, this, [this] { initialize(); });
}
KWinPointerCapture::~KWinPointerCapture() { stop(); }
void KWinPointerCapture::initialize(bool load) {
    if (m_stopped) return;
    const auto serial = m_serial;
    auto *pending = new QDBusPendingCallWatcher(call(QStringLiteral("Snapshot")), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, serial, load](auto *p) {
        QDBusPendingReply<QString> result = *p; p->deleteLater();
        if (m_stopped || serial != m_serial) return;
        if (!result.isError()) { nativeState(result.value()); return; }
        if (!load) { unavailable(QStringLiteral("This compositor has no compatible Farside pointer capture bridge. Install the matching Farside server package.")); return; }
        QDBusInterface plugins(QString::fromLatin1(Service), QStringLiteral("/Plugins"), QStringLiteral("org.kde.KWin.Plugins"), QDBusConnection::sessionBus());
        plugins.setTimeout(2000);
        auto *loader = new QDBusPendingCallWatcher(plugins.asyncCall(QStringLiteral("LoadPlugin"), QStringLiteral("farside-pointer-capture")), this);
        connect(loader, &QDBusPendingCallWatcher::finished, this, [this, serial](auto *p) { p->deleteLater(); if (!m_stopped && serial == m_serial) initialize(false); });
    });
}
void KWinPointerCapture::setControl(ConsoleWorkerWire::ControlState control) {
    if (m_stopped || control == m_control) return;
    ++m_serial; m_faulted = false; release(); m_control = control;
    if (!m_releasing) initialize();
}
void KWinPointerCapture::stop() {
    if (m_stopped) return;
    m_stopped = true; ++m_serial; release();
}
void KWinPointerCapture::release(bool refresh) {
    if (m_leaseEpoch.isEmpty()) return;
    const auto epoch = std::exchange(m_leaseEpoch, {}), generation = std::exchange(m_leaseGeneration, {});
    m_releasing = true;
    auto *pending = new QDBusPendingCallWatcher(call(QStringLiteral("Release"), {epoch, generation}), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, refresh](auto *p) { p->deleteLater(); m_releasing = false; if (!m_stopped && refresh) initialize(); });
}
void KWinPointerCapture::request(const QJsonObject &request) {
    if (m_stopped || m_releasing || !m_control.active || !PointerCaptureProtocol::request(request)
        || request.value(QStringLiteral("generation")).toString() != QString::number(m_control.generation)
        || request.value(QStringLiteral("epoch")).toString() != m_epoch) {
        unavailable(QStringLiteral("Pointer capture request is stale or unavailable."), request.value(QStringLiteral("id")).toString()); return;
    }
    policy(request);
}
void KWinPointerCapture::policy(const QJsonObject &request) {
    // Save the lease before sending: disconnect during an in-flight call must also release it.
    m_leaseEpoch = m_epoch; m_leaseGeneration = QString::number(m_control.generation);
    if (!request.value(QStringLiteral("enabled")).toBool()) m_permitted = false;
    const auto serial = ++m_serial;
    auto *pending = new QDBusPendingCallWatcher(call(QStringLiteral("SetPolicy"),
        {m_leaseEpoch, m_leaseGeneration, request.value(QStringLiteral("enabled")).toBool()}), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, serial, request](auto *p) {
        QDBusPendingReply<QString> result = *p; p->deleteLater();
        if (m_stopped || serial != m_serial) return;
        auto state = result.isError() ? QJsonObject{} : PointerCaptureProtocol::decode(result.value().toUtf8());
        const auto id = request.value(QStringLiteral("id")).toString();
        if (state.value(QStringLiteral("epoch")).toString() != m_epoch || state.value(QStringLiteral("generation")).toString() != QString::number(m_control.generation)
            || !PointerCaptureProtocol::state(state)) {
            m_faulted = true; release(false); unavailable(QStringLiteral("The compositor could not confirm pointer capture."), id); return;
        }
        publish(state, id);
    });
}
void KWinPointerCapture::nativeState(const QString &encoded) {
    if (m_stopped || m_releasing) return;
    auto state = PointerCaptureProtocol::decode(encoded.toUtf8());
    if (m_faulted && state.value(QStringLiteral("leased")).toBool()) return;
    m_faulted = false;
    // An unleased native snapshot has no generation; the worker supplies its current grant.
    const auto leaseGeneration = state.value(QStringLiteral("generation")).toString();
    state.insert(QStringLiteral("generation"), QString::number(m_control.generation));
    if (!PointerCaptureProtocol::state(state)) { unavailable(QStringLiteral("Invalid compositor capture state.")); return; }
    const auto epoch = state.value(QStringLiteral("epoch")).toString();
    if (!m_epoch.isEmpty() && epoch != m_epoch) { ++m_serial; release(); unavailable(QStringLiteral("Compositor capture epoch changed.")); }
    m_epoch = epoch;
    if (leaseGeneration != QString::number(m_control.generation)) state.insert(QStringLiteral("permitted"), false);
    publish(state);
}
void KWinPointerCapture::publish(QJsonObject state, const QString &id) {
    m_permitted = state.value(QStringLiteral("permitted")).toBool();
    state.insert(QStringLiteral("type"), QStringLiteral("pointer-capture-state"));
    state.insert(QStringLiteral("generation"), QString::number(m_control.generation));
    if (!id.isEmpty()) state.insert(QStringLiteral("id"), id);
    Q_EMIT stateChanged(state);
}
void KWinPointerCapture::unavailable(const QString &reason, const QString &id) {
    publish({{QStringLiteral("v"), 1}, {QStringLiteral("supported"), false}, {QStringLiteral("error"), reason.left(1024)}}, id);
}
}
