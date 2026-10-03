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
      ColumnLayout {
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 48
        Layout.alignment: Qt.AlignLeft
        spacing: Kirigami.Units.largeSpacing
        QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Permissions for both services. Loading and saving require administrator authorization. Save policy changes, then restart both services.") }
        Kirigami.InlineMessage { objectName: "brokerAuthenticationError"; Layout.fillWidth: true; visible: root.administration.error !== ""; type: Kirigami.MessageType.Error; text: root.administration.error }
        Kirigami.InlineMessage {
            objectName: "brokerAuthenticationRestart"; Layout.fillWidth: true; visible: root.administration.lastSaveRequiresRestart; type: Kirigami.MessageType.Information
            text: i18nc("@info", "Access policy saved. Restart Console and Virtual to load it. Saving does not change existing connections.")
            actions: [
                Kirigami.Action { text: i18nc("@action", "Restart Console…"); enabled: root.serviceAdministration && root.serviceAdministration.services[0].canRestart; onTriggered: { restartConfirmation.route = "console"; restartConfirmation.open(); } },
                Kirigami.Action { text: i18nc("@action", "Restart Virtual…"); enabled: root.serviceAdministration && root.serviceAdministration.services[1].canRestart; onTriggered: { restartConfirmation.route = "virtual"; restartConfirmation.open(); } }
            ]
        }
        QQC2.Button { objectName: "unlockBrokerAuthentication"; visible: !root.administration.loaded; text: i18nc("@action:button", "Load Administrator Settings…"); icon.name: "document-edit"; enabled: !root.administration.busy; onClicked: root.administration.reload() }
        Kirigami.InlineMessage { visible: root.removedPending; Layout.fillWidth: true; text: i18nc("@info", "Remote login removed from this draft. Save the access policy to apply it."); actions: Kirigami.Action { text: i18nc("@action", "Undo Remove"); onTriggered: { root.administration.undoRemoveAlias(); root.removedPending = false; } } }
        Repeater {
            model: ["console", "virtual"]
            delegate: ColumnLayout {
                id: section
                required property string modelData
                readonly property var route: root.administration.policy[modelData] || {}
                readonly property var pam: route.pam || {mode: "disabled", accounts: []}
                Layout.fillWidth: true
                visible: root.administration.loaded
                enabled: root.administration.loaded && !root.administration.busy
                Kirigami.Heading {
                    level: 2
                    text: section.modelData === "console" ? i18nc("@title:group", "Console") : i18nc("@title:group", "Virtual")
                }
                Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 32;
            Layout.alignment: Qt.AlignLeft
                    Layout.fillWidth: true
                    QQC2.ComboBox {
                        objectName: section.modelData + "PamMode"
                        Kirigami.FormData.label: i18nc("@label", "System accounts:")
                        implicitContentWidthPolicy: QQC2.ComboBox.WidestText
                        Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            {text: i18nc("@item:inlistbox", "All eligible accounts"), value: "any"},
                            {text: i18nc("@item:inlistbox", "Selected accounts"), value: "allow-list"},
                            {text: i18nc("@item:inlistbox", "Disabled"), value: "disabled"}
                        ]
                        currentIndex: section.pam.mode === "any" ? 0 : section.pam.mode === "allow-list" ? 1 : 2
                        onActivated: root.administration.setPam(section.modelData, currentValue,
                            currentValue === "allow-list" ? section.pam.accounts : [])
                    }
                    QQC2.TextField {
                        Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                        objectName: section.modelData + "PamAccounts"
                        Kirigami.FormData.label: i18nc("@label", "Allowed accounts:")
                        visible: section.pam.mode === "allow-list"
                        text: section.pam.accounts.join(", ")
                        placeholderText: i18nc("@info:placeholder", "System login names, separated by commas")
                        onEditingFinished: root.administration.setPam(section.modelData, "allow-list",
                            text.split(",").map(value => value.trim()).filter(value => value !== ""))
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: section.modelData === "console"
                        ? i18nc("@info", "Console control is limited to the signed-in desktop owner.")
                        : i18nc("@info", "Virtual desktops belong to the account used to sign in.")
                }
                Kirigami.Heading { level: 4; text: i18nc("@title:group", "Remote logins") }
                QQC2.Label { visible: (section.route.credentials || []).length === 0; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "No remote logins added."); color: Kirigami.Theme.disabledTextColor }
                ListView {
                    Layout.fillWidth: true
                    implicitHeight: contentHeight
                    interactive: false
                    clip: true
                    model: section.route.credentials || []
                    delegate: QQC2.ItemDelegate {
                        id: account
                        required property var modelData
                        width: ListView.view.width
                        contentItem: RowLayout {
                            QQC2.Label { Layout.fillWidth: true; text: i18nc("@info %1 remote login %2 desktop owner", "%1 → %2", account.modelData.alias, account.modelData.owner); elide: Text.ElideRight }
                            QQC2.ToolButton { text: i18nc("@action:button", "Edit…"); onClicked: root.editAlias(section.modelData, account.modelData.alias, account.modelData.owner) }
                            QQC2.ToolButton { text: i18nc("@action:button", "Remove"); onClicked: { if (root.administration.removeAlias(section.modelData, account.modelData.alias)) root.removedPending = true; } }
                        }
                    }
                }
                RowLayout {
                    QQC2.Button {
                        objectName: section.modelData + "AddAlias"
                        text: i18nc("@action:button", "Add Remote Login…")
                        icon.name: "list-add"
                        onClicked: root.editAlias(section.modelData, "", "")
                    }
                    Kirigami.ContextualHelpButton {
                        toolTipText: i18nc("@info:tooltip", "System-account passwords are checked using the computer's normal authentication. Remote logins use a separate password and the system account selected here; the remote login name never chooses the desktop owner.")
                    }
                }
                Kirigami.Separator { Layout.fillWidth: true }
            }
        }
        QQC2.Label { visible: root.administration.modified; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Unsaved changes · access policy only"); color: Kirigami.Theme.disabledTextColor }
    }
    }
    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            QQC2.Button { objectName: "discardBrokerAuthentication"; text: i18nc("@action:button", "Revert Changes"); enabled: root.administration.modified && !root.administration.busy; onClicked: root.administration.discard() }
            QQC2.ToolButton { objectName: "loadBrokerAuthentication"; icon.name: "view-refresh"; text: i18nc("@action:button", "Reload Policy…"); display: QQC2.AbstractButton.IconOnly; QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered; enabled: !root.administration.busy; onClicked: { if (root.administration.modified) reloadConfirmation.open(); else root.administration.reload(); } }
            Item { Layout.fillWidth: true }
            QQC2.BusyIndicator { running: root.administration.busy; visible: running; Layout.preferredWidth: Kirigami.Units.gridUnit; Layout.preferredHeight: Kirigami.Units.gridUnit }
            QQC2.Button { objectName: "saveBrokerAuthentication"; highlighted: true; text: i18nc("@action:button", "Save Access Policy…"); enabled: root.administration.loaded && root.administration.modified && !root.administration.busy; onClicked: root.administration.save() }
        }
    }
    Kirigami.PromptDialog {
        parent: root.QQC2.Overlay.overlay
        popupType: QQC2.Popup.Item
        id: reloadConfirmation
        title: i18nc("@title:window", "Reload Access Policy?"); subtitle: i18nc("@info", "Discard unsaved access-policy changes and reload saved values?")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: Kirigami.Action { text: i18nc("@action:button", "Discard and Reload"); onTriggered: { reloadConfirmation.close(); root.administration.reload(); } }
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
            QQC2.Label { Kirigami.FormData.isSection: true; Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 26; wrapMode: Text.Wrap; text: i18nc("@info", "Stage this login in the access policy. Save that policy separately to apply it.") }
            QQC2.TextField { id: aliasName; objectName: "brokerAliasName"; Kirigami.FormData.label: i18nc("@label", "Remote login:"); readOnly: aliasDialog.existing; maximumLength: 256; Layout.fillWidth: true }
            QQC2.TextField { id: ownerName; objectName: "brokerAliasOwner"; Kirigami.FormData.label: i18nc("@label", "Desktop account:"); maximumLength: 256; Layout.fillWidth: true }
            QQC2.Label { Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 18; wrapMode: Text.Wrap; text: i18nc("@info", "Enter an existing system login name. The server verifies account eligibility when saving."); color: Kirigami.Theme.disabledTextColor }
            Kirigami.PasswordField { id: aliasPassword; objectName: "brokerAliasPassword"; Kirigami.FormData.label: i18nc("@label", "New password:"); maximumLength: 4096; Layout.fillWidth: true }
            QQC2.Label { Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 18; wrapMode: Text.Wrap; text: aliasDialog.passwordRequired ? i18nc("@info", "A new password is required for a new login or changed desktop account.") : i18nc("@info", "Leave blank to keep the password. Stored passwords are never displayed.") }
            QQC2.Label { Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 18; wrapMode: Text.Wrap; visible: root.administration.error !== ""; text: root.administration.error }
        }
        customFooterActions: Kirigami.Action {
            objectName: "stageBrokerAlias"
            text: aliasDialog.existing ? i18nc("@action", "Update Policy") : i18nc("@action", "Add to Policy")
            enabled: aliasName.text.trim() !== "" && ownerName.text.trim() !== "" && (!aliasDialog.passwordRequired || aliasPassword.text !== "") && !root.administration.busy
            onTriggered: if (root.administration.setAlias(aliasDialog.route, aliasName.text.trim(), ownerName.text.trim(), aliasPassword.text)) aliasDialog.close()
        }
    }
}
