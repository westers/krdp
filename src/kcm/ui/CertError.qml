// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Only for a certificate the user manages: the server creates and renews its
// own (AUD-K3), and the Security Certificate section shows its state.
Kirigami.InlineMessage {
    id: certificateError
    readonly property bool manual: !kcm.settings().autogenerateCertificates
    readonly property bool broken: kcm.certificateState === "missing" || kcm.certificateState === "unusable" || kcm.certificateState === "expired"
    type: broken ? Kirigami.MessageType.Error : Kirigami.MessageType.Warning
    position: Kirigami.InlineMessage.Position.Header
    Layout.fillWidth: true
    visible: manual && (broken || kcm.certificateState === "expiring")
    text: broken
        ? i18nc("@info:status", "The server cannot accept connections: the chosen TLS certificate is missing, unreadable, expired or does not match its key. Choose another one, or let the server create one automatically.")
        : i18nc("@info:status %1 date", "The chosen TLS certificate expires on %1. Replace it, or let the server create one automatically.", kcm.certificateExpiry)
}
