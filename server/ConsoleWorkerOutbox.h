// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>
#include <optional>
#include <utility>

#include <QByteArray>
#include <QVector>

#include "ConsoleWorkerWire.h"

namespace KRdp
{
/**
 * AUD-FIX8: the order in which a capture worker speaks to its broker. The real worker
 * (consoleworker.cpp) sends every record that has an ordering rule through this class, and the
 * wire/ordering tests drive the same class, so a test fake cannot drift from the worker again.
 *
 * Wire v2 order, worker -> broker:
 *   1. Hello (authentication);
 *   2. EncoderCaps (the worker's own encoder probe), optional;
 *   3. Ready, once capture is confirmed;
 *   4. everything else.
 * The encoder opens before capture is confirmed, so an EncoderReport can happen before Ready.
 * It is held here and sent right after Ready, in order (the latest per event and codec; a
 * bounded number). An EncoderLoad before Ready is dropped: only the current CPU time matters and
 * the next one follows within EncoderLoadIntervalMs. The broker also tolerates reports that arrive
 * before Ready (an older worker), but a worker must not rely on it. EncoderStats (STATS-S6) is
 * never sent before Ready either: one made then is dropped (the broker fails a worker that sends
 * one before Ready). A Cursor (FIX-CURSOR) made before Ready is held, only the latest, and sent
 * right after Ready and the held reports: the client must see the shape the desktop starts with.
 */
class ConsoleWorkerOutbox
{
public:
    using Writer = std::function<void(const QByteArray &)>;
    static constexpr qsizetype MaxHeldReports = 16;

    explicit ConsoleWorkerOutbox(Writer writer)
        : m_writer(std::move(writer))
    {
    }

    /** A new connection: forget what an earlier one was told. */
    void reset()
    {
        m_helloSent = false;
        m_readySent = false;
        m_held.clear();
        m_heldCursor.reset();
    }

    void hello(const ConsoleWorkerWire::Hello &hello)
    {
        reset();
        m_helloSent = true;
        m_writer(ConsoleWorkerWire::frame(hello));
    }

    void caps(const ConsoleWorkerWire::EncoderCaps &caps)
    {
        if (m_helloSent && !m_readySent) {
            m_writer(ConsoleWorkerWire::frame(caps));
        }
    }

    /** Capture is confirmed: Ready once, then the reports held back until now. */
    void ready()
    {
        if (!m_helloSent || m_readySent) {
            return;
        }
        m_readySent = true;
        m_writer(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
        const auto held = std::exchange(m_held, {});
        for (const auto &report : held) {
            m_writer(ConsoleWorkerWire::frame(report));
        }
        if (const auto cursor = std::exchange(m_heldCursor, std::nullopt)) {
            m_writer(ConsoleWorkerWire::frame(*cursor));
        }
    }

    /** FIX-CURSOR: the desktop's cursor shape; before Ready only the latest is kept. */
    void cursor(const ConsoleWorkerWire::CursorShape &cursor)
    {
        if (!m_helloSent) {
            return;
        }
        if (m_readySent) {
            m_writer(ConsoleWorkerWire::frame(cursor));
        } else {
            m_heldCursor = cursor;
        }
    }

    void report(const ConsoleWorkerWire::EncoderReport &report)
    {
        if (!m_helloSent) {
            return;
        }
        if (m_readySent) {
            m_writer(ConsoleWorkerWire::frame(report));
            return;
        }
        // Only the latest state of each (event, codec) matters; keep the order of the rest.
        m_held.removeIf([&report](const ConsoleWorkerWire::EncoderReport &held) {
            return held.event == report.event && held.codec == report.codec;
        });
        if (m_held.size() >= MaxHeldReports) {
            m_held.removeFirst();
        }
        m_held.append(report);
    }

    void load(const ConsoleWorkerWire::EncoderLoad &load)
    {
        if (m_readySent) {
            m_writer(ConsoleWorkerWire::frame(load));
        }
    }

    /** STATS-S6: after Ready only; a report made before it is dropped (the next follows in 1 s). */
    void stats(const ConsoleWorkerWire::EncoderStats &stats)
    {
        if (m_readySent) {
            m_writer(ConsoleWorkerWire::frame(stats));
        }
    }

    /** Costs are current intervals, never held across initial capture. */
    void chromaTiming(const ConsoleWorkerWire::ChromaTiming &timing)
    {
        if (m_readySent) m_writer(ConsoleWorkerWire::frame(timing));
    }

    bool helloSent() const
    {
        return m_helloSent;
    }
    bool readySent() const
    {
        return m_readySent;
    }
    QVector<ConsoleWorkerWire::EncoderReport> held() const
    {
        return m_held;
    }

private:
    Writer m_writer;
    bool m_helloSent = false;
    bool m_readySent = false;
    QVector<ConsoleWorkerWire::EncoderReport> m_held;
    std::optional<ConsoleWorkerWire::CursorShape> m_heldCursor;
};
}
