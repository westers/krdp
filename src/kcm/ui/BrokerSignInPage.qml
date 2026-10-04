// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

KCM.SimpleKCM {
    id: root
    objectName: "brokerSignInPage"
    property var administration: kcm.brokerAuthentication
    property var serviceAdministration: null
    property bool removedPending: false
    title: i18nc("@title:window", "Who Can Connect")
    function editAlias(route, alias, owner) {
        aliasDialog.route = route; aliasDialog.existing = alias !== ""; aliasDialog.originalOwner = owner;
        aliasName.text = alias; ownerName.text = owner; aliasPassword.text = ""; aliasDialog.open();
    }
    Connections { target: root.administration; function onChanged() { if (!root.administration.modified) root.removedPending = false; } }
    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        BrokerApplyFailures { }
        Kirigami.InlineMessage { objectName: "brokerAuthenticationError"; Layout.fillWidth: true; visible: root.administration.error !== ""; type: Kirigami.MessageType.Error; text: root.administration.error }
        Kirigami.InlineMessage {
            objectName: "brokerAuthenticationRestart"; Layout.fillWidth: true; visible: root.administration.lastSaveRequiresRestart; type: Kirigami.MessageType.Information
            text: i18nc("@info", "Saved. Restart Console and Virtual to use the new rules.")
            actions: [
                Kirigami.Action { text: i18nc("@action", "Restart Console…"); enabled: root.serviceAdministration && root.serviceAdministration.services[0].canRestart; onTriggered: { restartConfirmation.route = "console"; restartConfirmation.open(); } },
                Kirigami.Action { text: i18nc("@action", "Restart Virtual…"); enabled: root.serviceAdministration && root.serviceAdministration.services[1].canRestart; onTriggered: { restartConfirmation.route = "virtual"; restartConfirmation.open(); } }
            ]
        }
        Kirigami.InlineMessage { visible: root.removedPending; Layout.fillWidth: true; text: i18nc("@info", "Remote login removed. Apply to save the change."); actions: Kirigami.Action { text: i18nc("@action", "Undo Remove"); onTriggered: { root.administration.undoRemoveAlias(); root.removedPending = false; } } }
        // Account names and aliases are administrator protected. Opening this page asks once;
        // if that was cancelled, this is the way to ask again.
        Kirigami.PlaceholderMessage {
            objectName: "accessLocked"
            Layout.fillWidth: true; Layout.topMargin: Kirigami.Units.gridUnit * 2
            visible: !root.administration.loaded
            icon.name: "lock"
            text: root.administration.busy ? i18nc("@info", "Waiting for authorization…") : i18nc("@info", "Administrator access needed")
            explanation: root.administration.busy ? "" : i18nc("@info", "Sign-in rules and remote logins are protected. Unlock them to view or change them.")
            helpfulAction: Kirigami.Action { objectName: "unlockAccessPolicy"; icon.name: "unlock"; text: i18nc("@action:button", "Unlock…"); enabled: !root.administration.busy; onTriggered: root.administration.reload() }
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.administration.loaded
            enabled: !root.administration.busy
            spacing: Kirigami.Units.largeSpacing
            BrokerAccessRoute { id: consoleRoute; Layout.fillWidth: true; page: root; route: "console"; twinFormLayouts: [virtualRoute] }
            BrokerAccessRoute { id: virtualRoute; Layout.fillWidth: true; page: root; route: "virtual"; twinFormLayouts: [consoleRoute] }
        }
    }
    Kirigami.PromptDialog {
        parent: root.QQC2.Overlay.overlay
        popupType: QQC2.Popup.Item
        id: restartConfirmation; property string route: "console"
        title: route === "console" ? i18nc("@title:window", "Restart Console?") : i18nc("@title:window", "Restart Virtual?")
        subtitle: i18nc("@info", "Remote clients using this service will disconnect. The other service is unaffected.")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: Kirigami.Action { text: i18nc("@action:button", "Restart"); onTriggered: { restartConfirmation.close(); root.serviceAdministration.perform(restartConfirmation.route, "restart"); } }
    }
    Kirigami.Dialog {
        id: aliasDialog; objectName: "brokerAliasDialog"
        property string route: "console"
        property bool existing: false
        property string originalOwner: ""
        readonly property bool passwordRequired: !existing || ownerName.text !== originalOwner
        title: existing ? (route === "console" ? i18nc("@title:window", "Edit Console Remote Login") : i18nc("@title:window", "Edit Virtual Remote Login")) : (route === "console" ? i18nc("@title:window", "Add Console Remote Login") : i18nc("@title:window", "Add Virtual Remote Login"))
        preferredWidth: Kirigami.Units.gridUnit * 28
        padding: Kirigami.Units.largeSpacing
        standardButtons: Kirigami.Dialog.Cancel
        onClosed: aliasPassword.text = ""
        Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 24;
            Layout.alignment: Qt.AlignLeft
            QQC2.Label { Kirigami.FormData.isSection: true; Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 26; wrapMode: Text.Wrap; text: i18nc("@info", "This login is saved when you click Apply.") }
            QQC2.TextField { id: aliasName; objectName: "brokerAliasName"; Kirigami.FormData.label: i18nc("@label", "Remote login:"); readOnly: aliasDialog.existing; maximumLength: 256; Layout.fillWidth: true }
            QQC2.TextField { id: ownerName; objectName: "brokerAliasOwner"; Kirigami.FormData.label: i18nc("@label", "Desktop account:"); maximumLength: 256; Layout.fillWidth: true }
            QQC2.Label { Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 18; wrapMode: Text.Wrap; font: Kirigami.Theme.smallFont; text: i18nc("@info", "Enter an existing login name of this computer. It is checked when you apply.") }
            Kirigami.PasswordField { id: aliasPassword; objectName: "brokerAliasPassword"; Kirigami.FormData.label: i18nc("@label", "New password:"); maximumLength: 4096; Layout.fillWidth: true }
            QQC2.Label { Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 18; wrapMode: Text.Wrap; text: aliasDialog.passwordRequired ? i18nc("@info", "A new password is required for a new login or a changed desktop account.") : i18nc("@info", "Leave blank to keep the current password. Saved passwords are never shown.") }
            QQC2.Label { Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 18; wrapMode: Text.Wrap; visible: root.administration.error !== ""; text: root.administration.error }
        }
        customFooterActions: Kirigami.Action {
            objectName: "stageBrokerAlias"
            text: aliasDialog.existing ? i18nc("@action", "Update Login") : i18nc("@action", "Add Login")
            enabled: aliasName.text.trim() !== "" && ownerName.text.trim() !== "" && (!aliasDialog.passwordRequired || aliasPassword.text !== "") && !root.administration.busy
            onTriggered: if (root.administration.setAlias(aliasDialog.route, aliasName.text.trim(), ownerName.text.trim(), aliasPassword.text)) aliasDialog.close()
        }
    }
}
