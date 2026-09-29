# SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
#
# SPDX-License-Identifier: BSD-2-Clause

add_definitions(-DTRANSLATION_DOMAIN=\"kcm_farside\")

# The KCM's non-QML logic, as a static library so autotests can link it
# without loading the plugin (AUD-K14).
add_library(kcm_krdpserver_core STATIC
    settingsdefaults.cpp
    useraccounts.cpp
    usersmodel.cpp
    serviceinfo.cpp
    journalreader.cpp
    farsideidentity.cpp
    coexistence.cpp
    systemdservicemanager.cpp
    hostdevices.cpp
)
set_target_properties(kcm_krdpserver_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(kcm_krdpserver_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/server)

# One schema for krdpserverrc: server/ owns it and the KCM generates its
# settings class from the same file (AUD-K13).
kconfig_target_kcfg_file(kcm_krdpserver_core
    FILE ${CMAKE_SOURCE_DIR}/server/krdpserversettings.kcfg
    CLASS_NAME KRDPServerSettings
    MUTATORS
    DEFAULT_VALUE_GETTERS
    GENERATE_PROPERTIES
    PARENT_IN_CONSTRUCTOR
    NOTIFIERS
    GENERATE_MOC
)

ecm_qt_declare_logging_category(kcm_krdpserver_core
    HEADER krdpkcm_logging.h
    IDENTIFIER KRDPKCM
    CATEGORY_NAME farside.kcm
    DESCRIPTION "KRDPKCM"
    EXPORT kcm_farside
    DEFAULT_SEVERITY Debug
)

target_link_libraries(kcm_krdpserver_core PUBLIC
    Qt::Core
    Qt::DBus
    KF6::ConfigGui
    KF6::CoreAddons
    KF6::I18n
    KRdpServerCertificate
    KRdpLegacyMigration
    PRIVATE
    qt6keychain
)

# Link libsystemd when found and advertise feature to code
if (Systemd_FOUND)
    target_link_libraries(kcm_krdpserver_core PRIVATE Systemd::systemd)
    target_compile_definitions(kcm_krdpserver_core PRIVATE HAVE_SYSTEMD)
endif()

kcmutils_add_qml_kcm(kcm_farside)

kcmutils_generate_module_data(kcm_farside
    MODULE_DATA_HEADER krdpserverdata.h
    MODULE_DATA_CLASS_NAME KRDPServerData
    SETTINGS_HEADERS krdpserversettings.h
    SETTINGS_CLASSES KRDPServerSettings
)

ecm_qt_declare_logging_category(kcm_farside
    HEADER krdp_logging.h
    IDENTIFIER KRDP
    CATEGORY_NAME farside.server
    DESCRIPTION "KRdp"
    EXPORT kcm_farside
    DEFAULT_SEVERITY Debug
)

target_sources(kcm_farside PRIVATE
   kcmkrdpserver.cpp
   kcmkrdpserver.h
)

qt_add_dbus_interface(kcm_krdpserver_dbus_srcs org.freedesktop.impl.portal.PermissionStore.xml org.freedesktop.impl.portal.PermissionStore)

target_sources(kcm_farside PRIVATE ${kcm_krdpserver_dbus_srcs})

target_link_libraries(kcm_farside PRIVATE
    kcm_krdpserver_core
    Qt::Quick
    Qt::DBus
    Qt::Network
    KF6::CoreAddons
    KF6::KCMUtils
    KF6::KCMUtilsQuick
    KF6::I18n
    K::KPipeWireRecord
)

ecm_qt_install_logging_categories(EXPORT kcm_farside DESTINATION ${KDE_INSTALL_LOGGINGCATEGORIESDIR})

# ClientDisplayInfo.h / VideoCodecSupport.h: the server's own validation rules
# (header-only; FreeRDP headers for the RDPGFX enums, nothing linked).
target_include_directories(kcm_farside PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(kcm_farside SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
if (BUILD_PLASMA_SESSION)
    target_compile_definitions(kcm_farside PRIVATE WITH_PLASMA_SESSION)
endif()
