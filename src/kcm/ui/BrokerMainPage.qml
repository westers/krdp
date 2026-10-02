// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

KCM.AbstractKCM {
    id: root
    objectName: "mainPage"
    title: i18nc("@title:window", "Farside Remote Desktop")
    framedView: false
    property var navigation: kcm
    property var administration: kcm.brokerServices
    property var consoleHost: kcm.consoleHostSettings
    property var virtualHost: kcm.virtualHostSettings
    property var sessionSettings: kcm.virtualSessionSettings
    property var authentication: kcm.brokerAuthentication
    property var preferences: kcm.brokerPreferences
    property string hostName: kcm.hostName
    property alias currentTab: tabs.currentIndex
    readonly property var flickable: tabs.currentIndex === 0 ? consolePage.flickable : tabs.currentIndex === 1 ? virtualPage.flickable : tabs.currentIndex === 2 ? accessPage.flickable : preferencesPage.flickable
    function showPreferences() { tabs.currentIndex = 3; }
    function copyAddressToClipboard(address) { navigation.copyAddressToClipboard(address); }
    Component.onCompleted: if (!administration.busy && administration.services.some(service => !service.known)) administration.refresh(false)

    header: QQC2.TabBar {
        id: tabs
        objectName: "settingsTabs"
        QQC2.TabButton { objectName: "consoleTab"; text: i18nc("@title:tab", "Console") }
        QQC2.TabButton { objectName: "virtualTab"; text: i18nc("@title:tab", "Virtual") }
        QQC2.TabButton { objectName: "accessTab"; text: i18nc("@title:tab", "Access") }
        QQC2.TabButton { objectName: "preferencesTab"; text: i18nc("@title:tab", "My Preferences") }
    }
    StackLayout {
        anchors.fill: parent
        currentIndex: tabs.currentIndex
        // These pages stay alive. Service notifications only update status;
        // they never replace a Repeater model or recreate an editor.
        BrokerHostsPage {
            Layout.minimumWidth: 0
            Layout.minimumHeight: 0
            id: consolePage
            objectName: "consoleSettingsPage"
            fixedScope: 0
            showCertificate: false
            showService: true
            administration: root.administration
            navigation: root
            hostName: root.hostName
            consoleSettings: root.consoleHost; virtualSettings: root.virtualHost; sessionSettings: root.sessionSettings
        }
        BrokerHostsPage {
            Layout.minimumWidth: 0
            Layout.minimumHeight: 0
            id: virtualPage
            objectName: "virtualSettingsPage"
            fixedScope: 1
            showCertificate: false
            showService: true
            administration: root.administration
            navigation: root
            hostName: root.hostName
            consoleSettings: root.consoleHost; virtualSettings: root.virtualHost; sessionSettings: root.sessionSettings
        }
        BrokerSignInPage {
            Layout.minimumWidth: 0
            Layout.minimumHeight: 0
            id: accessPage
            administration: root.authentication
            consoleSettings: root.consoleHost; virtualSettings: root.virtualHost; sessionSettings: root.sessionSettings
            serviceAdministration: root.administration
        }
        BrokerPreferencesPage { Layout.minimumWidth: 0; Layout.minimumHeight: 0; id: preferencesPage; preferences: root.preferences }
    }
}
