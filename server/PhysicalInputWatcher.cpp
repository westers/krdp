// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "PhysicalInputWatcher.h"
#include <QSocketNotifier>
#include <QFile>
#include <QMap>
#include <QDebug>
#include <libudev.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace KRdp {
struct PhysicalInputWatcher::Private {
    PhysicalInputWatcher *q;
    udev *context = udev_new();
    udev_monitor *monitor = nullptr;
    QMap<QString, QSocketNotifier *> devices;
    explicit Private(PhysicalInputWatcher *owner) : q(owner) {}
    void remove(const QString &path) {
        auto it = devices.find(path);
        if (it == devices.end()) return;
        auto notifier = it.value();
        devices.erase(it);
        notifier->setEnabled(false);
        close(notifier->socket());
        notifier->deleteLater();
    }
    void add(udev_device *device) {
        const char *node = udev_device_get_devnode(device);
        const char *sys = udev_device_get_syspath(device);
        if (!node || !sys || !QString::fromLocal8Bit(node).startsWith(u"/dev/input/event")) return;
        // KWin fake-input never enters evdev. Exclude software-created uinput devices too.
        if (strstr(sys, "/devices/virtual/input/")) return;
        const char *seat = udev_device_get_property_value(device, "ID_SEAT");
        if (seat && strcmp(seat, "seat0")) return;
        bool pointerOrKeyboard = false;
        for (const char *property : {"ID_INPUT_KEYBOARD", "ID_INPUT_MOUSE", "ID_INPUT_TOUCHPAD", "ID_INPUT_TOUCHSCREEN"}) {
            const char *value = udev_device_get_property_value(device, property);
            if (value && !strcmp(value, "1")) pointerOrKeyboard = true;
        }
        const QString path = QString::fromLocal8Bit(node);
        if (!pointerOrKeyboard || devices.contains(path)) return;
        const int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { qWarning() << "Cannot observe physical Console activity on" << path; return; }
        auto notifier = new QSocketNotifier(fd, QSocketNotifier::Read, q);
        devices.insert(path, notifier);
        QObject::connect(notifier, &QSocketNotifier::activated, q, [this, fd, path] {
            input_event events[32];
            bool active = false;
            ssize_t count;
            while ((count = read(fd, events, sizeof(events))) > 0) {
                for (ssize_t i = 0; i < count / ssize_t(sizeof(input_event)); ++i)
                    active |= PhysicalInputWatcher::isActivity(events[i]);
            }
            if (count < 0 && errno != EAGAIN && errno != EINTR) remove(path);
            // Only a boolean survives this callback. Never retain or log key codes.
            if (active) Q_EMIT q->activity();
        });
    }
    ~Private() {
        for (auto notifier : std::as_const(devices)) { notifier->setEnabled(false); close(notifier->socket()); }
        if (monitor) udev_monitor_unref(monitor);
        if (context) udev_unref(context);
    }
};
PhysicalInputWatcher::PhysicalInputWatcher(QObject *parent) : QObject(parent), d(std::make_unique<Private>(this)) {}
PhysicalInputWatcher::~PhysicalInputWatcher() = default;
void PhysicalInputWatcher::start() {
    if (!d->context || d->monitor) return;
    d->monitor = udev_monitor_new_from_netlink(d->context, "udev");
    if (!d->monitor) return;
    udev_monitor_filter_add_match_subsystem_devtype(d->monitor, "input", nullptr);
    if (udev_monitor_enable_receiving(d->monitor) < 0) return;
    auto notifier = new QSocketNotifier(udev_monitor_get_fd(d->monitor), QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this] {
        while (auto device = udev_monitor_receive_device(d->monitor)) {
            const char *action = udev_device_get_action(device);
            if (action && !strcmp(action, "remove")) {
                if (const char *node = udev_device_get_devnode(device)) d->remove(QString::fromLocal8Bit(node));
            } else d->add(device);
            udev_device_unref(device);
        }
    });
    auto enumerate = udev_enumerate_new(d->context);
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_scan_devices(enumerate);
    udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
        auto device = udev_device_new_from_syspath(d->context, udev_list_entry_get_name(entry));
        if (device) { d->add(device); udev_device_unref(device); }
    }
    udev_enumerate_unref(enumerate);
}
}
