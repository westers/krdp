// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
// One of definition.choices. The first, empty-valued choice means "inherit" and names the value it would inherit.
QQC2.ComboBox {
    id: root
    required property BrokerFieldState field
    Binding { target: root.field; property: "primary"; value: root }
    readonly property var choices: field.definition.choices
    function choiceText(value) {
        const option = choices.find(row => row.value === value);
        return option ? option.formText : value;
    }
    readonly property var displayChoices: choices.map(option => ({
        value: option.value,
        text: option.value !== "" ? option.formText : field.accountPreference ? field.inheritanceText : i18nc("@item:inlistbox", "Default (%1)", choiceText(field.defaultValue))
    }))
    objectName: field.prefix + field.key
    enabled: field.editable
    implicitContentWidthPolicy: QQC2.ComboBox.WidestText
    model: displayChoices; textRole: "text"; valueRole: "value"
    Accessible.name: field.label
    currentIndex: { const index = choices.findIndex(row => row.value === field.savedValue); return index < 0 ? 0 : index; }
    onActivated: { if (currentValue === "") field.settings.inherit(field.key); else field.settings.setValue(field.key, currentValue); }
}
