// SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleix.pol_gonzalez@mercedes-benz.com>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <type_traits>

#include <QDBusPendingCallWatcher>
#include <QPoint>
#include <QPointer>
#include <QRect>

#include "AbstractSession.h"
#include "krdp_export.h"

class QScreen;

namespace KRdp
{

struct VideoFrame;
class Server;

/**
 * An implementation of the Plasma screencasting wayland protocol.
 */
class KRDP_EXPORT PlasmaScreencastV1Session : public AbstractSession
{
    Q_OBJECT

public:
    PlasmaScreencastV1Session();
    ~PlasmaScreencastV1Session() override;

    void start() override;
    void refreshDisplayConfiguration() override;
    void requestKeyFrame() override;

    void sendEvent(const std::shared_ptr<QEvent> &event) override;
    void sendGlobalEvent(const std::shared_ptr<QEvent> &event) override;
    void setClipboardData(std::unique_ptr<QMimeData> data) override;

    QRect outputGeometry() const override;
    bool outputGeometryResolved() const override;

private:
    void injectNonMotionEvent(const std::shared_ptr<QEvent> &event);
    void scheduleStreamRecovery(int attempt, int delayMs);
    void attemptStreamRecovery(int attempt);
    bool setupScreencastRequest(bool allowWorkspaceFallback = true);
    void onScreencastCreated(uint nodeId);
    void restartEncodedStream(uint nodeId);
    void attachEncodedStream(uint nodeId, bool streamWasActive);
    void onPacketReceived(const PipeWireEncodedStream::Packet &data);
    void watchForVirtualScreen();
    bool adoptVirtualScreen(QScreen *screen);
    void updateVirtualGeometry(const QRect &geometry, bool adopted = false);

    class Private;
    const std::unique_ptr<Private> d;
};

}
