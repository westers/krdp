// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cerrno>
#include <poll.h>
#include <unistd.h>

#include <QByteArray>
#include <QElapsedTimer>

namespace KRdp::SelectionReader
{
enum class Status : quint8 {
    Ok,
    NoSelection, ///< nothing is on the clipboard
    NoFormat, ///< the clipboard has none of the formats asked for
    TooLarge, ///< the source sent more than the cap; the read was abandoned
    Timeout, ///< the source stopped sending, or took too long overall
    Failed,
    Unavailable ///< no data-control protocol on this compositor / no connection: the caller falls back
};

inline const char *statusName(Status status)
{
    switch (status) {
    case Status::Ok: return "ok";
    case Status::NoSelection: return "no-selection";
    case Status::NoFormat: return "no-format";
    case Status::TooLarge: return "oversize";
    case Status::Timeout: return "timeout";
    case Status::Failed: return "failed";
    case Status::Unavailable: return "unavailable";
    }
    return "unknown";
}

struct FdRead {
    Status status = Status::Failed;
    QByteArray data; ///< the whole payload for Ok; empty otherwise (an abandoned read keeps nothing)
    qint64 bytes = 0; ///< bytes received when the read ended (for TooLarge: just over the cap)
};

/**
 * OPT-072: reads \a fd (the read end of the pipe a data-control source writes into) until EOF, but never more than \a capBytes.
 * KSystemClipboard::mimeData()->data() does the same loop with no limit, so a huge or endless source would be read into memory
 * whole; this one gives up as soon as it has capBytes + 1 bytes (the source then sees a closed pipe when the caller closes
 * its end). \a idleMs bounds the wait for any one chunk, \a totalMs the whole read. Pure POSIX: unit-tested with pipes.
 */
inline FdRead readCapped(int fd, qint64 capBytes, int idleMs, int totalMs)
{
    FdRead result;
    QByteArray data;
    QElapsedTimer clock;
    clock.start();
    char buffer[64 * 1024];
    for (;;) {
        const qint64 left = totalMs - clock.elapsed();
        if (left <= 0) {
            result.status = Status::Timeout;
            result.bytes = data.size();
            return result;
        }
        pollfd pfd{fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, int(qMin<qint64>(left, idleMs)));
        if (ready < 0) {
            if (errno == EINTR) continue;
            result.bytes = data.size();
            return result; // Failed
        }
        if (ready == 0) {
            result.status = Status::Timeout;
            result.bytes = data.size();
            return result;
        }
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            result.bytes = data.size();
            return result; // Failed
        }
        if (n == 0) {
            result.status = Status::Ok;
            result.bytes = data.size();
            result.data = std::move(data);
            return result;
        }
        data.append(buffer, qsizetype(n));
        if (data.size() > capBytes) {
            result.status = Status::TooLarge;
            result.bytes = data.size();
            return result;
        }
    }
}
}
