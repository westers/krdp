# SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
# SPDX-License-Identifier: BSD-2-Clause

add_definitions(-DTRANSLATION_DOMAIN=\"farside-server\")

add_executable(krdpserver)
add_library(KRdpBrokerUserSettings STATIC BrokerUserSettings.cpp UserConfiguration.cpp)
set_target_properties(KRdpBrokerUserSettings PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(KRdpBrokerUserSettings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_include_directories(KRdpBrokerUserSettings SYSTEM PUBLIC ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(KRdpBrokerUserSettings PUBLIC Qt6::Core)
add_library(KRdpBrokerAuthentication STATIC BrokerAuthentication.cpp BrokerAuthenticationAdmin.cpp)
set_target_properties(KRdpBrokerAuthentication PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(KRdpBrokerAuthentication PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KRdpBrokerAuthentication PUBLIC Qt6::Core KRdp PRIVATE OpenSSL::Crypto)

add_executable(farside-authentication-helper authenticationhelper.cpp)
target_link_libraries(farside-authentication-helper PRIVATE KRdpBrokerAuthentication OpenSSL::Crypto)
configure_file(org.farside.authentication.policy.in ${CMAKE_CURRENT_BINARY_DIR}/org.farside.authentication.policy @ONLY)
install(TARGETS farside-authentication-helper DESTINATION ${KDE_INSTALL_LIBEXECDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.farside.authentication.policy DESTINATION ${KDE_INSTALL_DATADIR}/polkit-1/actions)
add_library(KRdpLegacyMigration STATIC FarsideMigration.cpp)
set_target_properties(KRdpLegacyMigration PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(KRdpLegacyMigration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KRdpLegacyMigration PUBLIC Qt6::Core Qt6::DBus KF6::ConfigCore qt6keychain)
# AUD-K3: certificate generation/renewal; also linked by the KCM to show the fingerprint.
add_library(KRdpServerCertificate STATIC ServerCertificate.cpp)
set_target_properties(KRdpServerCertificate PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(KRdpServerCertificate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KRdpServerCertificate PUBLIC Qt6::Core PRIVATE OpenSSL::Crypto)
add_library(KRdpGuardianClient STATIC VirtualSessionGuardianClient.cpp)
target_sources(KRdpGuardianClient PRIVATE VirtualSessionJournal.cpp)
target_include_directories(KRdpGuardianClient PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(KRdpGuardianClient PUBLIC Qt6::Core Qt6::Network)
add_executable(krdp-virtual-session-entry virtualsessionentry.cpp)
target_sources(krdp-virtual-session-entry PRIVATE VirtualSessionServiceScope.cpp VirtualSessionServiceOwner.cpp VirtualSessionLogin.cpp)
add_library(KRdpVirtualPam STATIC VirtualSessionPam.cpp VirtualSessionLogin.cpp)
target_include_directories(KRdpVirtualPam PRIVATE ${PAM_INCLUDE_DIR})
target_link_libraries(KRdpVirtualPam PUBLIC Qt6::Core PRIVATE Qt6::DBus ${PAM_LIBRARIES})
add_executable(krdp-virtual-pam-keeper virtualpamkeeper.cpp)
target_link_libraries(krdp-virtual-pam-keeper PRIVATE KRdpVirtualPam KRdpGuardianClient)
target_link_libraries(krdp-virtual-session-entry PRIVATE KRdpGuardianClient Qt6::DBus)
add_executable(krdp-virtual-session-cleanup virtualsessioncleanup.cpp VirtualSessionKeeperProcess.cpp
    VirtualSessionServiceScope.cpp VirtualSessionLoginRecovery.cpp)
target_link_libraries(krdp-virtual-session-cleanup PRIVATE KRdpGuardianClient Qt6::DBus)
configure_file(krdp-virtual-session@.service.in ${CMAKE_CURRENT_BINARY_DIR}/krdp-virtual-session@.service @ONLY)
add_executable(krdp-virtual-host virtualhost.cpp VirtualHostTls.cpp VirtualSessionHostController.cpp VirtualSessionTransport.cpp VirtualStockClient.cpp
    VirtualSessionControl.cpp VirtualSessionSupervisor.cpp ConsoleWorkerEndpoint.cpp ConsoleWorkerSession.cpp WorkerCodecBridge.cpp VirtualResize.cpp)
target_link_libraries(krdp-virtual-host PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus OpenSSL::Crypto KRdp KRdpGuardianClient KRdpServerCertificate)
target_link_libraries(krdp-virtual-host PRIVATE KRdpBrokerAuthentication)
configure_file(krdp-virtual-host.service.in ${CMAKE_CURRENT_BINARY_DIR}/krdp-virtual-host.service @ONLY)
add_executable(krdp-virtual-device-entry virtualdeviceentry.cpp)
target_link_libraries(krdp-virtual-device-entry PRIVATE Qt6::Core)
target_include_directories(krdp-virtual-device-entry PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_executable(krdp-virtual-guardian virtualguardian.cpp VirtualSessionGuardian.cpp VirtualSessionStorage.cpp)
target_link_libraries(krdp-virtual-guardian PRIVATE Qt6::Core Qt6::Network)
add_executable(krdp-virtual-guardianctl virtualguardianctl.cpp VirtualSessionGuardianClient.cpp)
target_link_libraries(krdp-virtual-guardianctl PRIVATE Qt6::Core Qt6::Network)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/virtual-session-support)
configure_file(virtual-session-bus.conf ${CMAKE_CURRENT_BINARY_DIR}/virtual-session-support/virtual-session-bus.conf COPYONLY)
configure_file(virtual-session-pipewire.conf ${CMAKE_CURRENT_BINARY_DIR}/virtual-session-support/virtual-session-pipewire.conf COPYONLY)
file(REMOVE_RECURSE ${CMAKE_CURRENT_BINARY_DIR}/virtual-session-support/defaults)
file(COPY ${CMAKE_SOURCE_DIR}/scripts/virtual-probe-config/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/virtual-session-support/defaults)
file(COPY ${CMAKE_SOURCE_DIR}/scripts/virtual-session-locked-config/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/virtual-session-support/locked-config)
add_executable(krdp-console-host consolehost.cpp ConsoleSeat.cpp ConsoleWorkerEndpoint.cpp ConsoleWorkerSession.cpp WorkerCodecBridge.cpp ConsoleHostController.cpp ConsoleWorkerLauncher.cpp)
target_link_libraries(krdp-console-host PRIVATE KRdpBrokerAuthentication)
add_executable(krdp-console-probe consoleprobe.cpp ConsoleSeat.cpp)
add_executable(krdp-greeter-probe greeterprobe.cpp)
add_executable(krdp-audio-route-probe audioprobe.cpp ../src/PipeWireAudioPlayback.cpp)
if (BUILD_PLASMA_SESSION)
    add_executable(krdp-console-worker consoleworker.cpp ConsoleResizeExecutor.cpp ConsoleResizeSession.cpp OutputRestoreJournal.cpp VirtualResize.cpp VirtualResizeExecutor.cpp VirtualResizeSession.cpp ConsoleMicrophoneSession.h ConsoleCameraSession.h ../src/PipeWireMicrophone.cpp ../src/PipeWireCamera.cpp ../src/PipeWireAudioPlayback.cpp)
    add_executable(krdp-virtual-worker-probe virtualworkerprobe.cpp ConsoleWorkerEndpoint.cpp H264KeyframeSize.cpp VirtualResize.cpp)
    target_link_libraries(krdp-virtual-worker-probe PRIVATE Qt6::Core Qt6::Gui Qt6::Network KRdp PkgConfig::AVCodec)
    add_executable(krdp-virtual-monitor-creation-probe virtualmonitorcreationprobe.cpp)
    target_link_libraries(krdp-virtual-monitor-creation-probe PRIVATE Qt6::Core Qt6::Gui KRdp)
    configure_file(org.kde.krdpvirtualmonitorprobe.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/org.kde.krdpvirtualmonitorprobe.desktop @ONLY)
    target_sources(krdp-console-worker PRIVATE H264KeyframeSize.cpp DisplayWakeGuard.cpp PhysicalOutputGuard.cpp)
    target_link_libraries(krdp-console-worker PRIVATE PkgConfig::AVCodec KRdpGuardianClient)
    add_executable(krdp-virtual-session-probe virtualsessionprobe.cpp VirtualSessionSupervisor.cpp VirtualSessionControl.cpp ConsoleWorkerEndpoint.cpp)
    target_link_libraries(krdp-virtual-session-probe PRIVATE Qt6::Core Qt6::Gui Qt6::Network KRdp KRdpGuardianClient)
    add_executable(krdp-virtual-rdp-host-probe virtualrdphostprobe.cpp VirtualSessionHostController.cpp
        VirtualSessionTransport.cpp VirtualStockClient.cpp VirtualSessionControl.cpp VirtualSessionSupervisor.cpp ConsoleWorkerEndpoint.cpp ConsoleWorkerSession.cpp WorkerCodecBridge.cpp VirtualResize.cpp)
    target_link_libraries(krdp-virtual-rdp-host-probe PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus KRdp KRdpGuardianClient)
    configure_file(org.kde.krdpvirtualprobe.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/org.kde.krdpvirtualprobe.desktop @ONLY)
endif()

target_sources(krdpserver PRIVATE main.cpp SessionController.cpp DisplayWakeGuard.cpp PhysicalOutputGuard.cpp OutputRestoreJournal.cpp HostLayoutExecutor.cpp ConsoleSeat.cpp ConsoleWorkerEndpoint.cpp ConsoleWorkerSession.cpp WorkerCodecBridge.cpp ConsoleHostController.cpp ConsoleWorkerLauncher.cpp)

kconfig_target_kcfg_file(krdpserver FILE krdpserversettings.kcfg CLASS_NAME ServerConfig SINGLETON)

target_include_directories(krdpserver PRIVATE ${CMAKE_BINARY_DIR})
target_include_directories(krdp-audio-route-probe PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(krdpserver PRIVATE KRdpLegacyMigration KRdpServerCertificate Qt6::Gui Qt6::Network Qt6::DBus KF6::CoreAddons KF6::ConfigGui KF6::DBusAddons KF6::Crash KRdp qt6keychain KF6::StatusNotifierItem KF6::I18n KF6::GlobalAccel)
target_link_libraries(krdp-console-host PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus KRdp KRdpServerCertificate)
target_link_libraries(krdp-console-probe PRIVATE Qt6::Core Qt6::DBus)
target_link_libraries(krdp-greeter-probe PRIVATE Qt6::Core Qt6::Gui)
target_link_libraries(krdp-audio-route-probe PRIVATE Qt6::Core PkgConfig::PipeWire)
if (BUILD_PLASMA_SESSION)
    target_link_libraries(krdp-console-worker PRIVATE Qt6::Gui Qt6::Network Qt6::DBus KF6::GlobalAccel KRdp PkgConfig::PipeWire)
endif()

if (BUILD_PLASMA_SESSION)
    target_compile_definitions(krdpserver PRIVATE -DWITH_PLASMA_SESSION)
    target_sources(krdpserver PRIVATE PlasmaBackendProbe.cpp)
    target_link_libraries(krdpserver PRIVATE Qt6::WaylandClient Qt6::GuiPrivate)
endif()

configure_file(org.kde.krdpserver.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/io.github.westers.farside.server.desktop @ONLY)
configure_file(krdp-console-host.service.in ${CMAKE_CURRENT_BINARY_DIR}/krdp-console-host.service @ONLY)
if (BUILD_PLASMA_SESSION)
    configure_file(org.kde.krdpconsoleworker.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/io.github.westers.farside.consoleworker.desktop @ONLY)
endif()
configure_file(app-org.kde.krdpserver.service.in ${CMAKE_CURRENT_BINARY_DIR}/app-io.github.westers.farside.server.service @ONLY)

if(KRDP_BUILD_SYSTEM_PACKAGE)
    set_target_properties(krdpserver PROPERTIES OUTPUT_NAME farside-server)
    foreach(helper console-host console-worker virtual-host virtual-session-entry virtual-pam-keeper
        virtual-session-cleanup virtual-device-entry virtual-guardian virtual-guardianctl)
        set_target_properties(krdp-${helper} PROPERTIES OUTPUT_NAME farside-${helper})
    endforeach()
endif()

foreach(target krdpserver krdp-console-host krdp-virtual-host krdp-virtual-rdp-host-probe)
    if(TARGET ${target})
        target_link_libraries(${target} PRIVATE KRdpBrokerUserSettings)
    endif()
endforeach()

install(TARGETS krdpserver DESTINATION ${KDE_INSTALL_BINDIR})
install(TARGETS krdp-console-host DESTINATION ${KDE_INSTALL_BINDIR})
if(KRDP_BUILD_CONSOLE_TEST_PACKAGE)
    install(TARGETS krdp-console-host RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
        COMPONENT KRdpConsoleTest EXCLUDE_FROM_ALL)
endif()
# Diagnostic probes are always built but only installed on request.
if(INSTALL_DIAGNOSTIC_PROBES)
    install(TARGETS krdp-console-probe krdp-greeter-probe krdp-audio-route-probe DESTINATION ${KDE_INSTALL_BINDIR})
endif()
if (BUILD_PLASMA_SESSION)
    install(TARGETS krdp-console-worker DESTINATION ${KDE_INSTALL_BINDIR})
    if(KRDP_BUILD_CONSOLE_TEST_PACKAGE)
        install(TARGETS krdp-console-worker RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
            COMPONENT KRdpConsoleTest EXCLUDE_FROM_ALL)
    endif()
endif()
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.westers.farside.server.desktop DESTINATION ${KDE_INSTALL_APPDIR})
# KDE_INSTALL_SYSTEMDUNITDIR is lib/systemd; system units belong in its system/ subdirectory.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/krdp-console-host.service DESTINATION ${KDE_INSTALL_SYSTEMDUNITDIR}/system RENAME farside-console-host.service)
install(FILES console-host.conf.example virtual-host.conf.example DESTINATION ${KDE_INSTALL_DATADIR}/farside)
if (BUILD_PLASMA_SESSION)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.westers.farside.consoleworker.desktop DESTINATION ${KDE_INSTALL_APPDIR})
endif()
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/app-io.github.westers.farside.server.service DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR})
install(FILES 00-krdp.preset DESTINATION ${KDE_INSTALL_SYSTEMDUNITDIR}/user-preset RENAME 00-farside.preset)

# Explicit staging only. Default installs must not enable the unfinished root
# virtual service. Policy/unit drafts go under share, never active configuration.
# Configure the intended prefix before staging: the draft units embed it.
if (BUILD_PLASMA_SESSION)
    install(TARGETS krdp-virtual-host krdp-virtual-session-entry
        krdp-virtual-pam-keeper krdp-virtual-session-cleanup
        krdp-virtual-device-entry krdp-virtual-guardian krdp-virtual-guardianctl
        krdp-console-worker
        RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
        COMPONENT KRdpVirtualSessionTest EXCLUDE_FROM_ALL)
    install(TARGETS KRdp LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
        COMPONENT KRdpVirtualSessionTest EXCLUDE_FROM_ALL)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/krdp-virtual-host.service
        ${CMAKE_CURRENT_BINARY_DIR}/krdp-virtual-session@.service
        krdp-virtual-session.pam
        DESTINATION ${KDE_INSTALL_DATADIR}/krdp/virtual-session/drafts
        COMPONENT KRdpVirtualSessionTest EXCLUDE_FROM_ALL)
    install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/launch-virtual-session.sh
        ${CMAKE_SOURCE_DIR}/scripts/virtual-session-desktop.sh
        DESTINATION ${KDE_INSTALL_DATADIR}/krdp/virtual-session
        COMPONENT KRdpVirtualSessionTest EXCLUDE_FROM_ALL)
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/virtual-session-support/
        DESTINATION ${KDE_INSTALL_DATADIR}/krdp/virtual-session/support
        COMPONENT KRdpVirtualSessionTest EXCLUDE_FROM_ALL)
endif()

# The krdp system deb (scripts/package-krdp.sh): the retained-desktop broker,
# its per-desktop helpers and real system units. Both brokers stay disabled
# (00-krdp-system.preset); an administrator enables them after providing TLS
# material in /etc/krdp. The worker is shared with the Console host above.
if(KRDP_BUILD_SYSTEM_PACKAGE)
    install(TARGETS krdp-virtual-host krdp-virtual-session-entry
        krdp-virtual-pam-keeper krdp-virtual-session-cleanup
        krdp-virtual-device-entry krdp-virtual-guardian krdp-virtual-guardianctl
        DESTINATION ${KDE_INSTALL_BINDIR})
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/krdp-virtual-host.service DESTINATION ${KDE_INSTALL_SYSTEMDUNITDIR}/system RENAME farside-virtual-host.service)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/krdp-virtual-session@.service DESTINATION ${KDE_INSTALL_SYSTEMDUNITDIR}/system RENAME farside-virtual-session@.service)
    install(FILES 00-krdp-system.preset DESTINATION ${KDE_INSTALL_SYSTEMDUNITDIR}/system-preset RENAME 00-farside-system.preset)
    install(FILES krdp-virtual-session.pam DESTINATION /etc/pam.d RENAME farside-virtual-session)
    install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/launch-virtual-session.sh
        ${CMAKE_SOURCE_DIR}/scripts/virtual-session-desktop.sh
        DESTINATION ${KDE_INSTALL_DATADIR}/farside/virtual-session)
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/virtual-session-support/
        DESTINATION ${KDE_INSTALL_DATADIR}/farside/virtual-session/support)
endif()
