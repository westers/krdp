// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

KCM.SimpleKCM {
    id: root
    objectName: "mainPage"
    title: i18nc("@title", "Farside Remote Desktop")
    property var navigation: kcm
    property var administration: kcm.brokerServices
    property var consoleHost: kcm.consoleHostSettings
    property var virtualHost: kcm.virtualHostSettings
    property var sessionSettings: kcm.virtualSessionSettings
    property var authentication: kcm.brokerAuthentication
    property var preferences: kcm.brokerPreferences
    property string hostName: kcm.hostName
    function openPage(page) { navigation.push(page + ".qml"); }
    function openHost(route) { navigation.push("BrokerHostsPage.qml", {initialScope: route === "console" ? 0 : 1}); }
    function endpoint(fields) {
        if (!fields || !Object.prototype.hasOwnProperty.call(fields, "Address") || !Object.prototype.hasOwnProperty.call(fields, "Port")) return "";
        let address = fields.Address;
        if (["", "*", "0.0.0.0", "::"].includes(address)) address = hostName;
        return (address.includes(":") ? "[" + address + "]" : address) + ":" + fields.Port;
    }
    Component.onCompleted: administration.refresh(false)
    Timer {
        interval: 5000
        running: root.visible && !root.administration.busy
        repeat: true
        onTriggered: root.administration.refresh(false)
    }
    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                objectName: "brokerScopeDescription"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: i18nc("@info", "Connect to this computer's desktop, or use a separate virtual desktop.")
            }
            QQC2.ToolButton {
                objectName: "refreshBrokerStatus"
                text: i18nc("@action", "Refresh Status")
                icon.name: "view-refresh"
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                enabled: !root.administration.busy
                onClicked: root.administration.refresh()
            }
        }
        Repeater {
            id: summaries
            model: root.administration.services
            delegate: Kirigami.FormLayout {
                id: route
                required property var modelData
                readonly property var host: modelData.route === "console" ? root.consoleHost : root.virtualHost
                readonly property var stored: host.metadata.effective || ({})
                readonly property var tls: host.metadata.tls || ({})
                Layout.fillWidth: true
                Kirigami.Separator {
                    objectName: route.modelData.route + "HostHeading"
                    Kirigami.FormData.isSection: true
                    Kirigami.FormData.label: route.modelData.route === "console" ? i18nc("@title:group", "Console") : i18nc("@title:group", "Virtual")
                }
                RowLayout {
                    Kirigami.FormData.label: i18nc("@label", "Remote desktop:")
                    QQC2.Switch {
                        objectName: route.modelData.route + "HostEnabled"
                        text: i18nc("@option:check", "Enabled")
                        Accessible.name: route.modelData.route === "console" ? i18nc("@option:check", "Console enabled") : i18nc("@option:check", "Virtual enabled")
                        checked: route.modelData.activeState === "active" || route.modelData.activeState === "reloading"
                        enabled: route.modelData.canStart || route.modelData.canStop
                        onClicked: {
                            if (checked) root.administration.perform(route.modelData.route, "start");
                            else { stopDialog.route = route.modelData.route; stopDialog.open(); }
                            checked = Qt.binding(() => route.modelData.activeState === "active" || route.modelData.activeState === "reloading");
                        }
                    }
                    BrokerServiceStatus { objectName: route.modelData.route + "HostStatus"; service: route.modelData }
                    Kirigami.ContextualHelpButton {
                        toolTipText: route.modelData.route === "console"
                            ? i18nc("@info:tooltip", "Console shares this computer's desktop, sign-in screen, and lock screen.")
                            : i18nc("@info:tooltip", "Virtual creates separate desktops that can be disconnected and resumed later.")
                    }
                }
                Kirigami.InlineMessage {
                    objectName: route.modelData.route + "HostError"
                    Layout.fillWidth: true
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                    visible: route.modelData.error !== ""
                    type: Kirigami.MessageType.Error
                    text: route.modelData.error
                }
                RowLayout {
                    objectName: route.modelData.route + "StoredEndpointRow"
                    Kirigami.FormData.label: i18nc("@label", "Saved address:")
                    visible: route.host.loaded
                    Kirigami.SelectableLabel {
                        objectName: route.modelData.route + "StoredEndpoint"
                        Layout.fillWidth: true
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 18
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                        wrapMode: Text.Wrap
                        text: root.endpoint(route.stored)
                    }
                    QQC2.ToolButton {
                        objectName: route.modelData.route + "CopyStoredEndpoint"
                        icon.name: "edit-copy"
                        text: i18nc("@action:button", "Copy Address")
                        display: QQC2.AbstractButton.IconOnly
                        QQC2.ToolTip.text: text
                        QQC2.ToolTip.visible: hovered
                        enabled: root.endpoint(route.stored) !== ""
                        onClicked: root.navigation.copyAddressToClipboard(root.endpoint(route.stored))
                    }
                    Kirigami.ContextualHelpButton {
                        toolTipText: i18nc("@info:tooltip", "This address comes from saved settings. Inspect the running host in Host Settings to check the address currently in use.")
                    }
                }
                QQC2.Label {
                    objectName: route.modelData.route + "InspectedEndpoint"
                    Kirigami.FormData.label: i18nc("@label", "Running address:")
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: route.host.runtimeCheckedAt !== "" && !route.host.runtimeStale && route.host.runtime.runningVerified === true
                    text: root.endpoint(route.host.runtime.running)
                }
                Kirigami.SelectableLabel {
                    objectName: route.modelData.route + "StoredFingerprint"
                    Kirigami.FormData.label: i18nc("@label", "Saved fingerprint:")
                    Layout.fillWidth: true
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 18
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                    wrapMode: Text.Wrap
                    visible: route.host.loaded && (route.tls.fingerprint || "") !== ""
                    text: route.tls.fingerprint || ""
                }
                QQC2.Label { objectName: route.modelData.route + "HostPending"; visible: route.host.modified; text: i18nc("@info", "Unsaved changes in Host Settings.") }
                Kirigami.InlineMessage {
                    objectName: route.modelData.route + "HostApply"
                    Layout.fillWidth: true
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                    visible: route.host.applicationRequired
                    text: i18nc("@info", "Settings saved. Restart this service to apply them.")
                }
                QQC2.Button {
                    objectName: route.modelData.route + "HostSettingsLink"
                    text: i18nc("@action:button", "Host Settings…")
                    icon.name: "configure"
                    onClicked: root.openHost(route.modelData.route)
                }
            }
        }
        Kirigami.FormLayout {
            Layout.fillWidth: true
            Item { Kirigami.FormData.isSection: true }
            Flow {
                objectName: "pageButtons"
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                QQC2.Button { objectName: "brokerSignInButton"; text: i18nc("@action:button", "Sign-In…"); icon.name: "system-users"; onClicked: root.openPage("BrokerSignInPage") }
                QQC2.Button { objectName: "brokerPreferencesLink"; text: i18nc("@action:button", "Preferences…"); icon.name: "preferences-desktop"; onClicked: root.openPage("BrokerPreferencesPage") }
                QQC2.Button { objectName: "brokerServicesLink"; text: i18nc("@action:button", "Services…"); icon.name: "system-run"; onClicked: root.openPage("BrokerServicesPage") }
            }
            RowLayout {
                QQC2.Button {
                    objectName: "brokerHostsLink"
                    text: i18nc("@action:button", "Virtual Desktop Defaults…")
                    icon.name: "video-display"
                    onClicked: root.navigation.push("BrokerHostsPage.qml", {initialScope: 2})
                }
                Kirigami.ContextualHelpButton {
                    objectName: "administrationScopeHelp"
                    toolTipText: i18nc("@info:tooltip", "Host settings and sign-in permissions apply to this computer and require administrator authentication. Preferences apply to the signed-in account. New Virtual Desktop settings apply only when creating a desktop.")
                }
            }
            QQC2.Label { objectName: "signInPendingNotice"; visible: root.authentication.modified; text: i18nc("@info", "Unsaved sign-in changes.") }
            QQC2.Label { objectName: "signInSavedNotice"; visible: root.authentication.lastSaveRequiresRestart; text: i18nc("@info", "Restart both services to apply sign-in changes.") }
            QQC2.Label { objectName: "preferencesPendingNotice"; visible: root.preferences.modified; text: i18nc("@info", "Unsaved preferences.") }
            QQC2.Label { visible: root.preferences.reconnectRequired; text: i18nc("@info", "Reconnect to apply saved preferences.") }
            QQC2.Label { visible: root.sessionSettings.modified || root.sessionSettings.applicationRequired; text: i18nc("@info", "New Virtual Desktop settings have pending changes.") }
            QQC2.Label {
                objectName: "stockScopeNotice"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: i18nc("@info", "KDE Remote Desktop has its own settings page.")
                color: Kirigami.Theme.disabledTextColor
            }
        }
    }
    QQC2.Dialog {
        id: stopDialog
        objectName: "confirmMainServiceStop"
        property string route
        modal: true
        title: route === "console" ? i18nc("@title:window", "Stop Console") : i18nc("@title:window", "Stop Virtual")
        footer: QQC2.DialogButtonBox {
            standardButtons: QQC2.Dialog.Cancel
            QQC2.Button {
                text: i18nc("@action:button", "Stop")
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.ActionRole
                onClicked: stopDialog.accept()
            }
        }
        width: Math.min(root.width - Kirigami.Units.largeSpacing * 2, Kirigami.Units.gridUnit * 26)
        x: Math.max(0, (root.width - width) / 2)
        y: Math.max(0, (root.height - height) / 2)
        contentItem: QQC2.Label { wrapMode: Text.Wrap; text: i18nc("@info", "Stopping this service disconnects its remote clients.") }
        onAccepted: root.administration.perform(route, "stop")
    }
}
