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
            text: i18nc("@info", "These preferences belong to your system account. Video and audio apply to Console and Virtual; display selection applies to Console. Host permissions and client support still determine what is available. Save, then reconnect to apply changes.")
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
        RowLayout {
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
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: i18nc("@info", "Use host setting leaves that preference unset. Console and Virtual can have different host defaults. A locked setting is preserved. AVC444 timing uses the built-in values for individual unset timing fields when any timing override is present.")
        }
        Repeater {
            model: root.preferences.loaded ? root.preferences.definitions : []
            delegate: ColumnLayout {
                id: row
                required property var modelData
                required property int index
                readonly property string key: modelData.key
                readonly property bool locked: root.preferences.lockedKeys.includes(key)
                readonly property bool overridden: Object.prototype.hasOwnProperty.call(root.preferences.values, key)
                readonly property string value: overridden ? root.preferences.values[key] : ""
                readonly property var choices: modelData.choices
                Layout.fillWidth: true
                Kirigami.Heading {
                    level: 3
                    visible: row.index === 0 || root.preferences.definitions[row.index - 1].group !== row.modelData.group
                    text: row.modelData.group
                }
                QQC2.Label { text: row.modelData.label }
                QQC2.ComboBox {
                    objectName: row.choices.length > 0 ? "preference_" + row.key : ""
                    visible: row.choices.length > 0
                    enabled: !row.locked
                    model: row.choices
                    textRole: "text"
                    valueRole: "value"
                    implicitContentWidthPolicy: QQC2.ComboBox.WidestText
                    Layout.maximumWidth: root.width - Kirigami.Units.largeSpacing * 2
                    currentIndex: {
                        for (let i = 0; i < row.choices.length; ++i) if (row.choices[i].value === row.value) return i;
                        return 0;
                    }
                    onActivated: {
                        if (currentValue === "") root.preferences.inherit(row.key);
                        else root.preferences.setValue(row.key, currentValue);
                    }
                }
                RowLayout {
                    visible: row.choices.length === 0
                    enabled: !row.locked
                    QQC2.TextField {
                        id: textValue
                        objectName: row.choices.length === 0 ? "preference_" + row.key : ""
                        text: row.value
                        placeholderText: i18nc("@info:placeholder", "Use host setting")
                        maximumLength: 256
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 16
                        onTextEdited: {
                            if (text === "") root.preferences.inherit(row.key);
                            else root.preferences.setValue(row.key, text);
                        }
                    }
                    QQC2.ToolButton {
                        objectName: "inherit_" + row.key
                        icon.name: "edit-undo"
                        text: i18nc("@action:button", "Use host setting")
                        display: QQC2.AbstractButton.IconOnly
                        enabled: row.overridden
                        QQC2.ToolTip.text: text
                        QQC2.ToolTip.visible: hovered
                        onClicked: root.preferences.inherit(row.key)
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: row.modelData.help + (row.locked ? " " + i18nc("@info", "This setting is locked.") : "")
                }
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
