// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
// Who may sign in to one route, and its remote logins. Edits go to the access-policy
// draft owned by the Who Can Connect page, which also hosts the add/edit dialog.
Kirigami.FormLayout {
    id: root
    wideMode: width < 1 || width >= Kirigami.Units.gridUnit * 20
    implicitWidth: Math.min(parent ? parent.width : 0, Kirigami.Units.gridUnit * 44)
    required property var page
    required property string route
    readonly property var admin: page.administration
    readonly property var policy: admin.policy[route] || ({})
    readonly property var pam: policy.pam || ({mode: "disabled", accounts: []})
    readonly property var credentials: policy.credentials || []
    Kirigami.Separator {
        Kirigami.FormData.isSection: true
        Kirigami.FormData.label: root.route === "console" ? i18nc("@title:group", "Console") : i18nc("@title:group", "Virtual")
    }
    RowLayout {
        Kirigami.FormData.label: i18nc("@label", "Who can sign in:")
        Kirigami.FormData.buddyFor: mode
        QQC2.ComboBox {
            id: mode
            objectName: root.route + "PamMode"
            implicitContentWidthPolicy: QQC2.ComboBox.WidestText
            Layout.minimumWidth: Kirigami.Units.gridUnit * 7; Layout.preferredWidth: Kirigami.Units.gridUnit * 11
            textRole: "text"
            valueRole: "value"
            model: [
                {text: i18nc("@item:inlistbox", "Everyone with an account"), value: "any"},
                {text: i18nc("@item:inlistbox", "Selected accounts"), value: "allow-list"},
                {text: i18nc("@item:inlistbox", "Nobody"), value: "disabled"}
            ]
            currentIndex: root.pam.mode === "any" ? 0 : root.pam.mode === "allow-list" ? 1 : 2
            onActivated: root.admin.setPam(root.route, currentValue, currentValue === "allow-list" ? root.pam.accounts : [])
        }
        Kirigami.ContextualHelpButton {
            toolTipText: root.route === "console"
                ? i18nc("@info:tooltip", "Passwords are checked the same way as when you sign in at this computer. In Console, only the person signed in at the computer can control it.")
                : i18nc("@info:tooltip", "Passwords are checked the same way as when you sign in at this computer. Each Virtual desktop belongs to the account that signed in.")
        }
    }
    QQC2.TextField {
        objectName: root.route + "PamAccounts"
        Kirigami.FormData.label: i18nc("@label", "Allowed accounts:")
        Layout.minimumWidth: Kirigami.Units.gridUnit * 7; Layout.preferredWidth: Kirigami.Units.gridUnit * 11
        visible: root.pam.mode === "allow-list"
        text: root.pam.accounts.join(", ")
        placeholderText: i18nc("@info:placeholder", "Login names, separated by commas")
        onEditingFinished: root.admin.setPam(root.route, "allow-list", text.split(",").map(value => value.trim()).filter(value => value !== ""))
    }
    ColumnLayout {
        Kirigami.FormData.label: i18nc("@label", "Remote logins:")
        Kirigami.FormData.labelAlignment: Qt.AlignTop
        spacing: Kirigami.Units.smallSpacing
        QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; font: Kirigami.Theme.smallFont; text: i18nc("@info", "Extra sign-in names with their own password.") }
        QQC2.Label { visible: root.credentials.length === 0; text: i18nc("@info", "None added") }
        Repeater {
            model: root.credentials
            delegate: RowLayout {
                id: login
                required property var modelData
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                QQC2.Label { Layout.fillWidth: true; text: i18nc("@info %1 remote login %2 desktop owner", "%1 → %2", login.modelData.alias, login.modelData.owner); elide: Text.ElideRight }
                QQC2.ToolButton {
                    objectName: root.route + "EditAlias_" + login.modelData.alias
                    icon.name: "document-edit"; text: i18nc("@action:button", "Edit…"); display: QQC2.AbstractButton.IconOnly
                    QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered
                    onClicked: root.page.editAlias(root.route, login.modelData.alias, login.modelData.owner)
                }
                QQC2.ToolButton {
                    objectName: root.route + "RemoveAlias_" + login.modelData.alias
                    icon.name: "edit-delete"; text: i18nc("@action:button", "Remove"); display: QQC2.AbstractButton.IconOnly
                    QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered
                    onClicked: { if (root.admin.removeAlias(root.route, login.modelData.alias)) root.page.removedPending = true; }
                }
            }
        }
        RowLayout {
            QQC2.Button {
                objectName: root.route + "AddAlias"
                text: i18nc("@action:button", "Add Remote Login…")
                icon.name: "list-add"
                onClicked: root.page.editAlias(root.route, "", "")
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18nc("@info:tooltip", "A remote login is an extra sign-in name with its own password. It signs in as the account you choose. The name itself never decides whose desktop opens.")
            }
        }
    }
}
