// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleWorkerSession.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

namespace KRdp
{
namespace
{
std::optional<ConsoleWorkerWire::Input> inputFor(const std::shared_ptr<QEvent> &event)
{
    ConsoleWorkerWire::Input result;
    result.eventType = event->type();
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        const auto mouse = std::static_pointer_cast<QMouseEvent>(event);
        result.type = ConsoleWorkerWire::Input::Type::Mouse;
        result.position = mouse->position();
        result.button = mouse->button();
        result.buttons = mouse->buttons();
        return result;
    }
    case QEvent::Wheel: {
        const auto wheel = std::static_pointer_cast<QWheelEvent>(event);
        result.type = ConsoleWorkerWire::Input::Type::Wheel;
        result.position = wheel->position();
        result.buttons = wheel->buttons();
        result.angleDelta = wheel->angleDelta();
        return result;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        const auto key = std::static_pointer_cast<QKeyEvent>(event);
        result.type = ConsoleWorkerWire::Input::Type::Key;
        result.nativeScanCode = key->nativeScanCode();
        result.nativeVirtualKey = key->nativeVirtualKey();
        result.text = key->text();
        return result;
    }
    default:
        return std::nullopt;
    }
}
}

ConsoleWorkerSession::ConsoleWorkerSession(std::function<void(const ConsoleWorkerWire::Input &)> sendInput, QObject *parent)
    : AbstractSession()
    , m_sendInput(std::move(sendInput))
{
    setParent(parent);
}

void ConsoleWorkerSession::start()
{
    // There is no local PipeWire producer. The broker marks this active only
    // after the worker's authenticated Ready record.
    setStarted(true);
}

void ConsoleWorkerSession::sendEvent(const std::shared_ptr<QEvent> &event)
{
    if (!streamActive() || !m_sendInput) {
        return;
    }
    if (const auto input = inputFor(event)) {
        m_sendInput(*input);
    }
}

void ConsoleWorkerSession::setClipboardData(std::unique_ptr<QMimeData> data)
{
    Q_UNUSED(data)
    // Clipboard worker IPC is deliberately a later record type; do not give a
    // system broker direct access to the greeter clipboard in the meantime.
}

void ConsoleWorkerSession::requestKeyFrame()
{
    Q_EMIT keyFrameRequested();
}

void ConsoleWorkerSession::setWorkerActive(bool active)
{
    setExternalStreamActive(active);
}

void ConsoleWorkerSession::submitFrame(const VideoFrame &frame)
{
    if (!streamActive()) {
        return;
    }
    setSize(frame.size);
    setLogicalSize(frame.size);
    Q_EMIT frameReceived(frame);
}
}
