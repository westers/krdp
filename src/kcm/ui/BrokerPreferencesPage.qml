// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

KCM.SimpleKCM {
    id: root
    objectName: "brokerPreferencesPage"
    title: i18nc("@title:window", "Your Console and Virtual Preferences")
    property var preferences: kcm.brokerPreferences
    // Explicit load keeps plugin construction and UI-load tests from reading
    // the work desktop's real configuration.
    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: i18nc("@info", "Set preferences for this account. Save, then reconnect to apply them.")
        }
        Kirigami.InlineMessage {
            objectName: "brokerPreferenceError"
            Layout.fillWidth: true
            visible: root.preferences.error !== ""
            type: Kirigami.MessageType.Error
            text: root.preferences.error
        }
        Kirigami.InlineMessage {
            objectName: "brokerPreferenceReconnect"
            Layout.fillWidth: true
            visible: root.preferences.reconnectRequired
            type: Kirigami.MessageType.Information
            text: i18nc("@info", "Preferences saved. Reconnect to use them; current connections are unchanged.")
        }
        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            QQC2.Button {
                objectName: "loadBrokerPreferences"
                text: root.preferences.loaded ? i18nc("@action:button", "Reload Preferences…") : i18nc("@action:button", "Load Preferences")
                icon.name: "view-refresh"
                onClicked: {
                    if (root.preferences.modified) discardDialog.open();
                    else root.preferences.reload();
                }
            }
            QQC2.Button {
                objectName: "saveBrokerPreferences"
                text: i18nc("@action:button", "Save Preferences")
                icon.name: "document-save"
                enabled: root.preferences.canSave
                onClicked: root.preferences.save()
            }
            QQC2.Button {
                objectName: "defaultBrokerPreferences"
                text: i18nc("@action:button", "Use Host Settings")
                enabled: root.preferences.loaded
                onClicked: root.preferences.defaults()
            }
        }
        Repeater {
            model: root.preferences.loaded ? root.preferences.definitions.map(row => row.group).filter((group, index, groups) => groups.indexOf(group) === index) : []
            delegate: Kirigami.FormLayout {
                id: group
                required property string modelData
                Layout.fillWidth: true
                Item { Kirigami.FormData.isSection: true; Kirigami.FormData.label: group.modelData }
                Repeater {
                    model: root.preferences.definitions.filter(row => row.group === group.modelData)
                    delegate: RowLayout {
                        id: row
                        required property var modelData
                        readonly property string key: modelData.key
                        readonly property bool locked: root.preferences.lockedKeys.includes(key)
                        readonly property bool overridden: Object.prototype.hasOwnProperty.call(root.preferences.values, key)
                        readonly property string value: overridden ? root.preferences.values[key] : ""
                        readonly property var choices: modelData.choices
                        Kirigami.FormData.label: row.modelData.label + ":"
                        Kirigami.FormData.buddyFor: row.choices.length > 0 ? choice : textValue
                        QQC2.ComboBox {
                            id: choice
                            objectName: row.choices.length > 0 ? "preference_" + row.key : ""
                            visible: row.choices.length > 0
                            enabled: !row.locked
                            model: row.choices
                            textRole: "text"
                            valueRole: "value"
                            Layout.fillWidth: true
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 25
                            implicitContentWidthPolicy: QQC2.ComboBox.WidestText
                            currentIndex: {
                                for (let i = 0; i < row.choices.length; ++i) if (row.choices[i].value === row.value) return i;
                                return 0;
                            }
                            onActivated: {
                                if (currentValue === "") root.preferences.inherit(row.key);
                                else root.preferences.setValue(row.key, currentValue);
                            }
                        }
                        QQC2.TextField {
                            id: textValue
                            objectName: row.choices.length === 0 ? "preference_" + row.key : ""
                            visible: row.choices.length === 0
                            enabled: !row.locked
                            Accessible.name: row.modelData.label
                            text: row.value
                            placeholderText: i18nc("@info:placeholder", "Use host setting")
                            maximumLength: 256
                            Layout.fillWidth: true
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 18
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 25
                            onTextEdited: {
                                if (text === "") root.preferences.inherit(row.key);
                                else root.preferences.setValue(row.key, text);
                            }
                        }
                        QQC2.ToolButton {
                            objectName: "inherit_" + row.key
                            visible: row.choices.length === 0
                            icon.name: "edit-undo"
                            text: i18nc("@action:button", "Use Host Setting")
                            display: QQC2.AbstractButton.IconOnly
                            enabled: !row.locked && row.overridden
                            QQC2.ToolTip.text: text
                            QQC2.ToolTip.visible: hovered
                            onClicked: root.preferences.inherit(row.key)
                        }
                        Kirigami.ContextualHelpButton {
                            toolTipText: row.modelData.help + (row.locked ? " " + i18nc("@info", "This setting is locked by the administrator.") : "")
                        }
                    }
                }
            }
        }
        RowLayout {
            QQC2.Label { text: i18nc("@info", "Unset preferences use the host's settings."); color: Kirigami.Theme.disabledTextColor }
            Kirigami.ContextualHelpButton {
                toolTipText: i18nc("@info:tooltip", "Video and audio preferences apply to Console and Virtual; display selection applies to Console. Host permissions and client support determine availability. Console and Virtual may have different defaults. Locked settings are preserved. Unset AVC444 timing fields use built-in values when any timing override is present.")
            }
        }

    }
    QQC2.Dialog {
        id: discardDialog
        objectName: "discardBrokerPreferences"
        modal: true
        width: Math.min(root.width - Kirigami.Units.largeSpacing * 2, Kirigami.Units.gridUnit * 26)
        x: Math.max(0, (root.width - width) / 2)
        y: Math.max(0, (root.height - height) / 2)
        title: i18nc("@title:window", "Discard Preference Changes")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        contentItem: QQC2.Label { wrapMode: Text.Wrap; text: i18nc("@info", "Reloading discards your unsaved changes. Continue?") }
        onAccepted: root.preferences.reload()
    }
}
