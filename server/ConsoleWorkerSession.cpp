// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "RelativePointerEvent.h"
#include "ConsoleWorkerSession.h"

#include <QMimeData>

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
    if (event->type() == RelativePointerEvent::EventType) {
        const auto relative = std::static_pointer_cast<RelativePointerEvent>(event);
        result.type = ConsoleWorkerWire::Input::Type::RelativePointer;
        result.eventType = relative->action;
        result.position = relative->delta;
        result.button = relative->button;
        result.angleDelta = relative->angleDelta;
        return result;
    }
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
    // OPT-067: the broker never touches a host clipboard. The client's text goes to the worker as a
    // ClipboardSet record (the host controller gates it by policy and control); an empty or
    // non-text copy is an empty text, which clears the host clipboard.
    // OPT-072: a copy with text is the text (Clipboard requests an image only when there is none); one with only a
    // picture goes on as a ClipboardSet of kind Image.
    if (data && !data->hasText() && data->hasFormat(QStringLiteral("image/png"))) {
        Q_EMIT clientClipboardImage(data->data(QStringLiteral("image/png")));
        return;
    }
    Q_EMIT clientClipboardText(data && data->hasText() ? data->text() : QString());
}

void ConsoleWorkerSession::requestKeyFrame()
{
    Q_EMIT keyFrameRequested();
}

void ConsoleWorkerSession::setWorkerActive(bool active)
{
    setExternalStreamActive(active);
}

void ConsoleWorkerSession::reportEncoder(const ConsoleWorkerWire::EncoderReport &report)
{
    if (report.event == ConsoleWorkerWire::EncoderReport::Event::Unavailable) {
        Q_EMIT encoderUnavailable(report.codec);
    } else if (report.event == ConsoleWorkerWire::EncoderReport::Event::ChromaCapability) {
        Q_EMIT chromaCapabilityChanged(report.chromaCapable);
    } else {
        Q_EMIT encoderBackendReported(report.codec, report.hardware);
    }
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

void ConsoleWorkerSession::reportChromaTiming(const ChromaTimingReport &report)
{
    Q_EMIT chromaTimingReported(report);
}
}
