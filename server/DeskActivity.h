// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <optional>

#include <QHash>
#include <QJsonObject>
#include <QPoint>
#include <QString>
#include <QVector>

#include "SurfaceLayout.h" // VideoMonitor

/**
 * OPT-071: telling a remote Console client that someone is at the physical desk, and where the host's pointer is.
 * Everything here is pure (a caller-supplied monotonic millisecond clock, no I/O) so it is unit-tested without a
 * daemon. Design: docs/superpowers/specs/2026-10-14-desk-activity-signal-design.md.
 *
 * Privacy: nothing here ever sees a key code. PhysicalInputWatcher reduces an evdev read to "something happened"
 * before it calls note(), and the records built here carry exactly the keys documented below.
 */
namespace KRdp::DeskActivity
{
/// At most this many `desk-activity` active records per second.
constexpr int MaxRateHz = 4;
constexpr qint64 MinIntervalMs = 1000 / MaxRateHz;
/// Quiet for this long: one `active:false` record. It is also how long the host pointer is relayed after the last event.
constexpr qint64 IdleMs = 10000;
/// At most this many `host-pointer` records per second (worker side and again at the broker).
constexpr int MaxPointerRateHz = 20;
constexpr qint64 MinPointerIntervalMs = 1000 / MaxPointerRateHz;
/// A hide of the host pointer waits this long, so crossing from one captured output to the next does not blink.
constexpr qint64 PointerHideDelayMs = 100;
/// The largest desktop coordinate the wire carries.
constexpr int MaxCoordinate = 65535;

/** Where the desk's session is, from the seat sessions the broker watches. The client words these neutrally. */
enum class Context {
    Session,
    Greeter,
    Locked,
};

inline QString contextName(Context context)
{
    switch (context) {
    case Context::Greeter:
        return QStringLiteral("greeter");
    case Context::Locked:
        return QStringLiteral("locked");
    case Context::Session:
        break;
    }
    return QStringLiteral("session");
}

/**
 * The `desk-activity` record. Exactly these keys; no key code, key name, device, button or coordinate (a test pins
 * the key set).
 */
inline QJsonObject record(bool active, quint32 sequence, Context context)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("desk-activity")},
                       {QStringLiteral("v"), 1},
                       {QStringLiteral("active"), active},
                       {QStringLiteral("seq"), qint64(sequence)},
                       {QStringLiteral("context"), contextName(context)}};
}

/** The `host-pointer` record: desktop coordinates in the client's RDP space. */
inline QJsonObject pointerRecord(QPoint position, bool visible)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("host-pointer")},
                       {QStringLiteral("v"), 1},
                       {QStringLiteral("x"), position.x()},
                       {QStringLiteral("y"), position.y()},
                       {QStringLiteral("visible"), visible}};
}

/** The `desk-activity-subscribe` request a client sends. Strict: exactly type, v:1, boolean enabled, optional boolean pointer. */
struct Subscription {
    bool enabled = false;
    bool pointer = false;
    bool operator==(const Subscription &) const = default;
};

inline std::optional<Subscription> parseSubscribe(const QJsonObject &request)
{
    static const QStringList allowed{QStringLiteral("type"), QStringLiteral("v"), QStringLiteral("enabled"), QStringLiteral("pointer")};
    for (auto it = request.begin(); it != request.end(); ++it) {
        if (!allowed.contains(it.key())) {
            return std::nullopt;
        }
    }
    if (request.value(QStringLiteral("type")).toString() != QLatin1String("desk-activity-subscribe") || request.value(QStringLiteral("v")) != 1
        || !request.value(QStringLiteral("enabled")).isBool()) {
        return std::nullopt;
    }
    Subscription subscription;
    subscription.enabled = request.value(QStringLiteral("enabled")).toBool();
    if (request.contains(QStringLiteral("pointer"))) {
        if (!request.value(QStringLiteral("pointer")).isBool()) {
            return std::nullopt;
        }
        subscription.pointer = subscription.enabled && request.value(QStringLiteral("pointer")).toBool();
    }
    return subscription;
}

/**
 * The state machine behind the `desk-activity` records: note() every time the desk moved (as often as the device
 * reports, hundreds per second), take() what is due.
 *
 *   active record: the first event goes out at once; while events continue one trailing record per MinIntervalMs;
 *   idle record:   once, IdleMs after the last event.
 */
class Tracker
{
public:
    /** The desk did something at \a nowMs (a monotonic clock). */
    void note(qint64 nowMs)
    {
        m_lastEvent = nowMs;
        m_pending = true;
    }

    /** True while the last event is less than IdleMs old: the desk counts as in use. */
    bool live(qint64 nowMs) const
    {
        return m_lastEvent && nowMs - *m_lastEvent < IdleMs;
    }

    /** A record due at \a nowMs: true = send `active:true`, false = send `active:false`; std::nullopt = nothing. Counts as sent. */
    std::optional<bool> take(qint64 nowMs)
    {
        if (m_pending && (!m_lastSent || nowMs < *m_lastSent || nowMs - *m_lastSent >= MinIntervalMs)) {
            m_pending = false;
            m_lastSent = nowMs;
            m_idleSent = false;
            return true;
        }
        if (!m_pending && !m_idleSent && m_lastEvent && nowMs - *m_lastEvent >= IdleMs) {
            m_idleSent = true;
            return false;
        }
        return std::nullopt;
    }

