// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kcmutils as KCM

// A combo box bound to one string setting of KRDPServerSettings. `model` is a
// list of {text, value}; an unknown saved value shows the first entry without
// changing the setting.
QQC2.ComboBox {
    id: combo

    required property var configObject
    required property string settingName

    readonly property var settingValue: configObject[settingName]

    textRole: "text"
    valueRole: "value"

    function sync(): void {
        const idx = indexOfValue(settingValue);
        currentIndex = idx >= 0 ? idx : 0;
    }

    onSettingValueChanged: sync()
    onModelChanged: sync()
    Component.onCompleted: sync()
    onActivated: {
        if (typeof currentValue === "string" && configObject[settingName] !== currentValue) {
            configObject[settingName] = currentValue;
        }
    }

    KCM.SettingStateBinding {
        configObject: combo.configObject
        settingName: combo.settingName
    }
}
