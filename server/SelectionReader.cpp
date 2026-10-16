// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "SelectionReader.h"

#include <fcntl.h>

#include <QHash>
#include <QLoggingCategory>

#include <wayland-client.h>

extern "C" {
#include "ext-data-control-v1-client-protocol.h"
}

namespace KRdp::SelectionReader
{
namespace
{
struct State {
    wl_seat *seat = nullptr;
    ext_data_control_manager_v1 *manager = nullptr;
    ext_data_control_device_v1 *device = nullptr;
    ext_data_control_offer_v1 *selection = nullptr;
    QHash<ext_data_control_offer_v1 *, QStringList> offers;
};

void registryGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    auto *state = static_cast<State *>(data);
    if (!state->seat && qstrcmp(interface, "wl_seat") == 0) {
        state->seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (!state->manager && qstrcmp(interface, "ext_data_control_manager_v1") == 0) {
        Q_UNUSED(version)
        state->manager = static_cast<ext_data_control_manager_v1 *>(wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
    }
}

void registryRemove(void *, wl_registry *, uint32_t)
{
}

const wl_registry_listener registryListener = {registryGlobal, registryRemove};

void offerMime(void *data, ext_data_control_offer_v1 *offer, const char *mime)
{
    static_cast<State *>(data)->offers[offer].append(QString::fromUtf8(mime));
}

const ext_data_control_offer_v1_listener offerListener = {offerMime};

void deviceDataOffer(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer)
{
    auto *state = static_cast<State *>(data);
    state->offers.insert(offer, {});
    ext_data_control_offer_v1_add_listener(offer, &offerListener, state);
}

void deviceSelection(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer)
{
    static_cast<State *>(data)->selection = offer;
}

void deviceFinished(void *, ext_data_control_device_v1 *)
{
}

void devicePrimarySelection(void *, ext_data_control_device_v1 *, ext_data_control_offer_v1 *)
{
}

const ext_data_control_device_v1_listener deviceListener = {deviceDataOffer, deviceSelection, deviceFinished, devicePrimarySelection};
}

Result readSelection(const QStringList &mimeOrder, const std::function<qint64(const QString &mime)> &capForMime, int idleMs, int totalMs)
{
    Result result;
    wl_display *display = wl_display_connect(nullptr);
    if (!display) return result; // Unavailable
    State state;
    wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, &state);
    if (wl_display_roundtrip(display) < 0 || !state.seat || !state.manager) {
        if (state.manager) ext_data_control_manager_v1_destroy(state.manager);
        if (state.seat) wl_seat_destroy(state.seat);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return result; // Unavailable
    }
    result.status = Status::Failed;
    state.device = ext_data_control_manager_v1_get_data_device(state.manager, state.seat);
    ext_data_control_device_v1_add_listener(state.device, &deviceListener, &state);
    // The compositor sends the current selection when a device is created; one round trip delivers data_offer, offer* and selection.
    if (wl_display_roundtrip(display) >= 0) {
        if (!state.selection) {
            result.status = Status::NoSelection;
        } else {
            const QStringList offered = state.offers.value(state.selection);
            QString chosen;
            for (const QString &candidate : mimeOrder) {
                if (offered.contains(candidate)) {
                    chosen = candidate;
                    break;
                }
            }
            if (chosen.isEmpty()) {
                result.status = Status::NoFormat;
            } else {
                int fds[2];
                if (pipe2(fds, O_CLOEXEC) == 0) {
                    ext_data_control_offer_v1_receive(state.selection, chosen.toUtf8().constData(), fds[1]);
                    ::close(fds[1]);
                    wl_display_flush(display);
                    const FdRead read = readCapped(fds[0], capForMime(chosen), idleMs, totalMs);
                    ::close(fds[0]); // an abandoned read: the source's next write fails with EPIPE
                    result.status = read.status;
                    result.data = read.data;
                    result.mime = chosen;
                    result.bytes = read.bytes;
                }
            }
        }
    }
    for (auto it = state.offers.cbegin(); it != state.offers.cend(); ++it) ext_data_control_offer_v1_destroy(it.key());
    ext_data_control_device_v1_destroy(state.device);
    ext_data_control_manager_v1_destroy(state.manager);
    wl_seat_destroy(state.seat);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return result;
}
}
