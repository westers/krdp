// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
// A number between definition.min and definition.max: a spin box, plus a slider when the control is "slider".
ColumnLayout {
    id: root
    required property BrokerFieldState field
    readonly property bool incomplete: field.accountPreference && field.overridden && field.savedValue === ""
    readonly property bool shown: !field.accountPreference || field.overridden
    readonly property int minimum: field.definition.min
    readonly property int maximum: field.definition.max
    readonly property int numericValue: Math.max(minimum, Math.min(maximum, Number(field.overridden ? field.savedValue : field.defaultValue) || minimum))
    Binding { target: root.field; property: "primary"; value: spin }
    Binding { target: root.field; property: "controlShown"; value: root.shown }
    function setNumber(value) { field.settings.setValue(field.key, String(value)); }
    spacing: Kirigami.Units.smallSpacing
    // The slider takes the width of the row. Where it could not stay usable beside the number box (a narrow pane), the number box and
    // the inherit button wrap to a line of their own below it.
    readonly property bool compact: width < Kirigami.Units.gridUnit * 17
    GridLayout {
        visible: root.shown
        Layout.fillWidth: true
        columns: root.compact ? 1 : 2
        columnSpacing: Kirigami.Units.smallSpacing; rowSpacing: Kirigami.Units.smallSpacing
        QQC2.Slider {
            objectName: root.field.prefix + root.field.key + "Slider"
            visible: root.field.definition.control === "slider"
            enabled: root.field.editable
            Layout.fillWidth: true; Layout.minimumWidth: Kirigami.Units.gridUnit * 8; Layout.preferredWidth: Kirigami.Units.gridUnit * 25; Layout.maximumWidth: Kirigami.Units.gridUnit * 25
            from: root.minimum; to: root.maximum; stepSize: 1; value: root.numericValue
            Accessible.name: root.field.label
            onMoved: { root.setNumber(Math.round(value)); value = Qt.binding(() => root.numericValue); }
        }
        RowLayout {
            Layout.fillWidth: root.field.definition.control !== "slider"
            QQC2.SpinBox {
                id: spin
                objectName: root.field.prefix + root.field.key
                editable: true; enabled: root.field.editable
                from: root.minimum; to: root.maximum; value: root.numericValue
                // Plain digits: a port or an index is an identifier, never "3,391".
                textFromValue: (value, locale) => root.incomplete ? "" : String(Math.round(value))
                valueFromText: (text, locale) => text.replace(/[^0-9]/g, "") === "" ? from : parseInt(text.replace(/[^0-9]/g, ""), 10)
                Accessible.name: root.field.label
                onValueModified: { root.setNumber(value); value = Qt.binding(() => root.numericValue); }
            }
            QQC2.Label { visible: root.field.definition.unit !== ""; text: root.field.definition.unit }
            QQC2.ToolButton {
                objectName: "inheritHost_" + root.field.key
                visible: !root.field.accountPreference
                opacity: root.field.overridden ? 1 : 0; enabled: root.field.editable && root.field.overridden
                Accessible.ignored: !enabled
                icon.name: "edit-undo"; text: root.field.inheritanceText; display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text; QQC2.ToolTip.visible: hovered && enabled
                onClicked: root.field.settings.inherit(root.field.key)
            }
            Item { Layout.fillWidth: true }
        }
    }
    QQC2.Label { visible: root.incomplete; Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Enter a value or move the slider to choose a custom value.") }
}
