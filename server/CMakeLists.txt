# SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
# SPDX-License-Identifier: BSD-2-Clause

add_definitions(-DTRANSLATION_DOMAIN=\"krdpserver\")

add_executable(krdpserver)
add_executable(krdp-console-probe consoleprobe.cpp ConsoleSeat.cpp)
add_executable(krdp-greeter-probe greeterprobe.cpp)
add_executable(krdp-audio-route-probe audioprobe.cpp)

target_sources(krdpserver PRIVATE main.cpp SessionController.cpp DisplayWakeGuard.cpp PhysicalOutputGuard.cpp HostLayoutExecutor.cpp ConsoleSeat.cpp)

kconfig_target_kcfg_file(krdpserver FILE krdpserversettings.kcfg CLASS_NAME ServerConfig SINGLETON)

target_include_directories(krdpserver PRIVATE ${CMAKE_BINARY_DIR})

target_link_libraries(krdpserver PRIVATE Qt6::Gui Qt6::DBus KF6::CoreAddons KF6::ConfigGui KF6::DBusAddons KF6::Crash KRdp qt6keychain KF6::StatusNotifierItem KF6::I18n KF6::GlobalAccel)
target_link_libraries(krdp-console-probe PRIVATE Qt6::Core Qt6::DBus)
target_link_libraries(krdp-greeter-probe PRIVATE Qt6::Core Qt6::Gui)
target_link_libraries(krdp-audio-route-probe PRIVATE Qt6::Core KRdp)

if (BUILD_PLASMA_SESSION)
    target_compile_definitions(krdpserver PRIVATE -DWITH_PLASMA_SESSION)
endif()

configure_file(org.kde.krdpserver.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/org.kde.krdpserver.desktop @ONLY)
configure_file(app-org.kde.krdpserver.service.in ${CMAKE_CURRENT_BINARY_DIR}/app-org.kde.krdpserver.service @ONLY)

install(TARGETS krdpserver DESTINATION ${KDE_INSTALL_BINDIR})
install(TARGETS krdp-console-probe DESTINATION ${KDE_INSTALL_BINDIR})
install(TARGETS krdp-greeter-probe DESTINATION ${KDE_INSTALL_BINDIR})
install(TARGETS krdp-audio-route-probe DESTINATION ${KDE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.krdpserver.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/app-org.kde.krdpserver.service DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR})
install(FILES 00-krdp.preset DESTINATION ${KDE_INSTALL_SYSTEMDUNITDIR}/user-preset)
