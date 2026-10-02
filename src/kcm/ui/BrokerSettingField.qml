// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root
    required property var settings
    required property var definition
    property string prefix: "host_"
    property bool accountPreference: false
    property bool editable: true
    property bool showHelp: false
    readonly property string key: definition ? definition.key : ""
    readonly property var choices: definition ? definition.choices : []
    readonly property bool overridden: Object.prototype.hasOwnProperty.call(settings.values, key)
    readonly property string savedValue: overridden ? settings.values[key] : ""
    readonly property string defaultValue: accountPreference ? "" : (settings.unitDefaults[key] || "")
    readonly property bool booleanField: choices.some(option => option.value === "true") && choices.some(option => option.value === "false")
    readonly property bool numericField: ["Quality", "Port", "MonitorIndex", "Avc444MotionGapMs", "Avc444RestMs", "Avc444MaxGapMs"].includes(key)
    readonly property int minimum: key === "Port" ? 1 : key.startsWith("Avc444") ? 16 : 0
    readonly property int maximum: key === "Quality" ? 100 : key.startsWith("Avc444") ? 5000 : 65535
    readonly property int numericValue: Math.max(minimum, Math.min(maximum, Number(overridden ? savedValue : defaultValue) || minimum))
    readonly property string inheritanceText: accountPreference ? i18nc("@option:check", "Use host setting") : i18nc("@option:check", "Use unit default")
    readonly property string label: {
        switch (key) {
        case "AdaptiveQuality": return i18nc("@label", "Automatic quality");
        case "VirtualMonitorPolicy": return i18nc("@label", "Physical displays");
        case "VirtualMonitorLayout": return i18nc("@label", "Client display layout");
        case "WakeDisplayOnConnect": return i18nc("@label", "Keep displays awake");
        case "Avc444MotionGapMs": return i18nc("@label", "AVC444 motion interval");
        case "Avc444RestMs": return i18nc("@label", "AVC444 rest interval");
        case "Avc444MaxGapMs": return i18nc("@label", "AVC444 maximum interval");
        default: return definition ? definition.label : "";
        }
    }
    Kirigami.FormData.label: label + ":"
    Kirigami.FormData.buddyFor: inputs
    spacing: Kirigami.Units.smallSpacing

    RowLayout {
        id: inputs
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing
        onActiveFocusChanged: if (activeFocus) {
            if (root.booleanField) booleanValue.forceActiveFocus();
            else if (root.numericField) numberValue.forceActiveFocus();
            else if (root.choices.length) choice.forceActiveFocus();
            else textValue.forceActiveFocus();
        }
        QQC2.CheckBox {
            id: booleanValue
            objectName: root.booleanField ? root.prefix + root.key : ""
            visible: root.booleanField
            enabled: root.editable
            tristate: true
            checkState: !root.overridden ? Qt.PartiallyChecked : root.savedValue === "true" ? Qt.Checked : Qt.Unchecked
            text: checkState === Qt.PartiallyChecked ? root.accountPreference ? root.inheritanceText : i18nc("@option:check", "Use unit default (%1)", root.defaultValue === "true" ? i18nc("@info", "On") : i18nc("@info", "Off")) : i18nc("@option:check", "Enabled")
            Accessible.name: root.definition ? root.definition.label : ""
            onClicked: {
                if (checkState === Qt.PartiallyChecked) root.settings.inherit(root.key);
                else root.settings.setValue(root.key, checkState === Qt.Checked ? "true" : "false");
                checkState = Qt.binding(() => !root.overridden ? Qt.PartiallyChecked : root.savedValue === "true" ? Qt.Checked : Qt.Unchecked);
            }
        }
        QQC2.Slider {
            objectName: root.key === "Quality" ? root.prefix + "QualitySlider" : ""
            visible: root.key === "Quality"
            enabled: root.editable && (!root.accountPreference || root.overridden)
            Layout.fillWidth: true
            Layout.minimumWidth: Kirigami.Units.gridUnit * 4
            Layout.maximumWidth: Kirigami.Units.gridUnit * 12
            from: 0; to: 100; stepSize: 1
            value: root.numericValue
            Accessible.name: root.definition ? root.definition.label : ""
            onMoved: { root.settings.setValue(root.key, String(Math.round(value))); value = Qt.binding(() => root.numericValue); }
        }
        QQC2.SpinBox {
            id: numberValue
            objectName: root.numericField ? root.prefix + root.key : ""
            visible: root.numericField
            enabled: root.editable && (!root.accountPreference || root.overridden)
            editable: true
            from: root.minimum; to: root.maximum
            value: root.numericValue
            textFromValue: (value, locale) => root.accountPreference && !root.overridden ? "—" : String(value)
            valueFromText: (text, locale) => Number(text)
            Accessible.name: root.definition ? root.definition.label : ""
            onValueModified: { root.settings.setValue(root.key, String(value)); value = Qt.binding(() => root.numericValue); }
        }
        QQC2.Label { visible: root.numericField && root.key.startsWith("Avc444"); text: i18nc("@label", "ms") }
        QQC2.CheckBox {
            objectName: root.numericField && root.accountPreference ? "inherit_" + root.key : ""
            visible: root.numericField && root.accountPreference
            text: root.inheritanceText
            enabled: root.editable
            checked: !root.overridden
            onClicked: {
                if (checked) root.settings.inherit(root.key);
                else root.settings.setValue(root.key, String(root.numericValue));
                checked = Qt.binding(() => !root.overridden);
            }
        }
        QQC2.ComboBox {
            id: choice
            objectName: root.choices.length && !root.booleanField ? root.prefix + root.key : ""
            visible: root.choices.length > 0 && !root.booleanField
            enabled: root.editable
            model: root.choices
            textRole: "text"; valueRole: "value"
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            implicitContentWidthPolicy: QQC2.ComboBox.WidestText
            Accessible.name: root.definition ? root.definition.label : ""
            currentIndex: { for (let i = 0; i < root.choices.length; ++i) if (root.choices[i].value === root.savedValue) return i; return 0; }
            onActivated: {
                if (currentValue === "") root.settings.inherit(root.key);
                else root.settings.setValue(root.key, currentValue);
            }
        }
        QQC2.TextField {
            id: textValue
            objectName: !root.choices.length && !root.numericField ? root.prefix + root.key : ""
            visible: !root.choices.length && !root.numericField
            enabled: root.editable
            Layout.fillWidth: true
            Layout.preferredWidth: Kirigami.Units.gridUnit * 18
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            Accessible.name: root.definition ? root.definition.label : ""
            text: root.savedValue
            placeholderText: root.accountPreference ? i18nc("@info:placeholder", "Use host setting") : root.defaultValue
            maximumLength: root.accountPreference ? 256 : 4096
            onTextEdited: {
                if (text === "" && root.key !== "RenderPci" && !root.key.startsWith("Certificate")) root.settings.inherit(root.key);
                else root.settings.setValue(root.key, text);
            }
        }
        // Keep space allocated so state changes never move the neighbouring input.
        QQC2.ToolButton {
            objectName: root.accountPreference ? "resetPreference_" + root.key : "inheritHost_" + root.key
            opacity: root.overridden && !root.key.startsWith("Certificate") && !root.booleanField ? 1 : 0
            enabled: root.editable && opacity > 0
            icon.name: "edit-undo"
            text: root.inheritanceText
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered && enabled
            Accessible.ignored: !enabled
            onClicked: root.settings.inherit(root.key)
        }
        Kirigami.ContextualHelpButton {
            visible: root.showHelp
            toolTipText: root.definition ? root.definition.help : ""
        }
        Item { Layout.fillWidth: root.booleanField || root.numericField && root.key !== "Quality" }
    }
}
