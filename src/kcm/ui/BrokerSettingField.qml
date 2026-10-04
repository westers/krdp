// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// One form row, drawn from a field definition (key, control, bounds, labels, help; see settingfielddefinition.h).
// The definition decides which control appears; this file knows no keys.
RowLayout {
    id: root
    required property var settings
    required property var definition
    property string prefix: "host_"
    property bool accountPreference: false
    // The page's own switch (for example Advanced collapsed); the definition's rules apply on top.
    property bool shown: true
    property bool busy: false
    property var lockedKeys: []
    // The definition's help text sits behind a (?) button next to the control, never as a grey paragraph.
    property bool showHelp: true
    readonly property string key: definition.key
    readonly property bool locked: lockedKeys.includes(key)
    readonly property bool editable: !busy && !locked
    readonly property bool available: definition.unavailable === ""
        && (definition.showWhenKey === "" || (settings.values[definition.showWhenKey] || "") === definition.showWhenValue)
    readonly property bool numeric: definition.control === "spin" || definition.control === "slider" || definition.control === "size"
    readonly property Component control: {
        switch (definition.control) {
        case "spin": case "slider": return numberControl;
        case "size": return sizeControl;
        case "choice": return choiceControl;
        case "address": return addressControl;
        default: return textControl;
        }
    }
    // The label's buddy must be a direct child, so it is the inner column; focus goes on to the field's main control.
    readonly property Item primary: inherit.visible ? inherit : fieldState.primary
    objectName: prefix + "field_" + key
    visible: shown && available
    Kirigami.FormData.label: fieldState.label + ":"
    Kirigami.FormData.buddyFor: column
    Layout.preferredWidth: Kirigami.Units.gridUnit * 24
    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
    spacing: Kirigami.Units.smallSpacing

    BrokerFieldState {
        id: fieldState
        settings: root.settings
        definition: root.definition
        prefix: root.prefix
        accountPreference: root.accountPreference
        editable: root.editable
    }
    Component { id: numberControl; BrokerNumberControl { field: fieldState } }
    Component { id: sizeControl; BrokerSizeControl { field: fieldState } }
    Component { id: choiceControl; BrokerChoiceControl { field: fieldState } }
    Component { id: addressControl; BrokerAddressControl { field: fieldState } }
    Component { id: textControl; BrokerTextControl { field: fieldState } }

    ColumnLayout {
        id: column
        Layout.fillWidth: true
        onActiveFocusChanged: if (activeFocus && root.primary) root.primary.forceActiveFocus()
        spacing: Kirigami.Units.smallSpacing
        BrokerInheritSelector { id: inherit; field: fieldState; visible: root.accountPreference && root.numeric }
        Loader {
            id: editor
            sourceComponent: root.control
            visible: fieldState.controlShown
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        }
        QQC2.Label { visible: root.locked; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Locked by the administrator"); font: Kirigami.Theme.smallFont }
    }
    Kirigami.ContextualHelpButton {
        objectName: root.prefix + "help_" + root.key
        Layout.alignment: Qt.AlignTop
        visible: root.showHelp && root.definition.help !== ""
        toolTipText: root.definition.help
    }
}
