// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PlasmaBackendProbe.h"

#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>

#include <wayland-client.h>

namespace KRdp
{
namespace
{
void onGlobal(void *data, wl_registry *, uint32_t, const char *interface, uint32_t)
{
    static_cast<QStringList *>(data)->append(QString::fromLatin1(interface));
}

void onGlobalRemove(void *, wl_registry *, uint32_t)
{
}

const wl_registry_listener registryListener = {onGlobal, onGlobalRemove};
}

QStringList advertisedWaylandGlobals()
{
    QStringList globals;
    if (!qGuiApp || QGuiApplication::platformName() != QLatin1String("wayland")) {
        return globals;
    }
    auto *native = qGuiApp->platformNativeInterface();
    auto *display = native ? static_cast<wl_display *>(native->nativeResourceForIntegration("wl_display")) : nullptr;
    if (!display) {
        return globals;
    }
    wl_event_queue *queue = wl_display_create_queue(display);
    if (!queue) {
        return globals;
    }
    auto *wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    if (!wrapper) {
        wl_event_queue_destroy(queue);
        return globals;
    }
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
    wl_registry *registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (registry) {
        wl_registry_add_listener(registry, &registryListener, &globals);
        if (wl_display_roundtrip_queue(display, queue) < 0) {
            globals.clear();
        }
        wl_registry_destroy(registry);
    }
    wl_event_queue_destroy(queue);
    return globals;
}
}
