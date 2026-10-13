// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <deque>
#include <utility>

#include <QtGlobal>

namespace KRdp::ClipboardLimits
{
/// Text carried between the broker and the worker: UTF-8 bytes (OPT-067).
constexpr qint64 MaxTextBytes = 1024 * 1024;
/// The same text as CF_UNICODETEXT on the cliprdr wire (UTF-16LE with its terminator, CRLF).
constexpr qint64 MaxWireBytes = 2 * MaxTextBytes + 2;
/// Client copies accepted per window, and the window.
constexpr int MaxSets = 20;
constexpr qint64 SetWindowMs = 10000;
/// Bytes of client copies accepted per minute.
constexpr qint64 MaxBytesPerMinute = 8 * 1024 * 1024;
constexpr qint64 ByteWindowMs = 60000;

/**
 * OPT-067: at most MaxSets accepted client copies per SetWindowMs and MaxBytesPerMinute per minute for
 * one connection. Time is injected (milliseconds on any monotonic clock) so it is unit-tested without
 * sleeping. A refused copy is not counted.
 */
class RateLimit
{
public:
    bool accept(qint64 nowMs, qint64 bytes)
    {
        while (!m_sets.empty() && nowMs - m_sets.front().first >= ByteWindowMs) m_sets.pop_front();
        qint64 inSetWindow = 0;
        qint64 bytesInWindow = 0;
        for (const auto &entry : m_sets) {
            bytesInWindow += entry.second;
            if (nowMs - entry.first < SetWindowMs) ++inSetWindow;
        }
        if (inSetWindow >= MaxSets || bytesInWindow + bytes > MaxBytesPerMinute) return false;
        m_sets.emplace_back(nowMs, bytes);
        return true;
    }

private:
    std::deque<std::pair<qint64, qint64>> m_sets; // (time, bytes)
};
}
