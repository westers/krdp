// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>

#include <AbstractSession.h>

#include "ConsoleWorkerWire.h"

namespace KRdp
{
/**
 * An RDP-facing session backed by a greeter/desktop capture worker.
 *
 * It intentionally contains no Wayland or PipeWire setup: those privileges
 * remain in the worker's compositor session. The persistent console broker
 * submits encoded frames here and relays the normalized input callback over
 * its authenticated local socket.
 */
class ConsoleWorkerSession final : public AbstractSession
{
    Q_OBJECT

public:
    explicit ConsoleWorkerSession(std::function<void(const ConsoleWorkerWire::Input &)> sendInput, QObject *parent = nullptr);

    void start() override;
    void sendEvent(const std::shared_ptr<QEvent> &event) override;
    void setClipboardData(std::unique_ptr<QMimeData> data) override;
    void requestKeyFrame() override;

    void setWorkerActive(bool active);
    void submitFrame(const VideoFrame &frame);

Q_SIGNALS:
    void keyFrameRequested();

private:
    std::function<void(const ConsoleWorkerWire::Input &)> m_sendInput;
};
}
