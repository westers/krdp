add_executable(AdaptiveQualityTest AdaptiveQualityTest.cpp)
target_include_directories(AdaptiveQualityTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(AdaptiveQualityTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME AdaptiveQualityTest COMMAND AdaptiveQualityTest)

add_executable(PeerContextLifecycleTest PeerContextLifecycleTest.cpp
    ${CMAKE_SOURCE_DIR}/src/PeerContext.cpp ${CMAKE_BINARY_DIR}/src/krdp_logging.cpp)
target_include_directories(PeerContextLifecycleTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
target_include_directories(PeerContextLifecycleTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(PeerContextLifecycleTest PRIVATE Qt6::Test freerdp winpr)
target_link_options(PeerContextLifecycleTest PRIVATE -Wl,--wrap=WTSOpenServerA -Wl,--wrap=WTSCloseServer -Wl,--wrap=freerdp_peer_context_free)
add_test(NAME PeerContextLifecycleTest COMMAND PeerContextLifecycleTest)

add_executable(VirtualSessionStateTest VirtualSessionStateTest.cpp)
target_include_directories(VirtualSessionStateTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionStateTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME VirtualSessionStateTest COMMAND VirtualSessionStateTest)

add_executable(VirtualSessionRegistryTest VirtualSessionRegistryTest.cpp)
target_include_directories(VirtualSessionRegistryTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionRegistryTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME VirtualSessionRegistryTest COMMAND VirtualSessionRegistryTest)

add_executable(VirtualSessionJournalTest VirtualSessionJournalTest.cpp)
target_include_directories(VirtualSessionJournalTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionJournalTest PRIVATE Qt6::Test KRdpGuardianClient)
target_link_options(VirtualSessionJournalTest PRIVATE "-Wl,--wrap=fsync")
add_test(NAME VirtualSessionJournalTest COMMAND VirtualSessionJournalTest)
add_executable(VirtualSessionMaintenanceGuardTest VirtualSessionMaintenanceGuardTest.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionMaintenanceGuard.cpp ${CMAKE_SOURCE_DIR}/server/PackageLease.cpp)
target_include_directories(VirtualSessionMaintenanceGuardTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionMaintenanceGuardTest PRIVATE Qt6::Test Qt6::Core)
target_link_options(VirtualSessionMaintenanceGuardTest PRIVATE "-Wl,--wrap=fsync" "-Wl,--wrap=write" "-Wl,--wrap=renameat" "-Wl,--wrap=fcntl" "-Wl,--wrap=flock")
add_test(NAME VirtualSessionMaintenanceGuardTest COMMAND VirtualSessionMaintenanceGuardTest)
set_tests_properties(VirtualSessionMaintenanceGuardTest PROPERTIES TIMEOUT 20)
add_executable(VirtualSessionMaintenanceRecordTest VirtualSessionMaintenanceRecordTest.cpp)
target_include_directories(VirtualSessionMaintenanceRecordTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionMaintenanceRecordTest PRIVATE Qt6::Core Qt6::Test)
add_test(NAME VirtualSessionMaintenanceRecordTest COMMAND VirtualSessionMaintenanceRecordTest)
add_executable(RuntimeProfileTest RuntimeProfileTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionRuntimeProfile.cpp)
target_include_directories(RuntimeProfileTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(RuntimeProfileTest PRIVATE Qt6::Core Qt6::Test)
add_test(NAME RuntimeProfileTest COMMAND RuntimeProfileTest)
set_tests_properties(RuntimeProfileTest PROPERTIES TIMEOUT 30)
add_executable(VirtualSessionCoordinatorIdentityTest VirtualSessionCoordinatorIdentityTest.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionCoordinatorIdentity.cpp)
target_include_directories(VirtualSessionCoordinatorIdentityTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionCoordinatorIdentityTest PRIVATE Qt6::Test Qt6::DBus Qt6::Core)
add_test(NAME VirtualSessionCoordinatorIdentityTest COMMAND VirtualSessionCoordinatorIdentityTest)
set_tests_properties(VirtualSessionCoordinatorIdentityTest PROPERTIES TIMEOUT 20)
add_executable(VirtualSessionKeeperProcessTest VirtualSessionKeeperProcessTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionKeeperProcess.cpp)
target_include_directories(VirtualSessionKeeperProcessTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionKeeperProcessTest PRIVATE Qt6::Test KRdpGuardianClient)
add_test(NAME VirtualSessionKeeperProcessTest COMMAND VirtualSessionKeeperProcessTest)
set_tests_properties(VirtualSessionKeeperProcessTest PROPERTIES TIMEOUT 10)
add_executable(VirtualSessionLoginRecoveryTest VirtualSessionLoginRecoveryTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionLoginRecovery.cpp)
target_include_directories(VirtualSessionLoginRecoveryTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionLoginRecoveryTest PRIVATE Qt6::Test Qt6::DBus KRdpGuardianClient)
add_test(NAME VirtualSessionLoginRecoveryTest COMMAND VirtualSessionLoginRecoveryTest)
set_tests_properties(VirtualSessionLoginRecoveryTest PROPERTIES TIMEOUT 30)

add_executable(VirtualSessionLaunchPlanTest VirtualSessionLaunchPlanTest.cpp)
target_include_directories(VirtualSessionLaunchPlanTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionLaunchPlanTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME VirtualSessionLaunchPlanTest COMMAND VirtualSessionLaunchPlanTest)

add_executable(VirtualSessionLoginTest VirtualSessionLoginTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionLogin.cpp)
add_executable(OwnerWatchProbe OwnerWatchProbe.cpp)
target_include_directories(OwnerWatchProbe PRIVATE ${CMAKE_SOURCE_DIR}/server)
add_executable(VirtualSessionOwnerWatchTest VirtualSessionOwnerWatchTest.cpp)
add_executable(VirtualSessionServiceScopeTest VirtualSessionServiceScopeTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionServiceScope.cpp)
target_include_directories(VirtualSessionServiceScopeTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionServiceScopeTest PRIVATE Qt6::Core Qt6::Test)
add_test(NAME VirtualSessionServiceScopeTest COMMAND VirtualSessionServiceScopeTest)
add_executable(ServiceOwnerChild ServiceOwnerChild.cpp)
target_link_libraries(ServiceOwnerChild PRIVATE Qt6::Core)
add_executable(VirtualSessionServiceOwnerTest VirtualSessionServiceOwnerTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionServiceOwner.cpp)
target_include_directories(VirtualSessionServiceOwnerTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionServiceOwnerTest PRIVATE Qt6::Test KRdpGuardianClient)
target_compile_definitions(VirtualSessionServiceOwnerTest PRIVATE SERVICE_OWNER_CHILD="$<TARGET_FILE:ServiceOwnerChild>")
add_dependencies(VirtualSessionServiceOwnerTest ServiceOwnerChild)
add_test(NAME VirtualSessionServiceOwnerTest COMMAND VirtualSessionServiceOwnerTest)
set_tests_properties(VirtualSessionServiceOwnerTest PROPERTIES TIMEOUT 30)
target_link_libraries(VirtualSessionOwnerWatchTest PRIVATE Qt6::Core Qt6::Test)
add_dependencies(VirtualSessionOwnerWatchTest OwnerWatchProbe)
add_test(NAME VirtualSessionOwnerWatchTest COMMAND VirtualSessionOwnerWatchTest)
set_tests_properties(VirtualSessionOwnerWatchTest PROPERTIES TIMEOUT 15)
target_include_directories(VirtualSessionLoginTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionLoginTest PRIVATE Qt6::Core Qt6::Test Qt6::DBus)
add_test(NAME VirtualSessionLoginTest COMMAND VirtualSessionLoginTest)
set_tests_properties(VirtualSessionLoginTest PROPERTIES TIMEOUT 30)

add_executable(VirtualSessionPamTest VirtualSessionPamTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionPam.cpp)
target_include_directories(VirtualSessionPamTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${PAM_INCLUDE_DIR})
target_link_libraries(VirtualSessionPamTest PRIVATE Qt6::Core Qt6::Test)
add_test(NAME VirtualSessionPamTest COMMAND VirtualSessionPamTest)

find_package(OpenSSL REQUIRED)
add_executable(VirtualSessionServicePlanTest VirtualSessionServicePlanTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualHostTls.cpp)
target_include_directories(VirtualSessionServicePlanTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionServicePlanTest PRIVATE Qt6::Test OpenSSL::Crypto KRdpGuardianClient)
target_compile_definitions(VirtualSessionServicePlanTest PRIVATE KRDP_SESSION_ENTRY="$<TARGET_FILE:krdp-virtual-session-entry>")
target_compile_definitions(VirtualSessionServicePlanTest PRIVATE KRDP_SESSION_UNIT="${CMAKE_BINARY_DIR}/server/krdp-virtual-session@.service")
target_compile_definitions(VirtualSessionServicePlanTest PRIVATE KRDP_VIRTUAL_HOST="$<TARGET_FILE:krdp-virtual-host>")
target_compile_definitions(VirtualSessionServicePlanTest PRIVATE KRDP_HOST_UNIT="${CMAKE_BINARY_DIR}/server/krdp-virtual-host.service")
add_dependencies(VirtualSessionServicePlanTest krdp-virtual-session-entry krdp-virtual-host)
add_dependencies(VirtualSessionServicePlanTest krdp-virtual-pam-keeper)
add_dependencies(VirtualSessionServicePlanTest krdp-virtual-session-cleanup)
target_compile_definitions(VirtualSessionServicePlanTest PRIVATE KRDP_SESSION_CLEANUP="$<TARGET_FILE:krdp-virtual-session-cleanup>")
target_compile_definitions(VirtualSessionServicePlanTest PRIVATE KRDP_PAM_KEEPER="$<TARGET_FILE:krdp-virtual-pam-keeper>")
add_test(NAME VirtualSessionServicePlanTest COMMAND VirtualSessionServicePlanTest)

add_executable(VirtualDeviceEntryTest VirtualDeviceEntryTest.cpp)
target_link_libraries(VirtualDeviceEntryTest PRIVATE Qt6::Test Qt6::Core)
target_compile_definitions(VirtualDeviceEntryTest PRIVATE KRDP_DEVICE_ENTRY="$<TARGET_FILE:krdp-virtual-device-entry>")
add_dependencies(VirtualDeviceEntryTest krdp-virtual-device-entry)
add_test(NAME VirtualDeviceEntryTest COMMAND VirtualDeviceEntryTest)

add_executable(VirtualSessionStorageTest VirtualSessionStorageTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionStorage.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionGuardian.cpp)
target_include_directories(VirtualSessionStorageTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionStorageTest PRIVATE Qt6::Test Qt6::Core Qt6::Network)
add_test(NAME VirtualSessionStorageTest COMMAND VirtualSessionStorageTest)

add_executable(VirtualSessionGuardianTest VirtualSessionGuardianTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionGuardian.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionStorage.cpp)
add_executable(GuardianSignalChild GuardianSignalChild.cpp)
target_link_libraries(GuardianSignalChild PRIVATE Qt6::Core)
target_include_directories(VirtualSessionGuardianTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionGuardianTest PRIVATE Qt6::Test Qt6::Core Qt6::Network)
add_test(NAME VirtualSessionGuardianTest COMMAND VirtualSessionGuardianTest)
add_dependencies(VirtualSessionGuardianTest krdp-virtual-guardian GuardianSignalChild)

add_executable(VirtualSessionGuardianClientTest VirtualSessionGuardianClientTest.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionGuardianClient.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionGuardian.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionStorage.cpp)
target_include_directories(VirtualSessionGuardianClientTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionGuardianClientTest PRIVATE Qt6::Test Qt6::Core Qt6::Network)
add_test(NAME VirtualSessionGuardianClientTest COMMAND VirtualSessionGuardianClientTest)

add_executable(VirtualSessionSupervisorTest VirtualSessionSupervisorTest.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionSupervisor.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionGuardian.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionStorage.cpp)
target_include_directories(VirtualSessionSupervisorTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionSupervisorTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME VirtualSessionSupervisorTest COMMAND VirtualSessionSupervisorTest)

add_executable(VirtualSessionControlTest VirtualSessionControlTest.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionControl.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionSupervisor.cpp)
target_include_directories(VirtualSessionControlTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualSessionControlTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME VirtualSessionControlTest COMMAND VirtualSessionControlTest)

add_executable(VirtualSessionTransportTest VirtualSessionTransportTest.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionTransport.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionControl.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionSupervisor.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp)
target_include_directories(VirtualSessionTransportTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(VirtualSessionTransportTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network KRdp)
add_test(NAME VirtualSessionTransportTest COMMAND VirtualSessionTransportTest)

add_executable(VirtualSessionHostControllerTest VirtualSessionHostControllerTest.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionGuardian.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionStorage.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionHostController.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionTransport.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionControl.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionSupervisor.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp)
target_include_directories(VirtualSessionHostControllerTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(VirtualSessionHostControllerTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus KRdp)
target_link_options(VirtualSessionHostControllerTest PRIVATE "-Wl,--wrap=fsync")
foreach(test VirtualSessionSupervisorTest VirtualSessionControlTest VirtualSessionTransportTest VirtualSessionHostControllerTest)
    target_link_libraries(${test} PRIVATE KRdpGuardianClient)
endforeach()
add_test(NAME VirtualSessionHostControllerTest COMMAND VirtualSessionHostControllerTest)

add_executable(CaptureWorkerModeTest CaptureWorkerModeTest.cpp)
target_include_directories(CaptureWorkerModeTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(CaptureWorkerModeTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME CaptureWorkerModeTest COMMAND CaptureWorkerModeTest)

find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
find_program(GDBUS_EXECUTABLE gdbus)
find_program(BASH_EXECUTABLE bash)
if(DBUS_RUN_SESSION_EXECUTABLE AND GDBUS_EXECUTABLE AND BASH_EXECUTABLE)
    add_test(NAME VirtualSessionBusTest COMMAND ${DBUS_RUN_SESSION_EXECUTABLE}
        --config-file=${CMAKE_SOURCE_DIR}/server/virtual-session-bus.conf
        -- ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/VirtualSessionBusTest.sh)
endif()

find_program(PIPEWIRE_EXECUTABLE pipewire)
if(PIPEWIRE_EXECUTABLE)
    add_executable(PipeWireMicrophoneTest PipeWireMicrophoneTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleMicrophoneSession.h ${CMAKE_SOURCE_DIR}/src/PipeWireMicrophone.cpp)
    target_include_directories(PipeWireMicrophoneTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
    target_link_libraries(PipeWireMicrophoneTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui PkgConfig::PipeWire)
    target_compile_definitions(PipeWireMicrophoneTest PRIVATE
        KRDP_AUDIO_CONFIG_DIR="${CMAKE_SOURCE_DIR}/server"
        KRDP_PIPEWIRE_EXECUTABLE="${PIPEWIRE_EXECUTABLE}")
    add_test(NAME PipeWireMicrophoneTest COMMAND PipeWireMicrophoneTest sourceReadinessAndTeardown)
    set_tests_properties(PipeWireMicrophoneTest PROPERTIES TIMEOUT 20)
endif()
find_program(PW_DUMP_EXECUTABLE pw-dump)
find_program(JQ_EXECUTABLE jq)
find_program(TIMEOUT_EXECUTABLE timeout)
if(PIPEWIRE_EXECUTABLE AND PW_DUMP_EXECUTABLE AND JQ_EXECUTABLE AND TIMEOUT_EXECUTABLE AND BASH_EXECUTABLE)
    add_test(NAME VirtualSessionAudioGraphTest COMMAND ${BASH_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/VirtualSessionAudioGraphTest.sh ${CMAKE_SOURCE_DIR}/server)
    set_tests_properties(VirtualSessionAudioGraphTest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15)
    find_program(WIREPLUMBER_EXECUTABLE wireplumber)
    find_program(PW_CAT_EXECUTABLE pw-cat)
    find_program(FFMPEG_EXECUTABLE ffmpeg)
    if(WIREPLUMBER_EXECUTABLE AND PW_CAT_EXECUTABLE AND DBUS_RUN_SESSION_EXECUTABLE)
        add_test(NAME PipeWireMicrophonePcmTest COMMAND ${DBUS_RUN_SESSION_EXECUTABLE}
            --config-file=${CMAKE_SOURCE_DIR}/server/virtual-session-bus.conf
            -- $<TARGET_FILE:PipeWireMicrophoneTest> sourcePcmDelivery)
        set_tests_properties(PipeWireMicrophonePcmTest PROPERTIES TIMEOUT 20)
    endif()
    if(WIREPLUMBER_EXECUTABLE AND PW_CAT_EXECUTABLE AND FFMPEG_EXECUTABLE AND DBUS_RUN_SESSION_EXECUTABLE)
        add_test(NAME VirtualSessionAudioPcmTest COMMAND ${DBUS_RUN_SESSION_EXECUTABLE}
            --config-file=${CMAKE_SOURCE_DIR}/server/virtual-session-bus.conf
            -- ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/VirtualSessionAudioGraphTest.sh
            ${CMAKE_SOURCE_DIR}/server --pcm)
        set_tests_properties(VirtualSessionAudioPcmTest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
        find_program(PAPLAY_EXECUTABLE paplay)
        find_program(PACTL_EXECUTABLE pactl)
        if(PAPLAY_EXECUTABLE AND PACTL_EXECUTABLE)
            add_test(NAME VirtualSessionAudioPulseTest COMMAND ${DBUS_RUN_SESSION_EXECUTABLE}
                --config-file=${CMAKE_SOURCE_DIR}/server/virtual-session-bus.conf
                -- ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/VirtualSessionAudioGraphTest.sh
                ${CMAKE_SOURCE_DIR}/server --pulse)
            set_tests_properties(VirtualSessionAudioPulseTest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 25)
        endif()
    endif()
endif()

add_executable(SurfaceLayoutTest SurfaceLayoutTest.cpp)
target_include_directories(SurfaceLayoutTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(SurfaceLayoutTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME SurfaceLayoutTest COMMAND SurfaceLayoutTest)

add_executable(MultiLayoutTest MultiLayoutTest.cpp)
target_include_directories(MultiLayoutTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(MultiLayoutTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME MultiLayoutTest COMMAND MultiLayoutTest)

add_executable(FrameQueuePolicyTest FrameQueuePolicyTest.cpp)
target_include_directories(FrameQueuePolicyTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(FrameQueuePolicyTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME FrameQueuePolicyTest COMMAND FrameQueuePolicyTest)

add_executable(ClientDisplayInfoTest ClientDisplayInfoTest.cpp)
target_include_directories(ClientDisplayInfoTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ClientDisplayInfoTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ClientDisplayInfoTest COMMAND ClientDisplayInfoTest)

add_executable(OutputSnapshotTest OutputSnapshotTest.cpp)
# ${CMAKE_BINARY_DIR}/src for krdp_export.h: OutputSnapshot.h now includes LayoutControl.h
# (hostMonitorsFrom); header-only use, nothing from libKRdp is linked.
target_include_directories(OutputSnapshotTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
target_link_libraries(OutputSnapshotTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME OutputSnapshotTest COMMAND OutputSnapshotTest)

add_executable(TakeoverDetectorTest TakeoverDetectorTest.cpp)
target_include_directories(TakeoverDetectorTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(TakeoverDetectorTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME TakeoverDetectorTest COMMAND TakeoverDetectorTest)

# The physical-console host reads logind asynchronously, but its adapter
# selection is deliberately pure: test the greeter -> seat0-user handoff
# without requiring a display manager in the test environment.
add_executable(ConsoleSeatTest ConsoleSeatTest.cpp)
target_include_directories(ConsoleSeatTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleSeatTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ConsoleSeatTest COMMAND ConsoleSeatTest)

add_executable(ConsoleControlTest ConsoleControlTest.cpp)
target_include_directories(ConsoleControlTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleControlTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ConsoleControlTest COMMAND ConsoleControlTest)

add_executable(ConsoleResizeTest ConsoleResizeTest.cpp)
target_include_directories(ConsoleResizeTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleResizeTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ConsoleResizeTest COMMAND ConsoleResizeTest)

add_executable(ConsoleResizeExecutorTest ConsoleResizeExecutorTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleResizeExecutor.cpp)
target_include_directories(ConsoleResizeExecutorTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleResizeExecutorTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ConsoleResizeExecutorTest COMMAND ConsoleResizeExecutorTest)

add_executable(ConsoleResizeSessionTest ConsoleResizeSessionTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleResizeExecutor.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleResizeSession.cpp)
target_include_directories(ConsoleResizeSessionTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ConsoleResizeSessionTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME ConsoleResizeSessionTest COMMAND ConsoleResizeSessionTest)

add_executable(ConsoleInputStateTest ConsoleInputStateTest.cpp)
target_include_directories(ConsoleInputStateTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleInputStateTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME ConsoleInputStateTest COMMAND ConsoleInputStateTest)

# The future persistent console broker uses this state machine to revoke the
# outgoing adapter before an SDDM/Plasma worker can be replaced.
add_executable(ConsoleHandoffTest ConsoleHandoffTest.cpp)
target_include_directories(ConsoleHandoffTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleHandoffTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ConsoleHandoffTest COMMAND ConsoleHandoffTest)

add_executable(ConsoleWorkerWireTest ConsoleWorkerWireTest.cpp)
target_include_directories(ConsoleWorkerWireTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleWorkerWireTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME ConsoleWorkerWireTest COMMAND ConsoleWorkerWireTest)

add_executable(ConsoleWorkerEndpointTest ConsoleWorkerEndpointTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp)
target_include_directories(ConsoleWorkerEndpointTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleWorkerEndpointTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network)
add_test(NAME ConsoleWorkerEndpointTest COMMAND ConsoleWorkerEndpointTest)

add_executable(ConsoleWorkerSessionTest ConsoleWorkerSessionTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp)
target_include_directories(ConsoleWorkerSessionTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleWorkerSessionTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui KRdp)
add_test(NAME ConsoleWorkerSessionTest COMMAND ConsoleWorkerSessionTest)

add_executable(ConsoleHostControllerTest ConsoleHostControllerTest.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleHostController.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleSeat.cpp)
target_include_directories(ConsoleHostControllerTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleHostControllerTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus KRdp)
add_test(NAME ConsoleHostControllerTest COMMAND ConsoleHostControllerTest)

add_executable(RdpAudioPriorityTest RdpAudioPriorityTest.cpp)
target_include_directories(RdpAudioPriorityTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(RdpAudioPriorityTest PRIVATE Qt6::Test Qt6::Core KRdp)
add_test(NAME RdpAudioPriorityTest COMMAND RdpAudioPriorityTest)

add_executable(PipeWireAudioRoutingTest PipeWireAudioRoutingTest.cpp)
target_include_directories(PipeWireAudioRoutingTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(PipeWireAudioRoutingTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME PipeWireAudioRoutingTest COMMAND PipeWireAudioRoutingTest)

add_executable(ClipboardTextTest ClipboardTextTest.cpp)
target_include_directories(ClipboardTextTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ClipboardTextTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ClipboardTextTest COMMAND ClipboardTextTest)

# LayoutControl has an out-of-line .cpp (codec + planner), unlike its
# header-only neighbours above, so it is compiled straight into the test
# binary rather than linked through the KRdp library (which would drag in
# FreeRDP/WinPR/PAM for a pure-logic test).
add_executable(LayoutControlTest LayoutControlTest.cpp ${CMAKE_SOURCE_DIR}/src/LayoutControl.cpp)
target_include_directories(LayoutControlTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
target_link_libraries(LayoutControlTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME LayoutControlTest COMMAND LayoutControlTest)

# LayoutOwner is header-only (server/); ${CMAKE_BINARY_DIR}/src for krdp_export.h via LayoutControl.h.
add_executable(LayoutOwnerTest LayoutOwnerTest.cpp)
target_include_directories(LayoutOwnerTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
target_link_libraries(LayoutOwnerTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME LayoutOwnerTest COMMAND LayoutOwnerTest)

# LayoutSessionDiff is header-only (server/): the per-connection session diff a live KRDPCTL apply runs.
add_executable(LayoutSessionDiffTest LayoutSessionDiffTest.cpp)
target_include_directories(LayoutSessionDiffTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(LayoutSessionDiffTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME LayoutSessionDiffTest COMMAND LayoutSessionDiffTest)

# VideoCodecSupport.h uses the RDPGFX enums: FreeRDP headers only, nothing linked.
# FreeRDP/WinPR dirs are SYSTEM (matching src/CMakeLists.txt's -isystem for the
# same headers): plain PRIVATE include dirs surfaced -Wpedantic/-Wdeprecated-declarations
# warnings from wtypes.h/sspi.h that the main library targets do not see.
add_executable(VideoCodecSupportTest VideoCodecSupportTest.cpp)
target_include_directories(VideoCodecSupportTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(VideoCodecSupportTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(VideoCodecSupportTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME VideoCodecSupportTest COMMAND VideoCodecSupportTest)

# GfxSurfaceCommand.h uses the RDPGFX bitmap stream structs (FreeRDP headers only, nothing
# linked) plus QSize, so it needs Qt6::Gui in addition to VideoCodecSupportTest's set above.
add_executable(GfxSurfaceCommandTest GfxSurfaceCommandTest.cpp)
target_include_directories(GfxSurfaceCommandTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(GfxSurfaceCommandTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(GfxSurfaceCommandTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME GfxSurfaceCommandTest COMMAND GfxSurfaceCommandTest)
