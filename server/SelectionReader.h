// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>

#include <QStringList>

#include "SelectionReaderFd.h"

namespace KRdp::SelectionReader
{
struct Result {
    Status status = Status::Unavailable;
    QByteArray data;
    QString mime; ///< the format that was read (or, for TooLarge, the one abandoned)
    qint64 bytes = 0;
};

/**
 * OPT-072: reads the current clipboard selection through ext_data_control_v1 on a connection of its own, in the first of
 * \a mimeOrder the selection offers, giving up at capForMime(mime) bytes. The data stays out of KSystemClipboard on purpose,
 * because that class reads an offer without any limit. Synchronous (a paste is rare). Unavailable when the compositor has
 * no ext_data_control_manager_v1, there is no Wayland display to connect to or the connection is single-use (WAYLAND_SOCKET):
 * the caller then keeps its older, unbounded path.
 */
Result readSelection(const QStringList &mimeOrder, const std::function<qint64(const QString &mime)> &capForMime, int idleMs = 1500, int totalMs = 5000);
}
