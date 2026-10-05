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
    title: i18nc("@title:window", "My Preferences")
    Component.onCompleted: if (scrollToDisplays) showDisplays()
    function showDisplays() { Qt.callLater(() => { if (preferences.loaded) flickable.contentY = Math.min(displaySection.mapToItem(flickable.contentItem, 0, 0).y, Math.max(0, flickable.contentHeight - flickable.height)); }); }
    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        BrokerApplyFailures { }
        Kirigami.InlineMessage { objectName: "brokerPreferenceError"; Layout.fillWidth: true; visible: root.preferences.error !== ""; type: Kirigami.MessageType.Error; text: root.preferences.error }
        Kirigami.InlineMessage { objectName: "brokerPreferenceReconnect"; Layout.fillWidth: true; visible: root.preferences.reconnectRequired; type: Kirigami.MessageType.Information; text: i18nc("@info", "Saved. Reconnect to use your preferences. Connections that are already open keep their current settings.") }
        Kirigami.FormLayout {
            id: form
            // The form decides side by side or stacked itself (the rows' small hints below no longer drive it), and takes the
            // pane width up to 44 grid units; rows then fill what is left of it, up to 24 grid units.
            wideMode: width < 1 || width >= Kirigami.Units.gridUnit * 20
            implicitWidth: Math.min(parent ? parent.width : 0, Kirigami.Units.gridUnit * 44)
            Layout.fillWidth: true
            visible: root.preferences.loaded
            Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: root.preferences.sectionTitle("video") }
            BrokerFieldRepeater { settings: root.preferences; section: "video"; prefix: "preference_"; accountPreference: true; lockedKeys: root.preferences.lockedKeys }
            Kirigami.Separator { id: displaySection; Kirigami.FormData.isSection: true; Kirigami.FormData.label: root.preferences.sectionTitle("displays") }
            BrokerFieldRepeater { settings: root.preferences; section: "displays"; prefix: "preference_"; accountPreference: true; lockedKeys: root.preferences.lockedKeys }
            // The display choices that follow the mode appear only for two of the five modes; say so instead of leaving one lone control.
            QQC2.Label {
                objectName: "displayModeHint"
                visible: ["specific", "virtual"].indexOf(root.preferences.values.MonitorMode || "") < 0
                Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                Layout.minimumWidth: Kirigami.Units.gridUnit * 7; Layout.preferredWidth: Kirigami.Units.gridUnit * 11
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                text: i18nc("@info", "More options appear here when you choose “One display” or “Client-created displays”.")
            }
            Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: root.preferences.sectionTitle("sound") }
            BrokerFieldRepeater { settings: root.preferences; section: "sound"; prefix: "preference_"; accountPreference: true; lockedKeys: root.preferences.lockedKeys }
            QQC2.Button {
                objectName: "preferenceAdvancedButton"
                flat: true
                text: i18nc("@action:button", "Advanced options")
                icon.name: root.showAdvanced ? "arrow-down" : "arrow-right"
                onClicked: root.showAdvanced = !root.showAdvanced
            }
            Kirigami.Separator { visible: root.showAdvanced; Kirigami.FormData.isSection: true; Kirigami.FormData.label: root.preferences.sectionTitle("advanced") }
            BrokerFieldRepeater { settings: root.preferences; advanced: true; shown: root.showAdvanced; prefix: "preference_"; accountPreference: true; lockedKeys: root.preferences.lockedKeys }
        }
    }
}
