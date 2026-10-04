// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
// A "WIDTHxHEIGHT" value as two spin boxes; shown once the preference is custom. Empty halves stay blank until typed.
RowLayout {
    id: root
    required property BrokerFieldState field
    readonly property bool shown: field.overridden
    readonly property int widthMin: field.definition.min
    readonly property int heightMin: field.definition.heightMin
    readonly property int sizeMax: field.definition.max
    readonly property string widthText: field.savedValue.split("x")[0] || ""
    readonly property string heightText: field.savedValue.split("x")[1] || ""
    Binding { target: root.field; property: "primary"; value: widthBox }
    Binding { target: root.field; property: "controlShown"; value: root.shown }
    Layout.fillWidth: true
    QQC2.SpinBox {
        id: widthBox
        objectName: root.field.prefix + "FallbackWidth"
        editable: true; from: root.widthMin; to: root.sizeMax; stepSize: 2; enabled: root.field.editable
        value: Number(root.widthText) || root.widthMin
        textFromValue: (value, locale) => root.widthText === "" ? "" : Number(value).toLocaleString(locale, "f", 0)
        valueFromText: (text, locale) => Number.fromLocaleString(locale, text)
        Accessible.name: i18nc("@label", "Fallback width")
        onValueModified: { root.field.settings.setValue(root.field.key, String(value) + "x" + root.heightText); value = Qt.binding(() => Number(root.widthText) || root.widthMin); }
    }
    QQC2.Label { text: "×" }
    QQC2.SpinBox {
        objectName: root.field.prefix + "FallbackHeight"
        editable: true; from: root.heightMin; to: root.sizeMax; stepSize: 2; enabled: root.field.editable
        value: Number(root.heightText) || root.heightMin
        textFromValue: (value, locale) => root.heightText === "" ? "" : Number(value).toLocaleString(locale, "f", 0)
        valueFromText: (text, locale) => Number.fromLocaleString(locale, text)
        Accessible.name: i18nc("@label", "Fallback height")
        onValueModified: { root.field.settings.setValue(root.field.key, root.widthText + "x" + String(value)); value = Qt.binding(() => Number(root.heightText) || root.heightMin); }
    }
    Item { Layout.fillWidth: true }
}
