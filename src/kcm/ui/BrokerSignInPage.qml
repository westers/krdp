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
    title: i18nc("@title:window", "Console and Virtual Sign-In")
    property var administration: kcm.brokerAuthentication
    property var consoleSettings: null
    property var virtualSettings: null
    property var sessionSettings: null
    property var serviceAdministration: null

    function editAlias(route, alias, owner) {
        aliasDialog.route = route;
        aliasDialog.existing = alias !== "";
        aliasName.text = alias;
        ownerName.text = owner;
        aliasPassword.text = "";
        aliasDialog.open();
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: i18nc("@info", "Choose who can connect. Loading and saving require administrator authentication.")
        }
        Kirigami.InlineMessage {
            objectName: "brokerAuthenticationError"
            Layout.fillWidth: true
            visible: root.administration.error !== ""
            type: Kirigami.MessageType.Error
            text: root.administration.error
        }
        Kirigami.InlineMessage {
            objectName: "brokerAuthenticationRestart"
            Layout.fillWidth: true
            visible: root.administration.lastSaveRequiresRestart
            type: Kirigami.MessageType.Information
            text: i18nc("@info", "Sign-in policy saved. Restart both Console and Virtual services to load it. Existing connections are not changed by saving.")
        }
        QQC2.Button {
            objectName: "unlockBrokerAuthentication"
            text: i18nc("@action:button", "Load Administrator Settings…")
            icon.name: "document-edit"
            visible: !root.administration.loaded
            enabled: !root.administration.busy
            onClicked: root.administration.reload()
        }
        Kirigami.FormLayout {
            Layout.fillWidth: true
            visible: root.consoleSettings !== null && root.virtualSettings !== null
            Item { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Certificates") }
            Repeater {
                model: ["console", "virtual"]
                delegate: RowLayout {
                    id: certificateRow
                    required property string modelData
                    readonly property var host: modelData === "console" ? root.consoleSettings : root.virtualSettings
                    Kirigami.FormData.label: modelData === "console" ? i18nc("@label", "Console:") : i18nc("@label", "Virtual:")
                    QQC2.Label {
                        text: certificateRow.host && certificateRow.host.loaded ? (certificateRow.host.metadata.tls || {}).state || i18nc("@info", "Unavailable") : i18nc("@info", "Not loaded")
                    }
                    QQC2.Button {
                        objectName: certificateRow.modelData + "CertificateDetails"
                        text: i18nc("@action:button", "Details…")
                        onClicked: { certificateDialog.scope = certificateRow.modelData === "console" ? 0 : 1; certificateDialog.open(); }
                    }
                }
            }
        }
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
                    level: 3
                    text: section.modelData === "console" ? i18nc("@title:group", "Console") : i18nc("@title:group", "Virtual")
                }
                Kirigami.FormLayout {
                    Layout.fillWidth: true
                    QQC2.ComboBox {
                        objectName: section.modelData + "PamMode"
                        Kirigami.FormData.label: i18nc("@label", "System accounts:")
                        implicitContentWidthPolicy: QQC2.ComboBox.WidestText
                        Layout.minimumWidth: Kirigami.Units.gridUnit * 16
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            {text: i18nc("@item:inlistbox", "Any non-root system account"), value: "any"},
                            {text: i18nc("@item:inlistbox", "Only listed system accounts"), value: "allow-list"},
                            {text: i18nc("@item:inlistbox", "No system accounts"), value: "disabled"}
                        ]
                        currentIndex: section.pam.mode === "any" ? 0 : section.pam.mode === "allow-list" ? 1 : 2
                        onActivated: root.administration.setPam(section.modelData, currentValue,
                            currentValue === "allow-list" ? section.pam.accounts : [])
                    }
                    QQC2.TextField {
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
                            QQC2.ToolButton { text: i18nc("@action:button", "Remove"); onClicked: root.administration.removeAlias(section.modelData, account.modelData.alias) }
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
    }
    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            QQC2.ToolButton {
                objectName: "loadBrokerAuthentication"
                icon.name: "view-refresh"; text: i18nc("@action:button", "Reload Policy…")
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered
                enabled: !root.administration.busy
                onClicked: { if (root.administration.modified) discardDialog.open(); else root.administration.reload(); }
            }
            Item { Layout.fillWidth: true }
            QQC2.BusyIndicator { running: root.administration.busy; Layout.preferredWidth: Kirigami.Units.gridUnit; Layout.preferredHeight: Kirigami.Units.gridUnit; opacity: running ? 1 : 0 }
            QQC2.Button { objectName: "saveBrokerAuthentication"; text: i18nc("@action:button", "Save Access Policy…"); icon.name: "document-save"; enabled: root.administration.loaded && root.administration.modified && !root.administration.busy; onClicked: root.administration.save() }
        }
    }
    QQC2.Dialog {
        id: certificateDialog
        objectName: "certificateDetailsDialog"
        parent: root
        modal: true
        property int scope: 0
        title: scope === 0 ? i18nc("@title:window", "Console Certificate") : i18nc("@title:window", "Virtual Certificate")
        width: Math.min(root.width - 12, Kirigami.Units.gridUnit * 42)
        height: Math.min(root.height - 12, Kirigami.Units.gridUnit * 34)
        x: Math.max(0, (root.width - width) / 2); y: Math.max(0, (root.height - height) / 2)
        standardButtons: QQC2.Dialog.Close
        contentItem: Loader {
            id: certificateLoader
            active: root.consoleSettings !== null && root.virtualSettings !== null && root.sessionSettings !== null
            sourceComponent: Component {
            BrokerHostsPage {
            certificateOnly: true
            fixedScope: certificateDialog.scope
            consoleSettings: root.consoleSettings
            virtualSettings: root.virtualSettings
            sessionSettings: root.sessionSettings
            }
            }
        }
    }
    QQC2.Dialog {
        id: discardDialog
        parent: root
        width: Math.min(root.width - 24, Kirigami.Units.gridUnit * 25)
        x: Math.max(0, (root.width - width) / 2)
        y: Math.max(0, (root.height - height) / 2)
        title: i18nc("@title:window", "Discard Sign-In Edits?")
        modal: true
        standardButtons: QQC2.Dialog.Discard | QQC2.Dialog.Cancel
        onDiscarded: root.administration.reload()
        contentItem: QQC2.Label { text: i18nc("@info", "Reloading discards pending sign-in changes."); wrapMode: Text.Wrap }
    }
    QQC2.Dialog {
        id: aliasDialog
        parent: root
        width: Math.min(root.width - 24, Kirigami.Units.gridUnit * 25)
        x: Math.max(0, (root.width - width) / 2)
        y: Math.max(0, (root.height - height) / 2)
        objectName: "brokerAliasDialog"
        property string route: "console"
        property bool existing: false
        title: existing ? i18nc("@title:window", "Edit Remote Login") : i18nc("@title:window", "Add Remote Login")
        modal: true
        onClosed: aliasPassword.text = ""
        contentItem: Kirigami.FormLayout {
            QQC2.TextField { id: aliasName; objectName: "brokerAliasName"; Kirigami.FormData.label: i18nc("@label", "Remote login:"); readOnly: aliasDialog.existing }
            QQC2.TextField { id: ownerName; objectName: "brokerAliasOwner"; Kirigami.FormData.label: i18nc("@label", "System account:") }
            QQC2.TextField { id: aliasPassword; objectName: "brokerAliasPassword"; Kirigami.FormData.label: i18nc("@label", "New password:"); echoMode: QQC2.TextField.Password }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: aliasDialog.existing
                text: i18nc("@info", "Leave blank to keep the password. Changing the account requires a new password.")
            }
            QQC2.Label { text: root.administration.error; visible: text !== ""; wrapMode: Text.Wrap }
        }
        footer: QQC2.DialogButtonBox {
            QQC2.Button {
                objectName: "stageBrokerAlias"
                text: i18nc("@action:button", "Keep Changes")
                enabled: aliasName.text !== "" && ownerName.text !== ""
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.ActionRole
                onClicked: {
                    if (root.administration.setAlias(aliasDialog.route, aliasName.text, ownerName.text, aliasPassword.text)) aliasDialog.close();
                }
            }
            QQC2.Button {
                text: i18nc("@action:button", "Cancel")
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.RejectRole
                onClicked: aliasDialog.close()
            }
        }
    }
}
