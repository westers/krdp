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
    property var preferences: kcm.brokerPreferences
    property bool showAdvanced: false
    property bool scrollToDisplays: false
    readonly property string monitorMode: preferences.values.MonitorMode || ""
    title: i18nc("@title:window", "My Preferences")
    function fields(keys) { return preferences.definitions.filter(row => keys.includes(row.key)); }
    Component.onCompleted: if (scrollToDisplays) showDisplays()
    function showDisplays() { Qt.callLater(() => { if (preferences.loaded) flickable.contentY = Math.min(displaySection.y, Math.max(0, flickable.contentHeight - flickable.height)); }); }
    ColumnLayout {
      ColumnLayout {
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 48
        Layout.alignment: Qt.AlignLeft
        spacing: Kirigami.Units.largeSpacing
        QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "For your account. Video and sound apply to Console and Virtual; display selection applies to Console. Save, then reconnect.") }
        QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Kirigami.Theme.disabledTextColor; text: i18nc("@info", "Unset preferences use host settings. Console and Virtual may have different defaults. Host permissions and administrator locks take precedence.") }
        Kirigami.InlineMessage { objectName: "brokerPreferenceError"; Layout.fillWidth: true; visible: root.preferences.error !== ""; type: Kirigami.MessageType.Error; text: root.preferences.error }
        Kirigami.InlineMessage { objectName: "brokerPreferenceReconnect"; Layout.fillWidth: true; visible: root.preferences.reconnectRequired; type: Kirigami.MessageType.Information; text: i18nc("@info", "Preferences saved. Reconnect to use them; current connections are unchanged.") }
        ColumnLayout {
            visible: root.preferences.loaded; Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
            Kirigami.Heading { level: 2; text: i18nc("@title:group", "Video") }
            Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 32; id: videoForm; twinFormLayouts: [displayForm, mediaForm, advancedForm]; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft; Repeater { model: root.fields(["Quality", "AdaptiveQuality", "Codec"]); delegate: preferenceField } }
        }
        ColumnLayout {
            id: displaySection
            visible: root.preferences.loaded; Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
            Kirigami.Heading { level: 2; text: i18nc("@title:group", "Console displays") }
            Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 32; id: displayForm; twinFormLayouts: [videoForm, mediaForm, advancedForm]; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft; Repeater { model: root.fields(["MonitorMode", "MonitorIndex", "VirtualMonitorPolicy", "VirtualMonitorLayout", "VirtualMonitorFallbackSize"]); delegate: preferenceField } }
            QQC2.Label { visible: root.monitorMode === ""; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Kirigami.Theme.disabledTextColor; text: i18nc("@info", "Display mode uses the host setting. Choose a sharing mode to customize its options; inactive values are preserved.") }
            QQC2.Label { visible: root.monitorMode === "virtual"; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Kirigami.Theme.disabledTextColor; text: i18nc("@info", "Client-created displays belong to Console, not a separate Virtual session. Turning off physical displays lasts for the connection; local reclaim restores them. Fallback size is used when client monitor data is unavailable.") }
        }
        ColumnLayout {
            visible: root.preferences.loaded; Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
            Kirigami.Heading { level: 2; text: i18nc("@title:group", "Sound and session") }
            Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 32; id: mediaForm; twinFormLayouts: [videoForm, displayForm, advancedForm]; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft; Repeater { model: root.fields(["PreferAudioQuality", "StandardClientMedia", "WakeDisplayOnConnect"]); delegate: preferenceField } }
            QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Kirigami.Theme.disabledTextColor; text: i18nc("@info", "Media still requires host permission and client consent. Keeping displays awake never unlocks the screen.") }
        }
        QQC2.Button { objectName: "preferenceAdvancedButton"; visible: root.preferences.loaded; text: i18nc("@action:button", "Advanced / Compatibility"); icon.name: root.showAdvanced ? "arrow-down" : "arrow-right"; onClicked: root.showAdvanced = !root.showAdvanced }
        ColumnLayout {
            visible: root.preferences.loaded && root.showAdvanced; Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
            Kirigami.Heading { level: 2; text: i18nc("@title:group", "Encoding and compatibility") }
            Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 32; id: advancedForm; twinFormLayouts: [videoForm, displayForm, mediaForm]; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft; Repeater { model: root.fields(["SoftwareEncoding", "Av1Tiles", "Avc444MotionGapMs", "Avc444RestMs", "Avc444MaxGapMs", "VirtualStockClientPolicy"]); delegate: preferenceField } }
            QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "AVC444 timing must satisfy motion ≤ rest ≤ maximum. If any timing override is set, unset timing fields use built-in values."); color: Kirigami.Theme.disabledTextColor }
        }
        QQC2.Label { visible: root.preferences.modified; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Unsaved changes · your account only."); color: Kirigami.Theme.disabledTextColor }
    }
    Component {
        id: preferenceField
        BrokerSettingField {
            id: field
            required property var modelData
            settings: root.preferences; definition: modelData; prefix: "preference_"; accountPreference: true
            visible: key === "MonitorIndex" ? root.monitorMode === "specific" : ["VirtualMonitorPolicy", "VirtualMonitorLayout", "VirtualMonitorFallbackSize"].includes(key) ? root.monitorMode === "virtual" : true
            editable: !root.preferences.lockedKeys.includes(key)
            showHelp: ["SoftwareEncoding", "Av1Tiles", "Avc444MotionGapMs", "Avc444RestMs", "Avc444MaxGapMs", "VirtualStockClientPolicy"].includes(key) || !field.editable
            QQC2.Label { visible: !field.editable; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Locked by the administrator"); color: Kirigami.Theme.disabledTextColor }
        }
    }
    }
    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            QQC2.Button { objectName: "defaultBrokerPreferences"; text: i18nc("@action:button", "Use Host Settings"); enabled: root.preferences.loaded; onClicked: root.preferences.defaults(); QQC2.ToolTip.text: i18nc("@info:tooltip", "Stage removal of editable overrides; administrator locks are preserved."); QQC2.ToolTip.visible: hovered }
            QQC2.Button { objectName: "discardBrokerPreferences"; text: i18nc("@action:button", "Revert Changes"); enabled: root.preferences.modified; onClicked: root.preferences.discard() }
            QQC2.ToolButton { objectName: "loadBrokerPreferences"; icon.name: "view-refresh"; text: i18nc("@action:button", "Reload Preferences…"); display: QQC2.AbstractButton.IconOnly; QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered; onClicked: { if (root.preferences.modified) reloadConfirmation.open(); else root.preferences.reload(); } }
            Item { Layout.fillWidth: true }
            QQC2.Button { objectName: "saveBrokerPreferences"; highlighted: true; text: i18nc("@action:button", "Save Preferences"); enabled: root.preferences.canSave; onClicked: root.preferences.save() }
        }
    }
    Kirigami.PromptDialog {
        parent: root.QQC2.Overlay.overlay
        popupType: QQC2.Popup.Item
        y: parent ? Math.round((parent.height - implicitHeight) / 2) : 0
        id: reloadConfirmation; objectName: "reloadBrokerPreferences"
        title: i18nc("@title:window", "Reload Preferences?")
        subtitle: i18nc("@info", "Discard your unsaved preference changes and reload saved values?")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: Kirigami.Action { objectName: "reloadPreferenceAccept"; text: i18nc("@action:button", "Discard and Reload"); onTriggered: { reloadConfirmation.close(); root.preferences.reload(); } }
    }
}
