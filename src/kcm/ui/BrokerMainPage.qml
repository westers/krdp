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
    property var navigation: kcm
    property var administration: kcm.brokerServices
    property var consoleHost: kcm.consoleHostSettings
    property var virtualHost: kcm.virtualHostSettings
    property var sessionSettings: kcm.virtualSessionSettings
    property var authentication: kcm.brokerAuthentication
    property var preferences: kcm.brokerPreferences
    property string hostName: kcm.hostName
    title: i18nc("@title:window", "Farside Remote Desktop")
    // Native KCM navigation: each destination is pushed on the KCM page stack and
    // the shell supplies the header, title and Back. Unsaved edits live in the
    // scoped models, so a popped page loses only transient view state.
    function hostProperties(scope) {
        return {objectName: scope === 0 ? "consoleSettingsPage" : "virtualSettingsPage", fixedScope: scope, consoleSettings: consoleHost, virtualSettings: virtualHost, sessionSettings: sessionSettings, administration: administration, navigation: root};
    }
    function openConsole() { navigation.push("BrokerHostsPage.qml", hostProperties(0)); }
    function openVirtual() { navigation.push("BrokerHostsPage.qml", hostProperties(1)); }
    function openAccess() { navigation.push("BrokerSignInPage.qml", {administration: authentication, serviceAdministration: administration}); }
    function pushPreferences(scrollToDisplays) { navigation.push("BrokerPreferencesPage.qml", {preferences: preferences, scrollToDisplays: scrollToDisplays}); }
    function openPreferences() { pushPreferences(false); }
    function showPreferences() { pushPreferences(true); }
    function copyAddressToClipboard(address) { navigation.copyAddressToClipboard(address); }
    Component.onCompleted: if (!administration.busy && administration.services.some(service => !service.known)) administration.refresh(false)
    ColumnLayout {
      ColumnLayout {
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 48
        Layout.alignment: Qt.AlignLeft
        spacing: Kirigami.Units.largeSpacing
        QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Choose which desktop to make available. Service switches apply immediately; other changes are saved with Apply.") }
        BrokerApplyFailures { }
        // Saved changes only take effect when the service restarts, and a restart drops connected clients,
        // so it is never done silently: each action asks for confirmation first.
        Kirigami.InlineMessage {
            id: restartNotice; objectName: "restartRequired"; Layout.fillWidth: true; type: Kirigami.MessageType.Information
            readonly property bool restartConsole: root.consoleHost.applicationRequired || root.authentication.lastSaveRequiresRestart
            readonly property bool restartVirtual: root.virtualHost.applicationRequired || root.authentication.lastSaveRequiresRestart
            visible: restartConsole || restartVirtual
            text: restartConsole && restartVirtual ? i18nc("@info", "Saved. Restart Console and Virtual to apply the changes.") : restartConsole ? i18nc("@info", "Saved. Restart Console to apply the changes.") : i18nc("@info", "Saved. Restart Virtual to apply the changes.")
            actions: [
                Kirigami.Action { objectName: "restartNoticeConsole"; visible: restartNotice.restartConsole; text: i18nc("@action", "Restart Console…"); enabled: root.administration.services[0].canRestart; onTriggered: consoleControls.request("restart") },
                Kirigami.Action { objectName: "restartNoticeVirtual"; visible: restartNotice.restartVirtual; text: i18nc("@action", "Restart Virtual…"); enabled: root.administration.services[1].canRestart; onTriggered: virtualControls.request("restart") }
            ]
        }
        Kirigami.Heading { level: 2; text: i18nc("@title:group", "Console") }
        QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Share this computer's desktop.") }
        BrokerServiceControls { id: consoleControls; Layout.fillWidth: true; administration: root.administration; route: "console"; host: root.consoleHost; navigation: root; hostName: root.hostName; showDetailsToggle: false; showBoot: true }
        Flow {
            Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
            QQC2.Button { objectName: "configureConsole"; text: root.consoleHost.modified ? i18nc("@action:button", "Configure Console… (unsaved)") : i18nc("@action:button", "Configure Console…"); onClicked: root.openConsole() }
            QQC2.Button { objectName: "consoleRestart"; text: i18nc("@action:button", "Restart…"); enabled: root.administration.services[0].canRestart; onClicked: consoleControls.request("restart") }
            QQC2.Button { objectName: "consoleStop"; text: i18nc("@action:button", "Stop…"); enabled: root.administration.services[0].canStop; onClicked: consoleControls.request("stop") }
            QQC2.Button { objectName: "consoleDetailsToggle"; flat: true; icon.name: consoleDetailsBox.visible ? "arrow-down" : "arrow-right"; text: i18nc("@action:button", "Details"); onClicked: consoleDetailsBox.visible = !consoleDetailsBox.visible }
        }
        BrokerServiceDetails { id: consoleDetailsBox; visible: false; Layout.fillWidth: true; host: root.consoleHost; administration: root.administration; route: "console"; navigation: root; hostName: root.hostName }
        Kirigami.Separator { Layout.fillWidth: true }
        Kirigami.Heading { level: 2; text: i18nc("@title:group", "Virtual") }
        QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Separate desktops for remote users.") }
        BrokerServiceControls { id: virtualControls; Layout.fillWidth: true; administration: root.administration; route: "virtual"; host: root.virtualHost; navigation: root; hostName: root.hostName; showDetailsToggle: false; showBoot: true }
        Flow {
            Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
            QQC2.Button { objectName: "configureVirtual"; text: root.virtualHost.modified || root.sessionSettings.modified ? i18nc("@action:button", "Configure Virtual… (unsaved)") : i18nc("@action:button", "Configure Virtual…"); onClicked: root.openVirtual() }
            QQC2.Button { objectName: "virtualRestart"; text: i18nc("@action:button", "Restart…"); enabled: root.administration.services[1].canRestart; onClicked: virtualControls.request("restart") }
            QQC2.Button { objectName: "virtualStop"; text: i18nc("@action:button", "Stop…"); enabled: root.administration.services[1].canStop; onClicked: virtualControls.request("stop") }
            QQC2.Button { objectName: "virtualDetailsToggle"; flat: true; icon.name: virtualDetailsBox.visible ? "arrow-down" : "arrow-right"; text: i18nc("@action:button", "Details"); onClicked: virtualDetailsBox.visible = !virtualDetailsBox.visible }
        }
        BrokerServiceDetails { id: virtualDetailsBox; visible: false; Layout.fillWidth: true; host: root.virtualHost; administration: root.administration; route: "virtual"; navigation: root; hostName: root.hostName }
      }
    }
    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            QQC2.Button { objectName: "configureAccess"; text: root.authentication.modified ? i18nc("@action:button", "Who Can Connect… (unsaved)") : i18nc("@action:button", "Who Can Connect…"); onClicked: root.openAccess() }
            QQC2.Button { objectName: "configurePreferences"; text: root.preferences.modified ? i18nc("@action:button", "My Preferences… (unsaved)") : i18nc("@action:button", "My Preferences…"); onClicked: root.openPreferences() }
            Item { Layout.fillWidth: true }
        }
    }
}
