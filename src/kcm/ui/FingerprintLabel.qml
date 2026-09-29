// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The certificate's SHA-256 fingerprint in groups of four bytes, as viewers
// show it, with a copy button.
RowLayout {
    id: row
    spacing: Kirigami.Units.smallSpacing

    readonly property string fingerprint: kcm.certificateFingerprint
    // SHA-256 is 32 bytes: two lines of four groups of four.
    readonly property string grouped: {
        const bytes = fingerprint.split(":");
        const lines = [];
        for (let i = 0; i < bytes.length; i += 16) {
            const groups = [];
            for (let j = i; j < Math.min(i + 16, bytes.length); j += 4) {
                groups.push(bytes.slice(j, Math.min(j + 4, i + 16)).join(":"));
            }
            lines.push(groups.join(" "));
        }
        return lines.join("\n");
    }

    Kirigami.SelectableLabel {
        visible: row.fingerprint !== ""
        font.family: Kirigami.Theme.fixedWidthFont.family
        text: row.grouped
        Accessible.name: i18nc("@info accessible name", "Certificate fingerprint")
    }
    CopyButton {
        visible: row.fingerprint !== ""
        Layout.alignment: Qt.AlignTop
        value: row.fingerprint
        text: i18nc("@action:button", "Copy Fingerprint")
    }
    MissingLabel {
        visible: row.fingerprint === ""
    }

    component MissingLabel: Kirigami.SelectableLabel {
        color: Kirigami.Theme.disabledTextColor
        text: kcm.settings().autogenerateCertificates
            ? i18nc("@info", "Farside creates its certificate when it first starts.")
            : i18nc("@info", "No usable certificate")
    }
}