    /** Milliseconds until take() may return something; 0 = now, -1 = nothing is waiting. */
    qint64 dueIn(qint64 nowMs) const
    {
        if (m_pending) {
            if (!m_lastSent || nowMs < *m_lastSent) {
                return 0;
            }
            return std::max<qint64>(0, *m_lastSent + MinIntervalMs - nowMs);
        }
        if (!m_idleSent && m_lastEvent) {
            return std::max<qint64>(0, *m_lastEvent + IdleMs - nowMs);
        }
        return -1;
    }

    /** True when the last thing a subscriber was told is `active:true` (so a new subscriber should hear it too). */
    bool announcedActive() const { return m_lastSent && !m_idleSent; }

    void reset() { *this = Tracker(); }

private:
    std::optional<qint64> m_lastEvent;
    std::optional<qint64> m_lastSent;
    bool m_pending = false;
    bool m_idleSent = true;
};

/** A source stream's cursor position in desktop (wire) coordinates, or std::nullopt when it cannot be placed. */
inline std::optional<QPoint> toDesktop(bool multiOutput, const QVector<VideoMonitor> &atlas, quint64 source, QPoint streamPosition)
{
    QPoint origin;
    if (multiOutput) {
        // Per-output captures are sources 1..N, in atlas order (source 0 is the workspace capture).
        if (source == 0 || source > quint64(atlas.size())) {
            return std::nullopt;
        }
        origin = atlas.at(qsizetype(source - 1)).geometry.topLeft();
    }
    const qint64 x = qint64(origin.x()) + streamPosition.x();
    const qint64 y = qint64(origin.y()) + streamPosition.y();
    return QPoint(int(std::clamp<qint64>(x, 0, MaxCoordinate)), int(std::clamp<qint64>(y, 0, MaxCoordinate)));
}

/**
 * Where the host's pointer is, from the capture sources' cursor metadata (the worker). Like CursorTracker each source
 * votes on visibility (KWin reports the cursor on the output it is over, absent on the others); the latest visible
 * report gives the position. Sends only on change, at most one per MinPointerIntervalMs, and holds a hide for
 * PointerHideDelayMs.
 */
class HostPointerTracker
{
public:
    struct Pointer {
        QPoint position;
        bool visible = false;
        bool operator==(const Pointer &) const = default;
    };

    void report(quint64 source, bool visible, QPoint desktopPosition, qint64 nowMs)
    {
        Source &entry = m_sources[source];
        entry.visible = visible;
        if (visible) {
            entry.position = desktopPosition;
            entry.order = ++m_order;
        }
        settle(nowMs);
    }

    void forget(quint64 source, qint64 nowMs)
    {
        if (m_sources.remove(source)) {
            settle(nowMs);
        }
    }

    void reset(qint64 nowMs)
    {
        m_sources.clear();
        settle(nowMs);
    }

    /** A new receiver: nothing has been sent to it yet. */
    void resend()
    {
        m_sent.reset();
        m_lastSentMs.reset();
    }

    std::optional<Pointer> take(qint64 nowMs)
    {
        if (dueIn(nowMs) != 0) {
            return std::nullopt;
        }
        m_sent = desired();
        m_lastSentMs = nowMs;
        return m_sent;
    }

    /** Milliseconds until take() returns something; 0 = now, -1 = nothing to send. */
    qint64 dueIn(qint64 nowMs) const
    {
        const auto wanted = desired();
        if (!wanted || (m_sent && *m_sent == *wanted)) {
            return -1;
        }
        qint64 wait = 0;
        if (m_lastSentMs && nowMs >= *m_lastSentMs) {
            wait = std::max<qint64>(wait, *m_lastSentMs + MinPointerIntervalMs - nowMs);
        }
        if (!wanted->visible && m_hiddenSinceMs >= 0) {
            wait = std::max<qint64>(wait, m_hiddenSinceMs + PointerHideDelayMs - nowMs);
        }
        return wait;
    }

    std::optional<Pointer> desired() const
    {
        if (m_sources.isEmpty()) {
            return std::nullopt;
        }
        const Source *best = nullptr;
        for (const auto &entry : m_sources) {
            if (entry.visible && (!best || entry.order > best->order)) {
                best = &entry;
            }
        }
        if (best) {
            return Pointer{best->position, true};
        }
        // Nobody sees it: hidden, at the last place it was.
        QPoint last = m_lastVisible;
        return Pointer{last, false};
    }

private:
    struct Source {
        bool visible = false;
        QPoint position;
        quint64 order = 0;
    };

    void settle(qint64 nowMs)
    {
        const auto wanted = desired();
        if (wanted && wanted->visible) {
            m_lastVisible = wanted->position;
            m_hiddenSinceMs = -1;
        } else if (m_hiddenSinceMs < 0) {
            m_hiddenSinceMs = nowMs;
        }
    }

    QHash<quint64, Source> m_sources;
    quint64 m_order = 0;
    QPoint m_lastVisible;
    std::optional<Pointer> m_sent;
    std::optional<qint64> m_lastSentMs;
    qint64 m_hiddenSinceMs = -1;
};
}
