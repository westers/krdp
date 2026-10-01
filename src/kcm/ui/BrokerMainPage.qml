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
        QQC2.Label {
            objectName: "brokerScopeDescription"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: i18nc("@info", "Console shares this computer's desktop or sign-in screen. Virtual provides separate desktops that can be reattached later. Select the matching connection type in your Farside client.")
        }
        QQC2.Button {
            objectName: "refreshBrokerStatus"
            icon.name: "view-refresh"
            text: i18nc("@action:button", "Refresh Status")
            enabled: !root.administration.busy
            onClicked: root.administration.refresh()
        }
        Repeater {
            model: root.administration.services
            delegate: ColumnLayout {
                id: route
                required property var modelData
                readonly property var host: modelData.route === "console" ? root.consoleHost : root.virtualHost
                readonly property var stored: host.metadata.effective || ({})
                readonly property var tls: host.metadata.tls || ({})
                Layout.fillWidth: true
                Kirigami.Heading {
                    objectName: route.modelData.route + "HostHeading"
                    level: 2
                    text: route.modelData.route === "console" ? i18nc("@title:group", "Console") : i18nc("@title:group", "Virtual")
                }
                BrokerServiceStatus {
                    objectName: route.modelData.route + "HostStatus"
                    service: route.modelData
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                }
                QQC2.Label {
                    objectName: route.modelData.route + "HostError"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: route.modelData.error !== ""
                    text: route.modelData.error
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: !route.host.loaded
                    text: i18nc("@info", "Open Host Settings and load this host to see its stored address and certificate. Administrator authentication may be required.")
                }
                RowLayout {
                    objectName: route.modelData.route + "StoredEndpointRow"
                    Layout.fillWidth: true
                    visible: route.host.loaded
                    Kirigami.SelectableLabel {
                        objectName: route.modelData.route + "StoredEndpoint"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: i18nc("@info", "Stored endpoint: %1", root.endpoint(route.stored))
                    }
                    QQC2.Button {
                        objectName: route.modelData.route + "CopyStoredEndpoint"
                        icon.name: "edit-copy"
                        text: i18nc("@action:button", "Copy")
                        enabled: root.endpoint(route.stored) !== ""
                        onClicked: root.navigation.copyAddressToClipboard(root.endpoint(route.stored))
                    }
                }
                QQC2.Label {
                    objectName: route.modelData.route + "StoredFingerprint"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: (route.tls.fingerprint || "") !== ""
                    text: i18nc("@info", "Stored certificate SHA-256: %1", route.tls.fingerprint || "")
                }
                QQC2.Label {
                    objectName: route.modelData.route + "InspectedEndpoint"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: route.host.runtimeCheckedAt !== "" && !route.host.runtimeStale && route.host.runtime.runningVerified === true
                    text: i18nc("@info", "Inspected startup endpoint: %1 (checked at %2).", root.endpoint(route.host.runtime.running), route.host.runtimeCheckedAt)
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: route.host.loaded
                    text: i18nc("@info", "Stored information does not prove the current listener or loaded certificate. Use Inspect Running Host in Host Settings to compare startup values.")
                }
                QQC2.Label {
                    objectName: route.modelData.route + "HostPending"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: route.host.modified
                    text: i18nc("@info", "Unsaved host edits are kept in Host Settings.")
                }
                QQC2.Label {
                    objectName: route.modelData.route + "HostApply"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: route.host.applicationRequired
                    text: i18nc("@info", "Host settings were saved. Use Services for an explicit restart, then inspect startup values. Restarting disconnects this host's clients.")
                }
                QQC2.Button {
                    objectName: route.modelData.route + "HostSettingsLink"
                    text: i18nc("@action:button", "Host Settings…")
                    icon.name: "configure"
                    onClicked: root.openHost(route.modelData.route)
                }
            }
        }
        QQC2.Label {
            objectName: "administrationScopeHelp"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: i18nc("@info", "Host settings, sign-in policy and services belong to this computer. Your preferences belong to your account and apply when you reconnect. Each section has its own Save and reset actions.")
        }
        QQC2.Label {
            objectName: "signInPendingNotice"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.authentication.modified
            text: i18nc("@info", "Unsaved sign-in edits are kept in Sign-In Settings.")
        }
        QQC2.Label {
            objectName: "signInSavedNotice"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.authentication.lastSaveRequiresRestart
            text: i18nc("@info", "Sign-in policy was saved. Restart both services to apply it.")
        }
        QQC2.Label {
            objectName: "preferencesPendingNotice"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.preferences.modified
            text: i18nc("@info", "Unsaved account preferences are kept in Your Preferences.")
        }
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.preferences.reconnectRequired
            text: i18nc("@info", "Saved account preferences apply when you reconnect.")
        }
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.sessionSettings.modified || root.sessionSettings.applicationRequired
            text: i18nc("@info", "New Virtual Desktop settings are managed in Host Settings. Saved device grants apply only to newly created desktops.")
        }
        Flow {
            objectName: "pageButtons"
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            QQC2.Button {
                objectName: "brokerServicesLink"
                text: i18nc("@action:button", "Services…")
                onClicked: root.openPage("BrokerServicesPage")
            }
            QQC2.Button {
                objectName: "brokerSignInButton"
                text: i18nc("@action:button", "Sign-In Settings…")
                onClicked: root.openPage("BrokerSignInPage")
            }
            QQC2.Button {
                objectName: "brokerPreferencesLink"
                text: i18nc("@action:button", "Your Preferences…")
                onClicked: root.openPage("BrokerPreferencesPage")
            }
            QQC2.Button {
                objectName: "brokerHostsLink"
                text: i18nc("@action:button", "Host Settings…")
                onClicked: root.openPage("BrokerHostsPage")
            }
        }
        QQC2.Label {
            objectName: "stockScopeNotice"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: i18nc("@info", "KDE Remote Desktop is configured separately in its own settings page. Farside's Console and Virtual services use their own settings and certificates.")
        }
    }
}
