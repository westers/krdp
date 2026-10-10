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
    property bool advancedChoices: false
    property var lockedKeys: []
    // The definition's help text sits behind a (?) button next to the control, never as a grey paragraph.
    property bool showHelp: true
    readonly property string key: definition.key
    readonly property bool locked: lockedKeys.includes(key)
    readonly property bool editable: !busy && !locked
    readonly property bool available: definition.unavailable === ""
        && (definition.showWhenKey === "" || (settings.values[definition.showWhenKey] || "") === definition.showWhenValue)
    // OPT-062 S3: a software-encoding row says so when the host's encoder probe found no software encoder for its codec.
    readonly property string softwareNote: {
        const probe = settings.metadata ? settings.metadata.videoEncoders : undefined;
        if (!definition.softwareCodec || !probe || probe.length === 0) return "";
        return probe.some(e => e.codec === definition.softwareCodec && !e.hw) ? "" : i18nc("@info", "No software encoder for this codec was found on this computer, so this setting has no effect.");
    }
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
    Layout.fillWidth: true
    // Size hints. Where the form has room for a label and a control side by side (about the detail pane beside both
    // sidebars, or wider) the hint is small (also before the first layout pass: FormLayout remembers the implicit width of that pass), because Kirigami's FormLayout only goes wide when label plus the widest
    // hint fit. In a narrower pane the labels stack above full-width controls instead.
    readonly property Item form: { let item = parent; while (item && item.wideMode === undefined) item = item.parent; return item; }
    readonly property bool sideBySide: !form || form.width < 1 || form.width >= Kirigami.Units.gridUnit * 20
    Layout.minimumWidth: Kirigami.Units.gridUnit * 7
    Layout.preferredWidth: Kirigami.Units.gridUnit * (sideBySide ? 11 : 24)
    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
    spacing: Kirigami.Units.smallSpacing

    BrokerFieldState {
        id: fieldState
        settings: root.settings
        definition: root.definition
        prefix: root.prefix
        accountPreference: root.accountPreference
        editable: root.editable
        advancedChoices: root.advancedChoices
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
        QQC2.Label { objectName: root.prefix + "note_" + root.key; visible: root.softwareNote !== ""; Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.softwareNote; font: Kirigami.Theme.smallFont }
        QQC2.Label { visible: root.locked; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Locked by the administrator"); font: Kirigami.Theme.smallFont }
    }
    Kirigami.ContextualHelpButton {
        objectName: root.prefix + "help_" + root.key
        Layout.alignment: Qt.AlignTop
        visible: root.showHelp && root.definition.help !== ""
        toolTipText: root.definition.help
    }
}
