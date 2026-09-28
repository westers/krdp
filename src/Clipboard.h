// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QMimeData>
#include <QObject>

#include <freerdp/freerdp.h>
#include <freerdp/server/cliprdr.h>

#include <mutex>

#include "krdp_export.h"

namespace KRdp
{

class RdpConnection;

class KRDP_EXPORT Clipboard : public QObject
{
    Q_OBJECT

public:
    explicit Clipboard(RdpConnection *session);
    ~Clipboard() override;

    bool initialize();
    void close();

    bool enabled();

    void setServerData(const QMimeData *data);

    Q_SIGNAL void clientDataChanged();
    std::unique_ptr<QMimeData> getClipboard() const;

    /**
     * Point \a context's client-PDU callbacks at \a clipboard. The callbacks
     * run on FreeRDP's cliprdr thread; they copy the PDU and hand it to the
     * main thread without waiting (AUD-P5). Public so a test can drive them
     * without a peer.
     */
    static void installClientCallbacks(CliprdrServerContext *context, Clipboard *clipboard);

    /**
     * AUD-FIX5: the client has a picture (VideoStream::graphicsDelivered()). Until then the
     * server sends the client no clipboard request and no clipboard announcement, only the
     * protocol's format-list responses: a stock client (wlfreerdp3 3.22 on an unlocked KDE
     * desktop) that blocks its connection on the local clipboard read a data request starts
     * must not be kept from its graphics setup by it. DeliveredFallback after the first client
     * format list the clipboard goes ahead anyway (a client that never acknowledges a frame).
     */
    void setGraphicsDelivered();

    /// How long the client's format lists are collected before one data request goes out.
    static constexpr int FormatListSettleMs = 250;
    /// A data request not answered for this long no longer holds back the next one.
    static constexpr int ResponseTimeoutMs = 5000;
    /// See setGraphicsDelivered().
    static constexpr int DeliveredFallbackMs = 15000;

private:
    void sendServerData();

    class Private;
    const std::unique_ptr<Private> d;
};
}
