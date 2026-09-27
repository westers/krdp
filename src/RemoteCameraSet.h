// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace KRdp
{
/**
 * The MS-RDPECAM devices of one connection, keyed by VirtualChannelName.
 *
 * FreeRDP's enumerator thread adds and removes devices
 * (DeviceAddedNotification / DeviceRemovedNotification) while the RDP
 * session thread walks them to start streams on local demand, so every
 * access holds one mutex. take() hands the device back to the caller, which
 * destroys it after the lock is released: a device's destructor joins its
 * FreeRDP channel thread and tears down its PipeWire node, which must not
 * happen under a lock the session thread waits for.
 */
template<typename Device>
class RemoteCameraSet
{
public:
    bool contains(const QByteArray &channelName) const
    {
        std::lock_guard lock(m_mutex);
        return find(channelName) != m_devices.end();
    }

    /** Add @p device unless a device with @p channelName already exists. Returns whether it was added. */
    bool insert(const QByteArray &channelName, std::unique_ptr<Device> device)
    {
        std::lock_guard lock(m_mutex);
        if (!device || find(channelName) != m_devices.end()) {
            return false;
        }
        m_devices.push_back({channelName, std::move(device)});
        return true;
    }

    /** Remove the device named @p channelName and return it (null if unknown). */
    std::unique_ptr<Device> take(const QByteArray &channelName)
    {
        std::lock_guard lock(m_mutex);
        const auto found = find(channelName);
        if (found == m_devices.end()) {
            return {};
        }
        std::unique_ptr<Device> device = std::move(found->device);
        m_devices.erase(found);
        return device;
    }

    /** Remove every device; the caller destroys them outside the lock. */
    std::vector<std::unique_ptr<Device>> takeAll()
    {
        std::lock_guard lock(m_mutex);
        std::vector<std::unique_ptr<Device>> devices;
        devices.reserve(m_devices.size());
        for (Entry &entry : m_devices) {
            devices.push_back(std::move(entry.device));
        }
        m_devices.clear();
        return devices;
    }

    /** Call @p visit(Device *) for each device, in insertion order, until it returns false. */
    template<typename Visitor>
    void forEach(Visitor &&visit)
    {
        std::lock_guard lock(m_mutex);
        for (Entry &entry : m_devices) {
            if (!visit(entry.device.get())) {
                break;
            }
        }
    }

    size_t size() const
    {
        std::lock_guard lock(m_mutex);
        return m_devices.size();
    }

private:
    struct Entry {
        QByteArray channelName;
        std::unique_ptr<Device> device;
    };
    typename std::vector<Entry>::const_iterator find(const QByteArray &channelName) const
    {
        return std::find_if(m_devices.cbegin(), m_devices.cend(), [&channelName](const Entry &entry) {
            return entry.channelName == channelName;
        });
    }
    typename std::vector<Entry>::iterator find(const QByteArray &channelName)
    {
        return std::find_if(m_devices.begin(), m_devices.end(), [&channelName](const Entry &entry) {
            return entry.channelName == channelName;
        });
    }

    mutable std::mutex m_mutex;
    std::vector<Entry> m_devices;
};
}
