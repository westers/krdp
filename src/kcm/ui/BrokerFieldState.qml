// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
import QtQuick
import org.kde.kirigami as Kirigami
// What every control of one settings field needs to know: the model, the field definition (from C++) and the
// stored value, with the inheritance wording that goes with the field's scope.
QtObject {
    required property var settings
    required property var definition
    required property string prefix
    required property bool accountPreference
    required property bool editable
    // Reported back by the control that is showing: its main item (for label focus) and whether it has anything to show.
    property Item primary: null
    property bool controlShown: true
    // The page's Advanced options are open: choices the definition lists as advanced are offered too.
    property bool advancedChoices: false
    readonly property string key: definition.key
    readonly property string label: definition.formLabel
    readonly property bool overridden: Object.prototype.hasOwnProperty.call(settings.values, key)
    readonly property string savedValue: overridden ? settings.values[key] : ""
    readonly property string defaultValue: accountPreference ? "" : (settings.unitDefaults[key] || "")
    readonly property string inheritanceText: definition.inheritText
}
