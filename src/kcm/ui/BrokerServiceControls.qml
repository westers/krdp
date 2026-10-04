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
    property bool showBoot: false
    readonly property var service: administration ? administration.services[route === "console" ? 0 : 1] : ({known: false, error: "", activeState: "", unitFileState: "", canStart: false, canStop: false, canRestart: false, canAutostart: false, autostart: false})
    function endpoint(values) {
        let address = values.Address || "";
        if (address === "0.0.0.0" || address === "::") return "";
        if (address.includes(":")) address = "[" + address + "]";
        return address && values.Port ? address + ":" + values.Port : "";
    }
    function bindingDescription(values) {
        return endpoint(values) || i18nc("@info", "All interfaces · port %1", values.Port || "");
    }
    function request(operation) {
        if (operation === "start") administration.perform(route, operation);
        else { confirmation.operation = operation; confirmation.open(); }
    }
    spacing: Kirigami.Units.smallSpacing
    Kirigami.FormLayout {
        wideMode: root.width >= Kirigami.Units.gridUnit * 32
        Layout.alignment: Qt.AlignLeft
        Layout.fillWidth: false
        QQC2.Switch {
            objectName: root.route + "HostEnabled"
            Kirigami.FormData.label: i18nc("@label", "Allow connections:")
            text: ""
            Accessible.name: i18nc("@option:check", "Allow connections")
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
            visible: root.host && root.host.loaded
            Kirigami.SelectableLabel {
                objectName: root.route + "StoredEndpoint"
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                wrapMode: Text.Wrap
                text: root.host ? root.bindingDescription(root.host.metadata.effective || {}) : ""
            }
            QQC2.ToolButton {
                objectName: root.route + "CopyStoredEndpoint"
                visible: root.host && root.endpoint(root.host.metadata.effective || {}) !== ""
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
            text: root.host ? root.bindingDescription(root.host.runtime.running || {}) : ""
        }
        QQC2.Switch {
            objectName: root.route + "ServiceAutostart"
            Kirigami.FormData.label: i18nc("@label", "Start when computer boots:")
            text: ""
            Accessible.name: i18nc("@option:check", "Start when this computer boots")
            visible: root.showBoot || root.detailsVisible
            enabled: root.service.canAutostart
            checked: root.service.autostart
            onClicked: {
                root.administration.perform(root.route, root.service.autostart ? "disable" : "enable");
                checked = Qt.binding(() => root.service.autostart);
            }
        }
    }
    QQC2.Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap
        visible: !root.host || !root.host.loaded
        text: i18nc("@info", "Saved address: not available. Start the service once so it publishes its settings."); color: Kirigami.Theme.disabledTextColor
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
        wideMode: root.width >= Kirigami.Units.gridUnit * 32
        Layout.alignment: Qt.AlignLeft
        visible: root.showBoot || root.detailsVisible
        Layout.fillWidth: true
        QQC2.Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            visible: root.service.unitFileState === "enabled-runtime"
            text: i18nc("@info", "Enabled for this boot only. Select startup to enable future boots.")
        }
        RowLayout {
            visible: root.detailsVisible
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
    Kirigami.PromptDialog {
        parent: root.QQC2.Overlay.overlay
        popupType: QQC2.Popup.Item
        id: confirmation
        objectName: root.route + "ConfirmServiceOperation"
        property string operation
        title: operation === "stop" ? i18nc("@title:window", "Stop %1?", root.route === "console" ? i18nc("@title", "Console") : i18nc("@title", "Virtual")) : i18nc("@title:window", "Restart %1?", root.route === "console" ? i18nc("@title", "Console") : i18nc("@title", "Virtual"))
        subtitle: i18nc("@info", "Remote clients using this service will disconnect. The other service is unaffected.")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: Kirigami.Action {
            text: confirmation.operation === "stop" ? i18nc("@action:button", "Stop") : i18nc("@action:button", "Restart")
            onTriggered: { confirmation.close(); root.administration.perform(root.route, confirmation.operation); }
        }
    }
}
