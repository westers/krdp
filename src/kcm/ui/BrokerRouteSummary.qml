// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
import QtQuick
// What the sidebar row and the route page both say about one service: its
// state in words and the address clients use. Pure presentation, no actions.
QtObject {
    id: root
    property var service
    property var host
    readonly property bool running: !!service && (service.activeState === "active" || service.activeState === "reloading")
    readonly property string stateText: {
        const s = service;
        if (!s || !s.known) return i18nc("@info:status", "Status unavailable");
        if (s.loadState === "not-found") return i18nc("@info:status", "Not installed");
        if (s.loadState === "masked" || s.unitFileState.startsWith("masked")) return i18nc("@info:status", "Blocked by the administrator");
        switch (s.activeState) {
        case "active": return i18nc("@info:status", "Running");
        case "inactive": return i18nc("@info:status", "Stopped");
        case "failed": return i18nc("@info:status", "Failed");
        case "activating": return i18nc("@info:status", "Starting…");
        case "deactivating": return i18nc("@info:status", "Stopping…");
        case "reloading": return i18nc("@info:status", "Reloading…");
        default: return i18nc("@info:status", "Status unavailable");
        }
    }
    readonly property var effective: host && host.loaded ? (host.metadata.effective || {}) : ({})
    function endpoint(values) {
        let address = values.Address || "";
        if (address === "0.0.0.0" || address === "::") return "";
        if (address.includes(":")) address = "[" + address + "]";
        return address && values.Port ? address + ":" + values.Port : "";
    }
    // The address as configured: only a concrete address can be typed by a client. A wildcard listener
    // has no single name, and none is invented for it.
    readonly property string address: effective.Port ? endpoint(effective) : ""
    // What the page says about where the service listens, whether or not it is a concrete address.
    readonly property string listenText: {
        if (!effective.Port) return "";
        return address !== "" ? address : i18nc("@info %1 port number", "All network interfaces, port %1", effective.Port);
    }
    // The sidebar row: state and port only (the page itself names the full address).
    readonly property string subtitle: effective.Port ? i18nc("@info:status %1 state %2 port number", "%1 · port %2", stateText, effective.Port) : stateText
}
