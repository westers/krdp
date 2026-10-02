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
    property bool showAdvanced: false
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
        QQC2.Button {
            objectName: "unlockBrokerPreferences"
            visible: !root.preferences.loaded
            text: i18nc("@action:button", "Load My Preferences")
            onClicked: root.preferences.reload()
        }
        Kirigami.FormLayout {
            Layout.fillWidth: true
            visible: root.preferences.loaded
            Item { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Video") }
            Repeater { model: root.preferences.definitions.filter(row => ["Quality", "AdaptiveQuality", "Codec"].includes(row.key)); delegate: preferenceField }
            Item { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Console displays") }
            Repeater { model: root.preferences.definitions.filter(row => ["MonitorMode", "MonitorIndex", "VirtualMonitorPolicy", "VirtualMonitorLayout", "VirtualMonitorFallbackSize"].includes(row.key)); delegate: preferenceField }
            Item { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Audio and session") }
            Repeater { model: root.preferences.definitions.filter(row => ["PreferAudioQuality", "StandardClientMedia", "WakeDisplayOnConnect"].includes(row.key)); delegate: preferenceField }
            QQC2.Button {
                objectName: "preferenceAdvancedButton"
                text: root.showAdvanced ? i18nc("@action:button", "Hide advanced options") : i18nc("@action:button", "Advanced options")
                icon.name: root.showAdvanced ? "arrow-down" : "arrow-right"
                flat: true
                onClicked: root.showAdvanced = !root.showAdvanced
            }
            Item { visible: root.showAdvanced; Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Encoding and compatibility") }
            Repeater { model: root.preferences.definitions.filter(row => ["SoftwareEncoding", "Av1Tiles", "Avc444MotionGapMs", "Avc444RestMs", "Avc444MaxGapMs", "VirtualStockClientPolicy"].includes(row.key)); delegate: preferenceField }
        }
        RowLayout {
            QQC2.Label { text: i18nc("@info", "Unset preferences use the host's settings."); color: Kirigami.Theme.disabledTextColor }
            Kirigami.ContextualHelpButton {
                toolTipText: i18nc("@info:tooltip", "Video and audio preferences apply to Console and Virtual; display selection applies to Console. Host permissions and client support determine availability. Console and Virtual may have different defaults. Locked settings are preserved. Unset AVC444 timing fields use built-in values when any timing override is present.")
            }
        }

    }
    Component {
        id: preferenceField
        BrokerSettingField {
            id: field
            required property var modelData
            readonly property bool advanced: ["SoftwareEncoding", "Av1Tiles", "Avc444MotionGapMs", "Avc444RestMs", "Avc444MaxGapMs", "VirtualStockClientPolicy"].includes(modelData.key)
            settings: root.preferences
            definition: modelData
            prefix: "preference_"
            accountPreference: true
            visible: !advanced || root.showAdvanced
            editable: !root.preferences.lockedKeys.includes(key)
            showHelp: advanced || !editable || ["VirtualMonitorPolicy", "VirtualMonitorLayout", "WakeDisplayOnConnect"].includes(key)
            QQC2.Label {
                Layout.fillWidth: true
                visible: !field.editable
                text: i18nc("@info", "Locked by the administrator")
                color: Kirigami.Theme.disabledTextColor
            }
        }
    }
    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            QQC2.Button { objectName: "defaultBrokerPreferences"; text: i18nc("@action:button", "Use Host Settings"); enabled: root.preferences.loaded; onClicked: root.preferences.defaults() }
            QQC2.ToolButton {
                objectName: "loadBrokerPreferences"
                icon.name: "view-refresh"; text: i18nc("@action:button", "Reload Preferences…")
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered
                onClicked: { if (root.preferences.modified) discardDialog.open(); else root.preferences.reload(); }
            }
            Item { Layout.fillWidth: true }
            QQC2.Button { objectName: "saveBrokerPreferences"; text: i18nc("@action:button", "Save Preferences"); icon.name: "document-save"; enabled: root.preferences.canSave; onClicked: root.preferences.save() }
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
