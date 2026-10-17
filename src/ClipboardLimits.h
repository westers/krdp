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
/// OPT-072: an image carried between the broker and the worker, and as the "PNG" cliprdr format: encoded bytes.
constexpr qint64 MaxImageBytes = 16 * 1024 * 1024;
/// The most a raw CF_DIB / CF_DIBV5 response is on the cliprdr wire (either direction).
constexpr qint64 MaxImageWireBytes = 64 * 1024 * 1024;
/// OPT-077: HTML carried between the broker and the worker, and as the "HTML Format" cliprdr format (the document, UTF-8).
constexpr qint64 MaxHtmlBytes = 8 * 1024 * 1024;
/// The most "HTML Format" can be on the cliprdr wire: the document plus its header and terminator.
constexpr qint64 MaxHtmlWireBytes = MaxHtmlBytes + 4096;
/// OPT-078: the most the worker reads of a `text/uri-list` (a copy of 100 000 files is a few MB of paths).
constexpr qint64 MaxUriListBytes = 16 * 1024 * 1024;
/// Client copies accepted per window, and the window.
constexpr int MaxSets = 20;
constexpr qint64 SetWindowMs = 10000;
/// Bytes of client copies accepted per minute, text and images together (an image counts its encoded bytes; OPT-072
/// raised this from 8 MiB so one 16 MiB image can pass; the copies-per-window limit is unchanged).
constexpr qint64 MaxBytesPerMinute = 64 * 1024 * 1024;
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
