// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

KCM.SimpleKCM {
    id: root
    objectName: "brokerServicesPage"
    title: i18nc("@title:window", "Console and Virtual Services")
    property var administration: kcm.brokerServices

    function status(service) {
        if (!service.known) return i18nc("@info:status", "Status unavailable");
        if (service.loadState === "not-found") return i18nc("@info:status", "Not installed");
        if (service.loadState === "masked" || service.unitFileState.startsWith("masked")) return i18nc("@info:status", "Blocked by the administrator");
        switch (service.activeState) {
        case "active": return i18nc("@info:status", "Running");
        case "inactive": return i18nc("@info:status", "Stopped");
        case "failed": return i18nc("@info:status", "Failed");
        case "activating": return i18nc("@info:status", "Starting…");
        case "deactivating": return i18nc("@info:status", "Stopping…");
        case "reloading": return i18nc("@info:status", "Reloading…");
        default: return i18nc("@info:status", "Status unavailable");
        }
    }
    function request(route, operation) {
        if (operation === "start") administration.perform(route, operation);
        else {
            confirmOperation.route = route;
            confirmOperation.operation = operation;
            confirmOperation.open();
        }
    }
    Component.onCompleted: administration.refresh()
    Timer {
        interval: 5000
        running: root.visible && !root.administration.busy
        repeat: true
        onTriggered: root.administration.refresh(false)
    }
    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: i18nc("@info", "These services belong to this computer. Changes apply immediately and may require administrator authentication. Console shares the desktop or sign-in screen; Virtual provides separate retained desktops.")
        }
        QQC2.Button {
            objectName: "refreshBrokerServices"
            text: i18nc("@action:button", "Refresh Status")
            icon.name: "view-refresh"
            enabled: !root.administration.busy
            onClicked: root.administration.refresh()
        }
        Repeater {
            model: root.administration.services
            delegate: ColumnLayout {
                id: section
                required property var modelData
                Layout.fillWidth: true
                Kirigami.Heading {
                    level: 3
                    text: section.modelData.route === "console" ? i18nc("@title:group", "Console") : i18nc("@title:group", "Virtual")
                }
                QQC2.Label {
                    objectName: section.modelData.route + "ServiceStatus"
                    text: root.status(section.modelData)
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: section.modelData.error !== ""
                    type: Kirigami.MessageType.Error
                    text: section.modelData.error
                }
                RowLayout {
                    QQC2.Button {
                        objectName: section.modelData.route + "ServiceStart"
                        text: i18nc("@action:button", "Start")
                        icon.name: "media-playback-start"
                        enabled: section.modelData.canStart
                        onClicked: root.request(section.modelData.route, "start")
                    }
                    QQC2.Button {
                        objectName: section.modelData.route + "ServiceStop"
                        text: i18nc("@action:button", "Stop…")
                        icon.name: "media-playback-stop"
                        enabled: section.modelData.canStop
                        onClicked: root.request(section.modelData.route, "stop")
                    }
                    QQC2.Button {
                        objectName: section.modelData.route + "ServiceRestart"
                        text: i18nc("@action:button", "Restart…")
                        icon.name: "view-refresh"
                        enabled: section.modelData.canRestart
                        onClicked: root.request(section.modelData.route, "restart")
                    }
                    QQC2.BusyIndicator { running: section.modelData.busy; visible: running }
                }
                QQC2.CheckBox {
                    id: startup
                    objectName: section.modelData.route + "ServiceAutostart"
                    text: i18nc("@option:check", "Start when this computer boots")
                    enabled: section.modelData.canAutostart
                    checked: section.modelData.autostart
                    onClicked: {
                        root.administration.perform(section.modelData.route, section.modelData.autostart ? "disable" : "enable");
                        checked = Qt.binding(() => section.modelData.autostart);
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: section.modelData.unitFileState === "enabled-runtime"
                        ? i18nc("@info", "Enabled temporarily for this boot. Select the startup option to enable it for future boots.")
                        : i18nc("@info", "Startup changes do not start or stop the service. Sign-in policy changes require restarting both services.")
                }
            }
        }
    }
    QQC2.Dialog {
        id: confirmOperation
        objectName: "confirmBrokerServiceOperation"
        property string route
        property string operation
        modal: true
        width: Math.min(root.width - Kirigami.Units.largeSpacing * 2, Kirigami.Units.gridUnit * 26)
        x: Math.max(0, (root.width - width) / 2)
        y: Math.max(0, (root.height - height) / 2)
        title: operation === "stop" ? i18nc("@title:window", "Stop Service") : i18nc("@title:window", "Restart Service")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        contentItem: QQC2.Label {
            wrapMode: Text.Wrap
            text: confirmOperation.route === "console"
                ? i18nc("@info", "This disconnects clients using Console. Continue?")
                : i18nc("@info", "This disconnects clients using Virtual. Continue?")
        }
        onAccepted: root.administration.perform(route, operation)
    }
}
