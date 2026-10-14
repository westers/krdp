// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QObject>
#include <memory>
#include <linux/input.h>

namespace KRdp {
/** Read-only seat0 physical activity. No grabs, key recording, or input injection. */
class PhysicalInputWatcher final : public QObject {
    Q_OBJECT
public:
    explicit PhysicalInputWatcher(QObject *parent = nullptr);
    ~PhysicalInputWatcher() override;
    void start();
    static bool isActivity(const input_event &event) {
        return (event.type == EV_KEY && event.value > 0)
            || (event.type == EV_REL && event.value != 0)
            || event.type == EV_ABS;
    }
    /**
     * Whether an evdev node counts as the desk: a seat0 keyboard, mouse, touchpad or touchscreen that is a real
     * device. Software-created (uinput, /devices/virtual/input/) nodes never count, and KWin fake-input is not an
     * evdev device at all, so what the remote user injects can never be mistaken for the desk (OPT-071).
     * \a seat is the ID_SEAT property (empty = seat0, udev's default).
     */
    static bool isDeskDevice(const QString &devnode, const QString &syspath, const QString &seat, bool keyboard, bool mouse, bool touchpad, bool touchscreen)
    {
        if (!devnode.startsWith(QLatin1String("/dev/input/event"))) return false;
        if (syspath.contains(QLatin1String("/devices/virtual/input/"))) return false;
        if (!seat.isEmpty() && seat != QLatin1String("seat0")) return false;
        return keyboard || mouse || touchpad || touchscreen;
    }
Q_SIGNALS:
    void activity();
private:
    struct Private;
    std::unique_ptr<Private> d;
};
}
