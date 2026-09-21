// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <QMap>
#include "ConsoleWorkerWire.h"

namespace KRdp
{
/** Track only input actually forwarded to the compositor, never viewer input. */
class ConsoleInputState
{
public:
    using Input = ConsoleWorkerWire::Input;

    void record(const Input &input)
    {
        if (input.type == Input::Type::Key) {
            // A shifted key can have a different keysym at release; hardware
            // scan code is its identity. Keep a separate fallback namespace.
            const quint64 id = input.nativeScanCode ? input.nativeScanCode : (quint64(1) << 32) | input.nativeVirtualKey;
            if (input.eventType == QEvent::KeyPress) {
                m_keys.insert(id, input); // Repeats are still just one held key.
            } else if (input.eventType == QEvent::KeyRelease) {
                m_keys.remove(id);
            }
            return;
        }
        m_position = input.position;
        if (input.type == Input::Type::Mouse && input.button != Qt::NoButton) {
            if (input.eventType == QEvent::MouseButtonPress) {
                m_buttons |= input.button;
            } else if (input.eventType == QEvent::MouseButtonRelease) {
                m_buttons &= ~Qt::MouseButtons(input.button);
            }
        }
    }

    QVector<Input> releaseAll()
    {
        QVector<Input> releases;
        for (auto input : std::as_const(m_keys)) {
            input.eventType = QEvent::KeyRelease;
            input.text.clear();
            releases.append(input);
        }
        m_keys.clear();
        for (quint32 bit = 1; bit <= quint32(Qt::MaxMouseButton); bit <<= 1) {
            const auto button = Qt::MouseButton(bit);
            if (!m_buttons.testFlag(button)) {
                continue;
            }
            m_buttons &= ~Qt::MouseButtons(button);
            Input input;
            input.type = Input::Type::Mouse;
            input.eventType = QEvent::MouseButtonRelease;
            input.position = m_position;
            input.button = button;
            input.buttons = m_buttons;
            releases.append(input);
        }
        return releases;
    }

private:
    QMap<quint64, Input> m_keys;
    Qt::MouseButtons m_buttons;
    QPointF m_position;
};
}
