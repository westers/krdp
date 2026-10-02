// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root
    required property var administration
    required property string route
    property var navigation
    property var host
    property string hostName: ""
    property bool detailsVisible: false
    property bool showDetailsToggle: true
    readonly property var service: administration ? administration.services[route === "console" ? 0 : 1] : ({known: false, error: "", activeState: "", unitFileState: "", canStart: false, canStop: false, canRestart: false, canAutostart: false, autostart: false})
    function endpoint(values) {
        let address = values.Address || "";
        if (address === "0.0.0.0" || address === "::") address = hostName;
        if (address.includes(":")) address = "[" + address + "]";
        return address && values.Port ? address + ":" + values.Port : "";
    }
    function request(operation) {
        if (operation === "start") administration.perform(route, operation);
        else { confirmation.operation = operation; confirmation.open(); }
    }
    spacing: Kirigami.Units.smallSpacing
    Kirigami.FormLayout {
        Layout.fillWidth: true
        QQC2.Switch {
            objectName: root.route + "HostEnabled"
            Kirigami.FormData.label: i18nc("@label", "Remote desktop:")
            text: i18nc("@option:check", "Allow connections")
            checked: root.service.activeState === "active" || root.service.activeState === "reloading"
            enabled: root.service.canStart || root.service.canStop
            onClicked: {
                root.request(checked ? "start" : "stop");
                checked = Qt.binding(() => root.service.activeState === "active" || root.service.activeState === "reloading");
            }
        }
        BrokerServiceStatus {
            objectName: root.route + "HostStatus"
            Kirigami.FormData.label: i18nc("@label", "Status:")
            service: root.service
        }
        RowLayout {
            objectName: root.route + "StoredEndpointRow"
            Kirigami.FormData.label: i18nc("@label", "Saved address:")
            visible: root.host && root.host.loaded && root.endpoint(root.host.metadata.effective || {}) !== ""
            Kirigami.SelectableLabel {
                objectName: root.route + "StoredEndpoint"
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                wrapMode: Text.Wrap
                text: root.host ? root.endpoint(root.host.metadata.effective || {}) : ""
            }
            QQC2.ToolButton {
                objectName: root.route + "CopyStoredEndpoint"
                icon.name: "edit-copy"; text: i18nc("@action:button", "Copy address")
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered
                onClicked: root.navigation.copyAddressToClipboard(root.endpoint(root.host.metadata.effective || {}))
            }
        }
        QQC2.Label {
            objectName: root.route + "InspectedEndpoint"
            Kirigami.FormData.label: i18nc("@label", "Running address:")
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            wrapMode: Text.Wrap
            visible: root.host && root.host.runtimeCheckedAt !== "" && !root.host.runtimeStale && root.host.runtime.runningVerified === true
            text: root.host ? root.endpoint(root.host.runtime.running || {}) : ""
        }
    }
    Kirigami.InlineMessage {
        Layout.fillWidth: true
        type: Kirigami.MessageType.Error
        visible: root.service.error !== ""
        text: root.service.error
    }
    QQC2.Button {
        objectName: root.showDetailsToggle ? root.route + "ServiceDetails" : ""
        visible: root.showDetailsToggle
        text: root.detailsVisible ? i18nc("@action:button", "Hide service details") : i18nc("@action:button", "Service details")
        icon.name: root.detailsVisible ? "arrow-down" : "arrow-right"
        flat: true
        onClicked: root.detailsVisible = !root.detailsVisible
    }
    Kirigami.FormLayout {
        visible: root.detailsVisible
        Layout.fillWidth: true
        QQC2.CheckBox {
            objectName: root.route + "ServiceAutostart"
            Kirigami.FormData.label: i18nc("@label", "Startup:")
            text: i18nc("@option:check", "Start when this computer boots")
            enabled: root.service.canAutostart
            checked: root.service.autostart
            onClicked: {
                root.administration.perform(root.route, root.service.autostart ? "disable" : "enable");
                checked = Qt.binding(() => root.service.autostart);
            }
        }
        QQC2.Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            visible: root.service.unitFileState === "enabled-runtime"
            text: i18nc("@info", "Enabled for this boot only. Select startup to enable future boots.")
        }
        RowLayout {
            QQC2.Button { objectName: root.route + "ServiceRestart"; text: i18nc("@action:button", "Restart…"); enabled: root.service.canRestart; onClicked: root.request("restart") }
            QQC2.Button { objectName: root.route + "ServiceStop"; text: i18nc("@action:button", "Stop…"); enabled: root.service.canStop; onClicked: root.request("stop") }
            QQC2.ToolButton {
                objectName: root.route + "RefreshStatus"
                icon.name: "view-refresh"; text: i18nc("@action:button", "Refresh status")
                enabled: root.administration && !root.administration.busy
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered
                onClicked: root.administration.refresh()
            }
        }
    }
    QQC2.Dialog {
        id: confirmation
        objectName: root.route + "ConfirmServiceOperation"
        property string operation
        parent: root
        modal: true
        width: Math.min(root.width - 12, Kirigami.Units.gridUnit * 26)
        x: Math.max(0, (root.width - width) / 2)
        title: operation === "stop" ? i18nc("@title:window", "Stop %1", root.route === "console" ? "Console" : "Virtual") : i18nc("@title:window", "Restart %1", root.route === "console" ? "Console" : "Virtual")
        contentItem: QQC2.Label { wrapMode: Text.Wrap; text: i18nc("@info", "This disconnects remote clients using this service.") }
        footer: QQC2.DialogButtonBox {
            standardButtons: QQC2.Dialog.Cancel
            QQC2.Button {
                text: confirmation.operation === "stop" ? i18nc("@action:button", "Stop") : i18nc("@action:button", "Restart")
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.ActionRole
                onClicked: confirmation.accept()
            }
        }
        onAccepted: root.administration.perform(root.route, operation)
    }
}
