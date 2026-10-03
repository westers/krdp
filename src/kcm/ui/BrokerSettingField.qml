// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root
    required property var settings
    property var definition: null
    property string prefix: "host_"
    property bool accountPreference: false
    property bool editable: true
    property bool showHelp: false
    readonly property string key: definition ? definition.key : ""
    readonly property var choices: definition ? definition.choices : []
    readonly property bool overridden: Object.prototype.hasOwnProperty.call(settings.values, key)
    readonly property string savedValue: overridden ? settings.values[key] : ""
    readonly property string defaultValue: accountPreference ? "" : (settings.unitDefaults[key] || "")
    readonly property bool numericField: ["Quality", "Port", "MonitorIndex", "Avc444MotionGapMs", "Avc444RestMs", "Avc444MaxGapMs"].includes(key)
    readonly property bool sizeField: key === "VirtualMonitorFallbackSize"
    readonly property int minimum: key === "Port" ? 1 : key.startsWith("Avc444") ? 16 : 0
    readonly property int maximum: key === "Quality" ? 100 : key.startsWith("Avc444") ? 5000 : 65535
    readonly property int numericValue: Math.max(minimum, Math.min(maximum, Number(overridden ? savedValue : defaultValue) || minimum))
    readonly property bool incompleteNumber: accountPreference && overridden && savedValue === ""
    readonly property string inheritanceText: accountPreference ? i18nc("@item:inlistbox", "Use host setting") : i18nc("@item:inlistbox", "Use unit default")
    readonly property string label: {
        switch (key) {
        case "Address": return i18nc("@label", "Listen on");
        case "Port": return i18nc("@label", "Port");
        case "Quality": return i18nc("@label", "Image quality");
        case "AdaptiveQuality": return i18nc("@label", "Adjust to connection");
        case "PreferAudioQuality": return i18nc("@label", "When network is busy");
        case "StandardClientMedia": return i18nc("@label", "Other RDP app media");
        case "Codec": return i18nc("@label", "Color detail");
        case "SoftwareEncoding": return i18nc("@label", "Encoding policy");
        case "CameraLoopbackDevice": return i18nc("@label", "Camera bridge device");
        case "RenderPci": return i18nc("@label", "GPU PCI identities");
        case "VirtualMonitorPolicy": return i18nc("@label", "Physical displays");
        case "VirtualMonitorLayout": return i18nc("@label", "Layout");
        case "VirtualMonitorFallbackSize": return i18nc("@label", "Fallback size");
        case "WakeDisplayOnConnect": return i18nc("@label", "Keep displays awake");
        case "Avc444MotionGapMs": return i18nc("@label", "AVC444 motion interval");
        case "Avc444RestMs": return i18nc("@label", "AVC444 rest interval");
        case "Avc444MaxGapMs": return i18nc("@label", "AVC444 maximum interval");
        case "VirtualStockClientPolicy": return i18nc("@label", "Other RDP apps in Virtual");
        default: return definition ? definition.label : "";
        }
    }
    readonly property var displayChoices: choices.map(option => {
        let text = option.text;
        if (option.value === "") text = root.accountPreference ? root.inheritanceText : i18nc("@item:inlistbox", "Default (%1)", root.choiceText(root.defaultValue));
        else text = root.choiceText(option.value);
        return {value: option.value, text: text};
    })
    function choiceText(value) {
        if (key === "PreferAudioQuality") return value === "true" ? i18nc("@item:inlistbox", "Keep sound smooth") : i18nc("@item:inlistbox", "Keep video sharp");
        if (key === "StandardClientMedia") return value === "true" ? i18nc("@item:inlistbox", "Allow") : i18nc("@item:inlistbox", "Block");
        if (key === "VirtualMonitorPolicy") return value === "extend" ? i18nc("@item:inlistbox", "Keep on") : i18nc("@item:inlistbox", "Turn off during connection");
        if (key === "Codec") return value === "avc420" ? i18nc("@item:inlistbox", "Standard color (AVC420)") : value === "avc444" ? i18nc("@item:inlistbox", "Full color (AVC444)") : i18nc("@item:inlistbox", "Automatic");
        const option = choices.find(row => row.value === value);
        return option ? option.text : value;
    }
    function setNumber(value) { settings.setValue(key, String(value)); }
    objectName: prefix + "field_" + key
    Kirigami.FormData.label: label + ":"
    Kirigami.FormData.buddyFor: accountPreference && (numericField || sizeField) ? numericMode : numericField ? numericInputs : choices.length ? choice : key === "Address" ? addressMode : textValue
    Layout.preferredWidth: Kirigami.Units.gridUnit * 24
    Layout.maximumWidth: Kirigami.Units.gridUnit * 24
    spacing: Kirigami.Units.smallSpacing

    QQC2.ComboBox {
        id: numericMode
        objectName: "inherit_" + root.key
        visible: root.accountPreference && (root.numericField || root.sizeField)
        enabled: root.editable
        Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        model: [root.inheritanceText, i18nc("@item:inlistbox", "Custom")]
        currentIndex: root.overridden ? 1 : 0
        Accessible.name: root.label
        onActivated: index => {
            if (index === 0) root.settings.inherit(root.key);
            else if (!root.overridden) root.settings.setValue(root.key, ""); // incomplete, not a fabricated default
        }
    }
    RowLayout {
        id: numericInputs
        onActiveFocusChanged: if (activeFocus) numberValue.forceActiveFocus()
        visible: root.numericField && (!root.accountPreference || root.overridden)
        Layout.fillWidth: true
        QQC2.Slider {
            objectName: root.key === "Quality" ? root.prefix + "QualitySlider" : ""
            visible: root.key === "Quality"
            enabled: root.editable
            Layout.fillWidth: true; Layout.minimumWidth: Kirigami.Units.gridUnit * 4
            from: 0; to: 100; stepSize: 1; value: root.numericValue
            Accessible.name: root.label
            onMoved: { root.setNumber(Math.round(value)); value = Qt.binding(() => root.numericValue); }
        }
        QQC2.SpinBox {
            id: numberValue
            objectName: root.numericField ? root.prefix + root.key : ""
            editable: true; enabled: root.editable
            from: root.minimum; to: root.maximum; value: root.numericValue
            textFromValue: (value, locale) => root.incompleteNumber ? "" : Number(value).toLocaleString(locale, "f", 0)
            valueFromText: (text, locale) => Number.fromLocaleString(locale, text)
            Accessible.name: root.label
            onValueModified: { root.setNumber(value); value = Qt.binding(() => root.numericValue); }
        }
        QQC2.Label { visible: root.key.startsWith("Avc444"); text: i18nc("@label", "ms") }
        QQC2.ToolButton {
            objectName: "inheritHost_" + root.key
            visible: !root.accountPreference
            opacity: root.overridden ? 1 : 0; enabled: root.editable && root.overridden
            Accessible.ignored: !enabled
            icon.name: "edit-undo"; text: root.inheritanceText
            QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered && enabled
            onClicked: root.settings.inherit(root.key)
        }
        Item { Layout.fillWidth: true }
    }
    QQC2.Label { visible: root.incompleteNumber; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Enter a value or move the slider to choose a custom value.") }
    RowLayout {
        visible: root.sizeField && root.overridden
        Layout.fillWidth: true
        QQC2.SpinBox { id: sizeWidth; objectName: root.sizeField ? root.prefix + "FallbackWidth" : ""; editable: true; from: 320; to: 8192; stepSize: 2; enabled: root.editable; value: Number(root.savedValue.split("x")[0]) || 320; textFromValue: (value, locale) => !root.savedValue.split("x")[0] ? "" : Number(value).toLocaleString(locale, "f", 0); valueFromText: (text, locale) => Number.fromLocaleString(locale, text); Accessible.name: i18nc("@label", "Fallback width"); onValueModified: { root.settings.setValue(root.key, String(value) + "x" + (root.savedValue.split("x")[1] || "")); value = Qt.binding(() => Number(root.savedValue.split("x")[0]) || 320); } }
        QQC2.Label { text: "×" }
        QQC2.SpinBox { id: sizeHeight; objectName: root.sizeField ? root.prefix + "FallbackHeight" : ""; editable: true; from: 200; to: 8192; stepSize: 2; enabled: root.editable; value: Number(root.savedValue.split("x")[1]) || 200; textFromValue: (value, locale) => !root.savedValue.split("x")[1] ? "" : Number(value).toLocaleString(locale, "f", 0); valueFromText: (text, locale) => Number.fromLocaleString(locale, text); Accessible.name: i18nc("@label", "Fallback height"); onValueModified: { root.settings.setValue(root.key, (root.savedValue.split("x")[0] || "") + "x" + String(value)); value = Qt.binding(() => Number(root.savedValue.split("x")[1]) || 200); } }
        Item { Layout.fillWidth: true }
    }
    QQC2.ComboBox {
        id: choice
        objectName: root.choices.length ? root.prefix + root.key : ""
        visible: root.choices.length > 0
        enabled: root.editable
        implicitContentWidthPolicy: QQC2.ComboBox.WidestText
        model: root.displayChoices; textRole: "text"; valueRole: "value"
        Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        Accessible.name: root.label
        currentIndex: { const index = root.choices.findIndex(row => row.value === root.savedValue); return index < 0 ? 0 : index; }
        onActivated: { if (currentValue === "") root.settings.inherit(root.key); else root.settings.setValue(root.key, currentValue); }
    }
    QQC2.ComboBox {
        id: addressMode
        objectName: root.key === "Address" ? root.prefix + "AddressMode" : ""
        visible: root.key === "Address"; enabled: root.editable
        Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        model: [i18nc("@item:inlistbox", "Use unit default (all IPv4 interfaces)"), i18nc("@item:inlistbox", "All IPv4 interfaces"), i18nc("@item:inlistbox", "All IPv6 interfaces"), i18nc("@item:inlistbox", "Custom address")]
        currentIndex: !root.overridden ? 0 : root.savedValue === "0.0.0.0" ? 1 : root.savedValue === "::" ? 2 : 3
        Accessible.name: root.label
        onActivated: index => { if (index === 0) root.settings.inherit(root.key); else root.settings.setValue(root.key, index === 1 ? "0.0.0.0" : index === 2 ? "::" : ""); }
    }
    QQC2.TextField {
        id: textValue
        objectName: !root.choices.length && !root.numericField && !root.sizeField ? root.prefix + root.key : ""
        visible: !root.choices.length && !root.numericField && !root.sizeField && (root.key !== "Address" || addressMode.currentIndex === 3)
        enabled: root.editable
        Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        Accessible.name: root.label
        text: root.savedValue
        placeholderText: root.accountPreference ? i18nc("@info:placeholder", "Use host setting") : root.defaultValue
        maximumLength: root.accountPreference ? 256 : 4096
        onTextEdited: {
            if (text === "" && root.key !== "RenderPci" && root.key !== "Address" && !root.key.startsWith("Certificate")) root.settings.inherit(root.key);
            else root.settings.setValue(root.key, text);
        }
    }
    QQC2.Label { visible: root.showHelp; Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24; wrapMode: Text.Wrap; text: root.definition ? root.definition.help : ""; color: Kirigami.Theme.disabledTextColor }
}
