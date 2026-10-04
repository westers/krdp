// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM
// Your own choices, applied to your account when you connect. "Use host setting"
// follows whatever the service is configured with.
KCM.SimpleKCM {
    id: root
    objectName: "brokerPreferencesPage"
    property var preferences: kcm.brokerPreferences
    property bool showAdvanced: false
    property bool scrollToDisplays: false
    readonly property string monitorMode: preferences.values.MonitorMode || ""
    title: i18nc("@title:window", "My Preferences")
    function fields(keys) { return preferences.definitions.filter(row => keys.includes(row.key)); }
    Component.onCompleted: if (scrollToDisplays) showDisplays()
    function showDisplays() { Qt.callLater(() => { if (preferences.loaded) flickable.contentY = Math.min(displaySection.mapToItem(flickable.contentItem, 0, 0).y, Math.max(0, flickable.contentHeight - flickable.height)); }); }
    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        BrokerApplyFailures { }
        Kirigami.InlineMessage { objectName: "brokerPreferenceError"; Layout.fillWidth: true; visible: root.preferences.error !== ""; type: Kirigami.MessageType.Error; text: root.preferences.error }
        Kirigami.InlineMessage { objectName: "brokerPreferenceReconnect"; Layout.fillWidth: true; visible: root.preferences.reconnectRequired; type: Kirigami.MessageType.Information; text: i18nc("@info", "Saved. Reconnect to use your preferences; current connections are unchanged.") }
        Kirigami.FormLayout {
            id: form
            Layout.fillWidth: true
            visible: root.preferences.loaded
            Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Video") }
            Repeater { model: root.fields(["Quality", "AdaptiveQuality", "Codec"]); delegate: preferenceField }
            Kirigami.Separator { id: displaySection; Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Console displays") }
            Repeater { model: root.fields(["MonitorMode", "MonitorIndex", "VirtualMonitorPolicy", "VirtualMonitorLayout", "VirtualMonitorFallbackSize"]); delegate: preferenceField }
            Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Sound and session") }
            Repeater { model: root.fields(["PreferAudioQuality", "StandardClientMedia", "WakeDisplayOnConnect"]); delegate: preferenceField }
            QQC2.Button {
                objectName: "preferenceAdvancedButton"
                flat: true
                text: i18nc("@action:button", "Advanced options")
                icon.name: root.showAdvanced ? "arrow-down" : "arrow-right"
                onClicked: root.showAdvanced = !root.showAdvanced
            }
            Kirigami.Separator { visible: root.showAdvanced; Kirigami.FormData.isSection: true; Kirigami.FormData.label: i18nc("@title:group", "Encoding and compatibility") }
            Repeater { model: root.fields(["SoftwareEncoding", "Av1Tiles", "Avc444MotionGapMs", "Avc444RestMs", "Avc444MaxGapMs", "VirtualStockClientPolicy"]); delegate: advancedPreferenceField }
        }
    }
    Component {
        id: advancedPreferenceField
        BrokerSettingField {
            id: advancedField
            required property var modelData
            settings: root.preferences; definition: modelData; prefix: "preference_"; accountPreference: true
            visible: root.showAdvanced
            editable: !root.preferences.lockedKeys.includes(key)
            QQC2.Label { visible: !advancedField.editable; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Locked by the administrator"); font: Kirigami.Theme.smallFont }
        }
    }
    Component {
        id: preferenceField
        BrokerSettingField {
            id: field
            required property var modelData
            settings: root.preferences; definition: modelData; prefix: "preference_"; accountPreference: true
            visible: key === "MonitorIndex" ? root.monitorMode === "specific" : ["VirtualMonitorPolicy", "VirtualMonitorLayout", "VirtualMonitorFallbackSize"].includes(key) ? root.monitorMode === "virtual" : true
            editable: !root.preferences.lockedKeys.includes(key)
            QQC2.Label { visible: !field.editable; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Locked by the administrator"); font: Kirigami.Theme.smallFont }
        }
    }
}
