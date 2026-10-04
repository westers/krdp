// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import org.kde.kirigami as Kirigami
// Saved-certificate summary plus an in-place editor for the host's local TLS draft.
// Collapsing hides the editor (never unloads it) so its state survives.
ColumnLayout {
    id: root
    objectName: "certificateSection"
    required property var host
    property bool open: false
    signal staged()
    signal cancelled()
    readonly property var draft: host.certificateDraft
    readonly property var certificate: host.metadata.tls || ({})
    property url certificateFile
    property url privateKeyFile
    property int selectionGeneration: 0
    // Whether the saved certificate's own details (paths, full fingerprint, validity) are shown in the editor.
    property bool detailsOpen: false
    readonly property string fingerprint: certificate.fingerprint || ""
    // The first three and last two bytes of the SHA-256 fingerprint: enough to recognize it, short enough for one line.
    readonly property string shortFingerprint: {
        const parts = fingerprint.split(":");
        return parts.length > 8 ? parts.slice(0, 3).join(":") + "…" + parts.slice(-2).join(":") : fingerprint;
    }
    readonly property string validity: certificate.state === "valid" && certificate.notAfter ? i18nc("@info %1 date", "Valid until %1", String(certificate.notAfter).slice(0, 10)) : certificateState(certificate.state)
    // Two short lines for the page (they stay readable beside the buttons at any width): the state with the expiry date when usable, and the short fingerprint.
    readonly property string summary: shortFingerprint !== "" ? validity + "\n" + i18nc("@info %1 fingerprint", "SHA-256 %1", shortFingerprint) : validity
    spacing: Kirigami.Units.smallSpacing
    function begin() {
        if (!host.beginCertificateEdit()) return;
        ++selectionGeneration; certificateFile = ""; privateKeyFile = ""; detailsOpen = false; open = true;
    }
    function cancel() {
        host.cancelCertificateEdit();
        ++selectionGeneration; certificateFile = ""; privateKeyFile = ""; detailsOpen = false; open = false; cancelled();
    }
    function certificateState(state) {
        switch (state) {
        case "valid": return i18nc("@info", "Current");
        case "expiring": return i18nc("@info", "Expires soon");
        case "expired": return i18nc("@info", "Expired");
        case "not-yet-valid": return i18nc("@info", "Not yet valid");
        case "missing": return i18nc("@info", "Not found");
        case "unsafe": return i18nc("@info", "Unsafe file or folder permissions");
        case "invalid": return i18nc("@info", "Unreadable, or the certificate and key do not match");
        default: return i18nc("@info", "Not checked");
        }
    }
    ColumnLayout {
        objectName: "certificateEditor"
        Layout.fillWidth: true
        visible: root.open
        spacing: Kirigami.Units.smallSpacing
        QQC2.Button {
            objectName: "certificateDetailsToggle"
            flat: true
            text: root.detailsOpen ? i18nc("@action:button", "Hide Current Certificate Details") : i18nc("@action:button", "Show Current Certificate Details")
            icon.name: root.detailsOpen ? "arrow-down" : "arrow-right"
            onClicked: root.detailsOpen = !root.detailsOpen
        }
        // Hidden, never unloaded: it is cheap, and the page keeps one set of rows for the whole editing session.
        Kirigami.FormLayout {
            id: savedForm
            objectName: "certificateDetails"
            Layout.fillWidth: true
            visible: root.detailsOpen
            QQC2.Label { Kirigami.FormData.label: i18nc("@label", "Status:"); text: root.certificateState(root.certificate.state) }
            Kirigami.SelectableLabel { visible: text !== ""; Kirigami.FormData.label: i18nc("@label", "Certificate:"); Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24; wrapMode: Text.WrapAnywhere; text: (root.host.metadata.effective || {}).Certificate || "" }
            Kirigami.SelectableLabel { visible: text !== ""; Kirigami.FormData.label: i18nc("@label", "Private key:"); Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24; wrapMode: Text.WrapAnywhere; text: (root.host.metadata.effective || {}).CertificateKey || "" }
            Kirigami.SelectableLabel { Kirigami.FormData.label: i18nc("@label", "Fingerprint:"); Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 24; wrapMode: Text.WrapAnywhere; text: root.certificate.fingerprint || i18nc("@info", "Not available") }
            QQC2.Label { Kirigami.FormData.label: i18nc("@label", "Validity:"); Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.certificate.notBefore ? i18nc("@info", "%1 to %2", root.certificate.notBefore, root.certificate.notAfter) : i18nc("@info", "Not available") }
        }
        Kirigami.Heading { level: 4; text: i18nc("@title:group", "Certificate Source") }
        ColumnLayout {
            visible: root.draft.loaded
            Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
            QQC2.ButtonGroup { id: sourceGroup }
            QQC2.RadioButton { objectName: "certificateKeep"; text: i18nc("@option:radio", "Keep the current certificate"); QQC2.ButtonGroup.group: sourceGroup; checked: root.draft.tlsMode === "keep"; onClicked: root.draft.chooseTls("keep") }
            QQC2.RadioButton { objectName: "certificateExisting"; text: i18nc("@option:radio", "Use existing certificate files"); QQC2.ButtonGroup.group: sourceGroup; checked: root.draft.tlsMode === "existing"; onClicked: root.draft.chooseTls("existing") }
            QQC2.RadioButton { objectName: "certificateStandard"; text: i18nc("@option:radio", "Use the standard Farside certificate"); QQC2.ButtonGroup.group: sourceGroup; checked: root.draft.tlsMode === "standard"; onClicked: root.draft.chooseTls("standard") }
            QQC2.RadioButton { objectName: "certificateImport"; text: i18nc("@option:radio", "Import a certificate and private key"); QQC2.ButtonGroup.group: sourceGroup; checked: root.draft.tlsMode === "import"; onClicked: root.draft.chooseTls("import") }
        }
        Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 32;
            Layout.alignment: Qt.AlignLeft
            Layout.fillWidth: true; visible: root.draft.loaded && root.draft.tlsMode === "existing"
            BrokerFieldRepeater { settings: root.draft; section: "certificate"; prefix: "certificate_" }
        }
        QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; visible: root.draft.tlsMode === "standard"; text: i18nc("@info", "Goes back to the standard Farside certificate. Your existing certificate files and earlier imports are kept. After the service restarts, clients may ask you to confirm the fingerprint again.") }
        ColumnLayout {
            Layout.fillWidth: true; visible: root.draft.loaded && root.draft.tlsMode === "import"
            Kirigami.FormLayout {
            wideMode: width >= Kirigami.Units.gridUnit * 32;
            Layout.alignment: Qt.AlignLeft
                Layout.fillWidth: true
                RowLayout {
                    Kirigami.FormData.label: i18nc("@label", "Certificate file:")
                    QQC2.TextField { Layout.fillWidth: true; readOnly: true; text: root.certificateFile.toString(); placeholderText: i18nc("@info:placeholder", "Choose a PEM certificate") }
                    QQC2.Button { objectName: "selectHostCertificate"; text: i18nc("@action:button", "Browse…"); onClicked: { certDialog.generation = root.selectionGeneration; certDialog.open(); } }
                }
                RowLayout {
                    Kirigami.FormData.label: i18nc("@label", "Private key file:")
                    QQC2.TextField { Layout.fillWidth: true; readOnly: true; text: root.privateKeyFile.toString(); placeholderText: i18nc("@info:placeholder", "Choose an unencrypted PEM key") }
                    QQC2.Button { objectName: "selectHostPrivateKey"; text: i18nc("@action:button", "Browse…"); onClicked: { keyDialog.generation = root.selectionGeneration; keyDialog.open(); } }
                }
            }
            QQC2.Button { objectName: "inspectHostImport"; text: i18nc("@action:button", "Check These Files"); enabled: root.certificateFile.toString() !== "" && root.privateKeyFile.toString() !== ""; onClicked: root.draft.importTls(root.certificateFile, root.privateKeyFile) }
            QQC2.Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: i18nc("@info", "Choose a current certificate and its matching unencrypted private key. The key is never shown. Imported files are managed by the administrator, and you renew them yourself.") }
            Kirigami.InlineMessage { visible: (root.draft.importMetadata.fingerprint || "") !== "" && root.draft.canStageCertificate; Layout.fillWidth: true; type: Kirigami.MessageType.Positive; text: i18nc("@info", "The certificate and key match. Ready to use.") }
            Kirigami.SelectableLabel { objectName: "hostImportPreview"; visible: (root.draft.importMetadata.fingerprint || "") !== ""; Layout.fillWidth: true; Layout.maximumWidth: Kirigami.Units.gridUnit * 28; wrapMode: Text.WrapAnywhere; text: i18nc("@info", "Selected certificate SHA-256: %1. Valid until %2.", root.draft.importMetadata.fingerprint || "", root.draft.importMetadata.notAfter || "") }
        }
        Kirigami.InlineMessage { Layout.fillWidth: true; visible: root.draft.error !== ""; type: Kirigami.MessageType.Error; text: root.draft.error }
        RowLayout {
            Layout.fillWidth: true
            QQC2.Button { objectName: "cancelCertificateEdit"; text: i18nc("@action:button", "Cancel"); onClicked: root.cancel() }
            Item { Layout.fillWidth: true }
            QQC2.Button { objectName: "stageCertificateEdit"; highlighted: true; text: i18nc("@action:button", "Use This Certificate"); enabled: root.draft.canStageCertificate && !root.host.busy && !root.host.outcomeUnknown; onClicked: { if (root.host.stageCertificateEdit()) { root.open = false; root.staged(); } } }
        }
    }
    Dialogs.FileDialog {
        id: certDialog; property int generation
        objectName: "hostCertificateDialog"
        title: i18nc("@title:window", "Choose Certificate")
        nameFilters: [i18nc("@info", "PEM certificates (*.pem *.crt)"), i18nc("@info", "All files (*)")]
        onAccepted: if (root.visible && generation === root.selectionGeneration && root.draft.loaded && root.draft.tlsMode === "import") { root.draft.clearTlsImport(); root.certificateFile = selectedFile; }
    }
    Dialogs.FileDialog {
        id: keyDialog; property int generation
        objectName: "hostPrivateKeyDialog"
        title: i18nc("@title:window", "Choose Unencrypted Private Key")
        nameFilters: [i18nc("@info", "PEM keys (*.pem *.key)"), i18nc("@info", "All files (*)")]
        onAccepted: if (root.visible && generation === root.selectionGeneration && root.draft.loaded && root.draft.tlsMode === "import") { root.draft.clearTlsImport(); root.privateKeyFile = selectedFile; }
    }
}
