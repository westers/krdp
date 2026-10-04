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
target_include_directories(VirtualSessionControlTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(VirtualSessionControlTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME VirtualSessionControlTest COMMAND VirtualSessionControlTest)

add_executable(VirtualSessionTransportTest VirtualSessionTransportTest.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionTransport.cpp ${CMAKE_SOURCE_DIR}/server/VirtualStockClient.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionControl.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionSupervisor.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp ${CMAKE_SOURCE_DIR}/server/WorkerCodecBridge.cpp ${CMAKE_SOURCE_DIR}/server/VirtualResize.cpp)
target_include_directories(VirtualSessionTransportTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(VirtualSessionTransportTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network KRdp)
add_test(NAME VirtualSessionTransportTest COMMAND VirtualSessionTransportTest)

# AUD-D4: stock RDP clients on the virtual broker (policy, error codes, first layout, DISP size).
add_executable(VirtualStockClientTest VirtualStockClientTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualStockClient.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualResize.cpp)
target_include_directories(VirtualStockClientTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(VirtualStockClientTest PRIVATE Qt6::Test Qt6::Core KRdp KRdpGuardianClient)
add_test(NAME VirtualStockClientTest COMMAND VirtualStockClientTest)

add_executable(VirtualSessionHostControllerTest VirtualSessionHostControllerTest.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionGuardian.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionStorage.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionHostController.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionTransport.cpp ${CMAKE_SOURCE_DIR}/server/VirtualStockClient.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualSessionControl.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionSupervisor.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp ${CMAKE_SOURCE_DIR}/server/WorkerCodecBridge.cpp
    ${CMAKE_SOURCE_DIR}/server/VirtualResize.cpp)
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

find_program(KREADCONFIG6_EXECUTABLE kreadconfig6)
if(KREADCONFIG6_EXECUTABLE AND BASH_EXECUTABLE)
    add_test(NAME VirtualSessionLockConfigTest COMMAND ${BASH_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/VirtualSessionLockConfigTest.sh ${KREADCONFIG6_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/scripts/virtual-session-locked-config ${CMAKE_SOURCE_DIR}/scripts/launch-virtual-session.sh)
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
        # N1: the microphone's `inUse` follows linked consumers and clears when the last one leaves.
        add_test(NAME PipeWireMicrophoneInUseTest COMMAND ${DBUS_RUN_SESSION_EXECUTABLE}
            --config-file=${CMAKE_SOURCE_DIR}/server/virtual-session-bus.conf
            -- $<TARGET_FILE:PipeWireMicrophoneTest> inUseFollowsLinkedConsumers)
        set_tests_properties(PipeWireMicrophoneInUseTest PROPERTIES TIMEOUT 40)
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

# AUD-FIX12: the encoder watchdog and the silent-surface rule (no surface silent > 2 s).
# OPT-055 K4.1: what a session does on encoderFailed, and the idle self-restart decision.
add_executable(EncoderFailurePolicyTest EncoderFailurePolicyTest.cpp)
target_include_directories(EncoderFailurePolicyTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(EncoderFailurePolicyTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME EncoderFailurePolicyTest COMMAND EncoderFailurePolicyTest)
set_tests_properties(EncoderFailurePolicyTest PROPERTIES TIMEOUT 60)

add_executable(EncoderWatchdogTest EncoderWatchdogTest.cpp)
target_include_directories(EncoderWatchdogTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(EncoderWatchdogTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME EncoderWatchdogTest COMMAND EncoderWatchdogTest)
set_tests_properties(EncoderWatchdogTest PROPERTIES TIMEOUT 60)

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

add_executable(RemoteTopologyCatalogTest RemoteTopologyCatalogTest.cpp)
target_include_directories(RemoteTopologyCatalogTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(RemoteTopologyCatalogTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME RemoteTopologyCatalogTest COMMAND RemoteTopologyCatalogTest)

add_executable(RemoteTopologyDraftTest RemoteTopologyDraftTest.cpp)
target_include_directories(RemoteTopologyDraftTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(RemoteTopologyDraftTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME RemoteTopologyDraftTest COMMAND RemoteTopologyDraftTest)

add_executable(VirtualInitialBootstrapTest VirtualInitialBootstrapTest.cpp)
target_include_directories(VirtualInitialBootstrapTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(VirtualInitialBootstrapTest PRIVATE Qt6::Test Qt6::Gui Qt6::Network)
add_test(NAME VirtualInitialBootstrapTest COMMAND VirtualInitialBootstrapTest)

add_executable(ConsoleTopologyPlanTest ConsoleTopologyPlanTest.cpp)
target_include_directories(ConsoleTopologyPlanTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ConsoleTopologyPlanTest PRIVATE Qt6::Test Qt6::Gui)
add_test(NAME ConsoleTopologyPlanTest COMMAND ConsoleTopologyPlanTest)

add_executable(ConsoleTopologyReadbackTest ConsoleTopologyReadbackTest.cpp ${CMAKE_SOURCE_DIR}/server/H264KeyframeSize.cpp ${CMAKE_SOURCE_DIR}/server/VirtualResize.cpp)
target_include_directories(ConsoleTopologyReadbackTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ConsoleTopologyReadbackTest PRIVATE Qt6::Test Qt6::Gui PkgConfig::AVCodec)
add_test(NAME ConsoleTopologyReadbackTest COMMAND ConsoleTopologyReadbackTest)

add_executable(RemoteTopologyFitTest RemoteTopologyFitTest.cpp)
target_include_directories(RemoteTopologyFitTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(RemoteTopologyFitTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME RemoteTopologyFitTest COMMAND RemoteTopologyFitTest)

add_executable(VirtualInitialLayoutTest VirtualInitialLayoutTest.cpp)
target_include_directories(VirtualInitialLayoutTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(VirtualInitialLayoutTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME VirtualInitialLayoutTest COMMAND VirtualInitialLayoutTest)

add_executable(RetainedKScreenReadbackTest RetainedKScreenReadbackTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualResize.cpp ${CMAKE_SOURCE_DIR}/server/H264KeyframeSize.cpp ${CMAKE_SOURCE_DIR}/server/OutputRestoreJournal.cpp)
target_include_directories(RetainedKScreenReadbackTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
target_link_libraries(RetainedKScreenReadbackTest PRIVATE Qt6::Test Qt6::Gui PkgConfig::AVCodec)
add_test(NAME RetainedKScreenReadbackTest COMMAND RetainedKScreenReadbackTest)

add_executable(RemoteTopologyProtocolTest RemoteTopologyProtocolTest.cpp)
target_include_directories(RemoteTopologyProtocolTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(RemoteTopologyProtocolTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME RemoteTopologyProtocolTest COMMAND RemoteTopologyProtocolTest)

add_executable(RetainedMultiCaptureTest RetainedMultiCaptureTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualResize.cpp ${CMAKE_SOURCE_DIR}/server/H264KeyframeSize.cpp)
target_include_directories(RetainedMultiCaptureTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(RetainedMultiCaptureTest PRIVATE Qt6::Test Qt6::Gui PkgConfig::AVCodec)
add_test(NAME RetainedMultiCaptureTest COMMAND RetainedMultiCaptureTest)

add_executable(RetainedMultiInputTest RetainedMultiInputTest.cpp)
target_include_directories(RetainedMultiInputTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(RetainedMultiInputTest PRIVATE Qt6::Test Qt6::Gui)
add_test(NAME RetainedMultiInputTest COMMAND RetainedMultiInputTest)

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

add_executable(ConsoleSeatWatcherTest ConsoleSeatWatcherTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleSeat.cpp)
target_include_directories(ConsoleSeatWatcherTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleSeatWatcherTest PRIVATE Qt6::Test Qt6::Core Qt6::DBus)
add_test(NAME ConsoleSeatWatcherTest COMMAND ConsoleSeatWatcherTest)

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

add_executable(ConsoleResizeSessionTest ConsoleResizeSessionTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleResizeExecutor.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleResizeSession.cpp ${CMAKE_SOURCE_DIR}/server/OutputRestoreJournal.cpp)
target_include_directories(ConsoleResizeSessionTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ConsoleResizeSessionTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME ConsoleResizeSessionTest COMMAND ConsoleResizeSessionTest)

add_executable(VirtualResizeTest VirtualResizeTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualResize.cpp ${CMAKE_SOURCE_DIR}/server/VirtualResizeExecutor.cpp)
# AUD-FIX11 R6: no packets on a surface before a keyframe of their codec with in-band headers.
add_executable(SurfaceChainTest SurfaceChainTest.cpp)
target_include_directories(SurfaceChainTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(SurfaceChainTest PRIVATE Qt6::Test Qt6::Core PkgConfig::AVCodec)
add_test(NAME SurfaceChainTest COMMAND SurfaceChainTest)
set_tests_properties(SurfaceChainTest PROPERTIES TIMEOUT 30)

add_executable(H264KeyframeSizeTest H264KeyframeSizeTest.cpp ${CMAKE_SOURCE_DIR}/server/H264KeyframeSize.cpp)
target_include_directories(H264KeyframeSizeTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(H264KeyframeSizeTest PRIVATE Qt6::Test Qt6::Core PkgConfig::AVCodec)
add_test(NAME H264KeyframeSizeTest COMMAND H264KeyframeSizeTest)
set_tests_properties(H264KeyframeSizeTest PROPERTIES TIMEOUT 30)
target_include_directories(VirtualResizeTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(VirtualResizeTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME VirtualResizeTest COMMAND VirtualResizeTest)
add_executable(VirtualResizeSessionTest VirtualResizeSessionTest.cpp ${CMAKE_SOURCE_DIR}/server/VirtualResize.cpp ${CMAKE_SOURCE_DIR}/server/VirtualResizeExecutor.cpp ${CMAKE_SOURCE_DIR}/server/VirtualResizeSession.cpp)
target_include_directories(VirtualResizeSessionTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(VirtualResizeSessionTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME VirtualResizeSessionTest COMMAND VirtualResizeSessionTest)
set_tests_properties(VirtualResizeTest VirtualResizeSessionTest PROPERTIES TIMEOUT 30)

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

add_executable(ConsoleVirtualOutputPlanTest ConsoleVirtualOutputPlanTest.cpp)
target_include_directories(ConsoleVirtualOutputPlanTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
target_link_libraries(ConsoleVirtualOutputPlanTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME ConsoleVirtualOutputPlanTest COMMAND ConsoleVirtualOutputPlanTest)

add_executable(ConsoleWorkerEndpointTest ConsoleWorkerEndpointTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp)
target_include_directories(ConsoleWorkerEndpointTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleWorkerEndpointTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network)
add_test(NAME ConsoleWorkerEndpointTest COMMAND ConsoleWorkerEndpointTest)

# FIX-CURSOR: the cursor shape tracker, the worker wire's Cursor record, the outbox and the endpoint.
add_executable(CursorShapeTest CursorShapeTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp)
target_include_directories(CursorShapeTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
target_link_libraries(CursorShapeTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network KRdp)
add_test(NAME CursorShapeTest COMMAND CursorShapeTest)
set_tests_properties(CursorShapeTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# AUD-FIX8 B3: render nodes in a virtual desktop's sandbox (bwrap), groups, VaapiDriverMode.
add_executable(RenderAccessTest RenderAccessTest.cpp)
target_include_directories(RenderAccessTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
target_link_libraries(RenderAccessTest PRIVATE Qt6::Test Qt6::Core KRdp)
add_test(NAME RenderAccessTest COMMAND RenderAccessTest)
set_tests_properties(RenderAccessTest PROPERTIES TIMEOUT 120)

# AUD-FIX8: end to end - the real krdp-console-worker (sandboxed like a virtual desktop's) in a
# private headless session (own D-Bus without activation, PipeWire, WirePlumber, kwin_wayland
# --virtual) against the real broker endpoint: Ready, encoder caps and H.264 keyframes.
if(TARGET krdp-console-worker)
    # FIX-CURSOR: a Wayland window that changes its cursor shape on request.
    add_executable(krdp-e2e-cursor-client CursorShapeClient.cpp)
    target_link_libraries(krdp-e2e-cursor-client PRIVATE Qt6::Gui)
    # OPT-055 T-K4b: test-only LD_PRELOAD shim that makes av_buffersink_get_frame fail (never installed).
    add_library(farside-failfilter MODULE FailFilterShim.cpp)
    target_link_libraries(farside-failfilter PRIVATE ${CMAKE_DL_LIBS})
    add_executable(WorkerEndToEndTest WorkerEndToEndTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp
        ${CMAKE_SOURCE_DIR}/server/H264KeyframeSize.cpp ${CMAKE_SOURCE_DIR}/server/OutputRestoreJournal.cpp
        ${CMAKE_SOURCE_DIR}/server/VirtualResize.cpp)
    target_include_directories(WorkerEndToEndTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
    target_link_libraries(WorkerEndToEndTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network KRdp PkgConfig::AVCodec)
    target_compile_definitions(WorkerEndToEndTest PRIVATE
        KRDP_CONSOLE_WORKER="$<TARGET_FILE:krdp-console-worker>"
        KRDP_SERVER_DIR="${CMAKE_SOURCE_DIR}/server"
        KRDP_E2E_CURSOR_CLIENT="$<TARGET_FILE:krdp-e2e-cursor-client>"
        KRDP_E2E_FAILFILTER="$<TARGET_FILE:farside-failfilter>"
        KRDP_E2E_SERVER="$<TARGET_FILE:krdpserver>")
    add_dependencies(WorkerEndToEndTest krdp-console-worker krdp-e2e-cursor-client farside-failfilter krdpserver)
    # OPT-055 K4 user-server cases need the FreeRDP client probe; without it they skip.
    if(TARGET krdpctl-probe)
        target_compile_definitions(WorkerEndToEndTest PRIVATE KRDP_E2E_PROBE="$<TARGET_FILE:krdpctl-probe>")
        add_dependencies(WorkerEndToEndTest krdpctl-probe)
    endif()
    add_test(NAME WorkerEndToEndTest COMMAND WorkerEndToEndTest)
    set_tests_properties(WorkerEndToEndTest PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
endif()

# AUD-FIX8: the worker -> broker order (ConsoleWorkerOutbox, the class the real worker sends through).
add_executable(ConsoleWorkerOrderTest ConsoleWorkerOrderTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp)
target_include_directories(ConsoleWorkerOrderTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleWorkerOrderTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network)
add_test(NAME ConsoleWorkerOrderTest COMMAND ConsoleWorkerOrderTest)

add_executable(ConsoleWorkerSessionTest ConsoleWorkerSessionTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp)
target_include_directories(ConsoleWorkerSessionTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleWorkerSessionTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui KRdp)
add_test(NAME ConsoleWorkerSessionTest COMMAND ConsoleWorkerSessionTest)

add_executable(ConsoleHostControllerTest ConsoleHostControllerTest.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleHostController.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp ${CMAKE_SOURCE_DIR}/server/WorkerCodecBridge.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleSeat.cpp)
target_include_directories(ConsoleHostControllerTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleHostControllerTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus KRdp)
add_test(NAME ConsoleHostControllerTest COMMAND ConsoleHostControllerTest)

# Manual, bounded Sol/Buzz RDP fixture; not installed or part of automatic ctest.
add_executable(ConsoleOwnedRdpHost ConsoleOwnedRdpHost.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleHostController.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp ${CMAKE_SOURCE_DIR}/server/WorkerCodecBridge.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleSeat.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleSeatWatcher.h
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerLauncher.cpp)
target_include_directories(ConsoleOwnedRdpHost PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleOwnedRdpHost PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus KRdp KRdpBrokerAuthentication KRdpBrokerUserSettings)

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
add_executable(AvcCodecSelectionTest AvcCodecSelectionTest.cpp)
target_include_directories(AvcCodecSelectionTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(AvcCodecSelectionTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(AvcCodecSelectionTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME AvcCodecSelectionTest COMMAND AvcCodecSelectionTest)

# GfxSurfaceCommand.h uses the RDPGFX bitmap stream structs (FreeRDP headers only, nothing
# linked) plus QSize, so it needs Qt6::Gui in addition to VideoCodecSupportTest's set above.
add_executable(GfxSurfaceCommandTest GfxSurfaceCommandTest.cpp)
target_include_directories(GfxSurfaceCommandTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(GfxSurfaceCommandTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(GfxSurfaceCommandTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME GfxSurfaceCommandTest COMMAND GfxSurfaceCommandTest)

# --- AUD-WS-S ---
# AUD-S1: the pre-authentication channel gate, fed through a fake freerdp_peer.
add_executable(PreAuthChannelGateTest PreAuthChannelGateTest.cpp)
target_include_directories(PreAuthChannelGateTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
target_include_directories(PreAuthChannelGateTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(PreAuthChannelGateTest PRIVATE Qt6::Test Qt6::Core KRdp freerdp winpr)
add_test(NAME PreAuthChannelGateTest COMMAND PreAuthChannelGateTest)

# AUD-S1/S2/S3: a real Server on 127.0.0.1 with krdpctl-probe as the client.
if(TARGET krdpctl-probe)
    add_executable(RdpPreAuthLoopbackTest RdpPreAuthLoopbackTest.cpp)
    target_include_directories(RdpPreAuthLoopbackTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
    target_include_directories(RdpPreAuthLoopbackTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
    target_link_libraries(RdpPreAuthLoopbackTest PRIVATE Qt6::Test Qt6::Core Qt6::Network KRdp)
    target_compile_definitions(RdpPreAuthLoopbackTest PRIVATE KRDPCTL_PROBE="$<TARGET_FILE:krdpctl-probe>")
    add_dependencies(RdpPreAuthLoopbackTest krdpctl-probe)
    add_test(NAME RdpPreAuthLoopbackTest COMMAND RdpPreAuthLoopbackTest)
    set_tests_properties(RdpPreAuthLoopbackTest PROPERTIES TIMEOUT 180)
endif()

# AUD-FIX4 D1: the in-flight window on a throttled and then blocked link, end to end.
if(TARGET krdpctl-probe)
    add_executable(VideoFlowLoopbackTest VideoFlowLoopbackTest.cpp)
    target_include_directories(VideoFlowLoopbackTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
    target_include_directories(VideoFlowLoopbackTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
    target_link_libraries(VideoFlowLoopbackTest PRIVATE Qt6::Test Qt6::Core Qt6::Network KRdp)
    target_compile_definitions(VideoFlowLoopbackTest PRIVATE KRDPCTL_PROBE="$<TARGET_FILE:krdpctl-probe>")
    add_dependencies(VideoFlowLoopbackTest krdpctl-probe)
    add_test(NAME VideoFlowLoopbackTest COMMAND VideoFlowLoopbackTest)
    set_tests_properties(VideoFlowLoopbackTest PROPERTIES TIMEOUT 180)
endif()

# FIX-CURSOR: a worker's Cursor records become RDP pointer updates at a real libfreerdp client.
if(TARGET krdpctl-probe)
    add_executable(CursorLoopbackTest CursorLoopbackTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp)
    target_include_directories(CursorLoopbackTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
    target_include_directories(CursorLoopbackTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
    target_link_libraries(CursorLoopbackTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network KRdp)
    target_compile_definitions(CursorLoopbackTest PRIVATE KRDPCTL_PROBE="$<TARGET_FILE:krdpctl-probe>")
    add_dependencies(CursorLoopbackTest krdpctl-probe)
    add_test(NAME CursorLoopbackTest COMMAND CursorLoopbackTest)
    set_tests_properties(CursorLoopbackTest PROPERTIES TIMEOUT 180 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# AUD-FIX5: a stock client that stalls on (or ignores) a clipboard request still gets its picture.
if(TARGET krdpctl-probe)
    add_executable(ClipboardStallLoopbackTest ClipboardStallLoopbackTest.cpp)
    target_include_directories(ClipboardStallLoopbackTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
    target_include_directories(ClipboardStallLoopbackTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
    target_link_libraries(ClipboardStallLoopbackTest PRIVATE Qt6::Test Qt6::Core Qt6::Network KRdp)
    target_compile_definitions(ClipboardStallLoopbackTest PRIVATE KRDPCTL_PROBE="$<TARGET_FILE:krdpctl-probe>")
    add_dependencies(ClipboardStallLoopbackTest krdpctl-probe)
    add_test(NAME ClipboardStallLoopbackTest COMMAND ClipboardStallLoopbackTest)
    set_tests_properties(ClipboardStallLoopbackTest PROPERTIES TIMEOUT 180)
endif()

# AUD-S4: ListenAddress parsing (header-only, server/).
add_executable(ListenAddressTest ListenAddressTest.cpp)
target_include_directories(ListenAddressTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ListenAddressTest PRIVATE Qt6::Test Qt6::Core Qt6::Network)
add_test(NAME ListenAddressTest COMMAND ListenAddressTest)
# --- end AUD-WS-S ---

# --- AUD-WS-P ---
# Physical direct session fixes (AUDIT-FIX-PLAN.md WS-P). Pure headers except
# where noted.
add_executable(ScreencastTargetTest ScreencastTargetTest.cpp)
target_include_directories(ScreencastTargetTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ScreencastTargetTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME ScreencastTargetTest COMMAND ScreencastTargetTest)

add_executable(PressedInputTrackerTest PressedInputTrackerTest.cpp)
target_include_directories(PressedInputTrackerTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(PressedInputTrackerTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME PressedInputTrackerTest COMMAND PressedInputTrackerTest)

# Links libKRdp: a real Server and RdpConnection over a socketpair.
add_executable(RdpConnectionInitTest RdpConnectionInitTest.cpp)
target_include_directories(RdpConnectionInitTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(RdpConnectionInitTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(RdpConnectionInitTest PRIVATE Qt6::Test Qt6::Core Qt6::Network KRdp freerdp winpr ${CMAKE_DL_LIBS})
add_test(NAME RdpConnectionInitTest COMMAND RdpConnectionInitTest)
set_tests_properties(RdpConnectionInitTest PROPERTIES TIMEOUT 60)

# Links libKRdp: drives Clipboard's cliprdr callbacks from another thread.
add_executable(ClipboardHandoffTest ClipboardHandoffTest.cpp)
target_include_directories(ClipboardHandoffTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(ClipboardHandoffTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(ClipboardHandoffTest PRIVATE Qt6::Test Qt6::Core KRdp)
add_test(NAME ClipboardHandoffTest COMMAND ClipboardHandoffTest)
set_tests_properties(ClipboardHandoffTest PROPERTIES TIMEOUT 30)

# --- AUD-WS-K ---
# K13: one krdpserverrc schema, owned by server/ and used by the KCM.
add_test(NAME KcfgSingleSourceTest
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/check-single-kcfg.cmake)

# K3: certificate generation and renewal decisions with an injected clock.
add_executable(ServerCertificateTest ServerCertificateTest.cpp)
target_include_directories(ServerCertificateTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(ServerCertificateTest PRIVATE Qt6::Test Qt6::Core KRdpServerCertificate freerdp winpr)
add_test(NAME ServerCertificateTest COMMAND ServerCertificateTest)

# K4/K5: port range and backend choice shared by krdpserver and the KCM.
add_executable(ServerSettingsPolicyTest ServerSettingsPolicyTest.cpp)
target_include_directories(ServerSettingsPolicyTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ServerSettingsPolicyTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ServerSettingsPolicyTest COMMAND ServerSettingsPolicyTest)

# K1/K2/K5/K6/K9/K10: the KCM's logic (kcm_krdpserver_core), with a fake keychain.
add_executable(KcmSettingsTest KcmSettingsTest.cpp)
target_link_libraries(KcmSettingsTest PRIVATE Qt6::Test Qt6::Core kcm_krdpserver_core)
add_test(NAME KcmSettingsTest COMMAND KcmSettingsTest)

# FARSIDE-KCM: Farside next to KDE's own remote desktop (stock detection and
# the page's actions, against a fake /proc and a fake systemd), host checks.
add_executable(KcmCoexistenceTest KcmCoexistenceTest.cpp)
target_link_libraries(KcmCoexistenceTest PRIVATE Qt6::Test Qt6::Core kcm_krdpserver_core)
add_test(NAME KcmCoexistenceTest COMMAND KcmCoexistenceTest)

# FARSIDE-KCM: the built settings page and its sub-pages load without warnings,
# and the main page's key items are reachable and don't overlap at 1280x800.
# Offscreen, with a throwaway config and no session bus: nothing of the user's
# (config, keychain, services) is read or touched.
find_package(Qt6 REQUIRED COMPONENTS Quick Qml Widgets)
add_executable(KcmUiTest KcmUiTest.cpp)
target_link_libraries(KcmUiTest PRIVATE Qt6::Test Qt6::Quick Qt6::Qml Qt6::Widgets KF6::KCMUtilsQuick KF6::CoreAddons KRdpBrokerHostPublicSnapshot)
target_compile_definitions(KcmUiTest PRIVATE KCM_PLUGIN_PATH="$<TARGET_FILE:kcm_farside>")
add_dependencies(KcmUiTest kcm_farside)
add_test(NAME KcmUiTest COMMAND KcmUiTest)
set(_kcm_ui_home ${CMAKE_CURRENT_BINARY_DIR}/KcmUiTest-home)
file(MAKE_DIRECTORY ${_kcm_ui_home}/run)
file(CHMOD ${_kcm_ui_home}/run DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set_tests_properties(KcmUiTest PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=org.kde.desktop;XDG_CONFIG_HOME=${_kcm_ui_home}/config;XDG_DATA_HOME=${_kcm_ui_home}/data;XDG_STATE_HOME=${_kcm_ui_home}/state;XDG_CACHE_HOME=${_kcm_ui_home}/cache;XDG_RUNTIME_DIR=${_kcm_ui_home}/run;DBUS_SESSION_BUS_ADDRESS=unix:path=${_kcm_ui_home}/no-bus;LANGUAGE=en_US;LC_ALL=C.UTF-8")

# --- AUD-WS-C ---
# Console broker/worker audit fixes (AUD-C-1 .. AUD-C-10).
add_executable(ConsoleWorkerEndpointHardeningTest ConsoleWorkerEndpointHardeningTest.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp)
target_include_directories(ConsoleWorkerEndpointHardeningTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleWorkerEndpointHardeningTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network)
add_test(NAME ConsoleWorkerEndpointHardeningTest COMMAND ConsoleWorkerEndpointHardeningTest)
add_executable(OutputRestoreJournalTest OutputRestoreJournalTest.cpp ${CMAKE_SOURCE_DIR}/server/OutputRestoreJournal.cpp)
target_include_directories(OutputRestoreJournalTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(OutputRestoreJournalTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME OutputRestoreJournalTest COMMAND OutputRestoreJournalTest)
add_executable(ConsoleOutputJournalTest ConsoleOutputJournalTest.cpp
    ${CMAKE_SOURCE_DIR}/server/OutputRestoreJournal.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleResizeExecutor.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleResizeSession.cpp
    ${CMAKE_SOURCE_DIR}/server/PhysicalOutputGuard.cpp)
target_include_directories(ConsoleOutputJournalTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleOutputJournalTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui KRdp)
add_test(NAME ConsoleOutputJournalTest COMMAND ConsoleOutputJournalTest)
set_tests_properties(ConsoleOutputJournalTest PROPERTIES TIMEOUT 60)
add_executable(ConsoleHostLifecycleTest ConsoleHostLifecycleTest.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleHostController.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp ${CMAKE_SOURCE_DIR}/server/WorkerCodecBridge.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleSeat.cpp)
target_include_directories(ConsoleHostLifecycleTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleHostLifecycleTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus KRdp)
add_test(NAME ConsoleHostLifecycleTest COMMAND ConsoleHostLifecycleTest)
# --- AUD-WS-L ---
# KRDPCTL v2: requestId, capabilities, chroma timing (pure).
add_executable(KrdpctlV2Test KrdpctlV2Test.cpp)
target_include_directories(KrdpctlV2Test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
target_include_directories(KrdpctlV2Test SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(KrdpctlV2Test PRIVATE Qt6::Test Qt6::Core KRdp)
add_test(NAME KrdpctlV2Test COMMAND KrdpctlV2Test)
# KRDPCTL v2 end to end on 127.0.0.1: the production virtual broker transport and krdpctl-probe.
if(TARGET krdpctl-probe)
    add_executable(KrdpctlV2LoopbackTest KrdpctlV2LoopbackTest.cpp
        ${CMAKE_SOURCE_DIR}/server/VirtualSessionTransport.cpp ${CMAKE_SOURCE_DIR}/server/VirtualStockClient.cpp ${CMAKE_SOURCE_DIR}/server/VirtualSessionControl.cpp
        ${CMAKE_SOURCE_DIR}/server/VirtualSessionSupervisor.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp
        ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp ${CMAKE_SOURCE_DIR}/server/WorkerCodecBridge.cpp ${CMAKE_SOURCE_DIR}/server/VirtualResize.cpp
        ${CMAKE_SOURCE_DIR}/server/VirtualSessionGuardianClient.cpp)
    target_include_directories(KrdpctlV2LoopbackTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
    target_link_libraries(KrdpctlV2LoopbackTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network KRdp)
    target_compile_definitions(KrdpctlV2LoopbackTest PRIVATE KRDPCTL_PROBE="$<TARGET_FILE:krdpctl-probe>")
    add_dependencies(KrdpctlV2LoopbackTest krdpctl-probe)
    add_test(NAME KrdpctlV2LoopbackTest COMMAND KrdpctlV2LoopbackTest)
    set_tests_properties(KrdpctlV2LoopbackTest PROPERTIES TIMEOUT 180)
endif()
# --- end AUD-WS-L ---

# --- AUD-WS-D ---
# AUD-D1 server hygiene: RDPECAM device bookkeeping and the PipeWire endpoint
# toggle loop (private daemon; exits 77 when it can't run one).
add_executable(RemoteCameraSetTest RemoteCameraSetTest.cpp)
target_include_directories(RemoteCameraSetTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(RemoteCameraSetTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME RemoteCameraSetTest COMMAND RemoteCameraSetTest)

add_executable(CameraPrerequisitesTest CameraPrerequisitesTest.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleCameraSession.h ${CMAKE_SOURCE_DIR}/src/PipeWireCamera.cpp)
target_include_directories(CameraPrerequisitesTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
target_link_libraries(CameraPrerequisitesTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui PkgConfig::PipeWire)
add_test(NAME CameraPrerequisitesTest COMMAND CameraPrerequisitesTest)

add_executable(DeviceToggleLoopTest DeviceToggleLoopTest.cpp
    ${CMAKE_SOURCE_DIR}/src/PipeWireMicrophone.cpp
    ${CMAKE_SOURCE_DIR}/src/PipeWireCamera.cpp
    ${CMAKE_SOURCE_DIR}/src/PipeWireAudioPlayback.cpp)
target_include_directories(DeviceToggleLoopTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(DeviceToggleLoopTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui PkgConfig::PipeWire)
if(PIPEWIRE_EXECUTABLE)
    set(_krdp_device_loop_pipewire "${PIPEWIRE_EXECUTABLE}")
else()
    set(_krdp_device_loop_pipewire "pipewire") # not found: the test skips
endif()
target_compile_definitions(DeviceToggleLoopTest PRIVATE
    KRDP_AUDIO_CONFIG_DIR="${CMAKE_SOURCE_DIR}/server"
    KRDP_PIPEWIRE_EXECUTABLE="${_krdp_device_loop_pipewire}")
add_test(NAME DeviceToggleLoopTest COMMAND DeviceToggleLoopTest)
# ~20 s idle; 55-80 s seen under a load average of 25.
set_tests_properties(DeviceToggleLoopTest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

# AUD-D2/D3: the KRDPCTL `device` record, the `devices` capabilities group,
# DeviceConsent generations and StandardClientMedia (pure, socket-free).
add_executable(DeviceControlTest DeviceControlTest.cpp)
target_include_directories(DeviceControlTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
target_include_directories(DeviceControlTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(DeviceControlTest PRIVATE Qt6::Test Qt6::Core KRdp)
add_test(NAME DeviceControlTest COMMAND DeviceControlTest)
# Devices end to end on 127.0.0.1 with krdpctl-probe's standard audio channels:
# a stock client under StandardClientMedia, and `device` records from our own
# client. Private PipeWire daemon; exits 77 when it can't run one.
if(TARGET krdpctl-probe)
    add_executable(DeviceLoopbackTest DeviceLoopbackTest.cpp)
    target_include_directories(DeviceLoopbackTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
    target_include_directories(DeviceLoopbackTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
    target_link_libraries(DeviceLoopbackTest PRIVATE Qt6::Test Qt6::Core Qt6::Network KRdp)
    target_compile_definitions(DeviceLoopbackTest PRIVATE
        KRDPCTL_PROBE="$<TARGET_FILE:krdpctl-probe>"
        KRDP_AUDIO_CONFIG_DIR="${CMAKE_SOURCE_DIR}/server"
        KRDP_PIPEWIRE_EXECUTABLE="${_krdp_device_loop_pipewire}")
    add_dependencies(DeviceLoopbackTest krdpctl-probe)
    add_test(NAME DeviceLoopbackTest COMMAND DeviceLoopbackTest)
    set_tests_properties(DeviceLoopbackTest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
endif()
# --- end AUD-WS-D ---

# --- AUD-FIX ---
# F1: Wayland request version gating (fake-input destroy is since 5).
if(BUILD_PLASMA_SESSION)
    add_executable(WaylandRequestVersionTest WaylandRequestVersionTest.cpp)
    target_include_directories(WaylandRequestVersionTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
    target_link_libraries(WaylandRequestVersionTest PRIVATE Qt6::Test Qt6::Core Qt::WaylandClient)
    target_compile_definitions(WaylandRequestVersionTest PRIVATE
        PLASMA_PROTOCOLS_DIR="${PLASMA_WAYLAND_PROTOCOLS_DIR}"
        KRDP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    # The generated client-protocol headers come from KRdp's build.
    add_dependencies(WaylandRequestVersionTest KRdp)
    add_test(NAME WaylandRequestVersionTest COMMAND WaylandRequestVersionTest)
endif()
# F2: the configured systemd units restart after a crash, with a start limit.
add_executable(SystemdUnitPolicyTest SystemdUnitPolicyTest.cpp)
target_link_libraries(SystemdUnitPolicyTest PRIVATE Qt6::Test Qt6::Core)
target_compile_definitions(SystemdUnitPolicyTest PRIVATE KRDP_UNIT_DIR="${CMAKE_BINARY_DIR}/server")
add_test(NAME SystemdUnitPolicyTest COMMAND SystemdUnitPolicyTest)
# F3: the Wayland globals probe behind the Plasma-by-default backend choice.
if(BUILD_PLASMA_SESSION)
    add_executable(PlasmaBackendProbeTest PlasmaBackendProbeTest.cpp ${CMAKE_SOURCE_DIR}/server/PlasmaBackendProbe.cpp)
    target_include_directories(PlasmaBackendProbeTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
    target_link_libraries(PlasmaBackendProbeTest PRIVATE Qt6::Test Qt6::Gui Qt6::GuiPrivate Qt6::WaylandClient)
    add_test(NAME PlasmaBackendProbeTest COMMAND PlasmaBackendProbeTest)
    set_tests_properties(PlasmaBackendProbeTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
# F4: the console broker's refusal reaches a real libfreerdp client as 0x9.
if(TARGET krdpctl-probe)
    add_executable(ConsoleRefusalLoopbackTest ConsoleRefusalLoopbackTest.cpp
        ${CMAKE_SOURCE_DIR}/server/ConsoleHostController.cpp
        ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerEndpoint.cpp
        ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerSession.cpp ${CMAKE_SOURCE_DIR}/server/WorkerCodecBridge.cpp
        ${CMAKE_SOURCE_DIR}/server/ConsoleSeat.cpp)
    target_include_directories(ConsoleRefusalLoopbackTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
    target_include_directories(ConsoleRefusalLoopbackTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
    target_link_libraries(ConsoleRefusalLoopbackTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus KRdp)
    target_compile_definitions(ConsoleRefusalLoopbackTest PRIVATE KRDPCTL_PROBE="$<TARGET_FILE:krdpctl-probe>")
    add_dependencies(ConsoleRefusalLoopbackTest krdpctl-probe)
    add_test(NAME ConsoleRefusalLoopbackTest COMMAND ConsoleRefusalLoopbackTest)
    set_tests_properties(ConsoleRefusalLoopbackTest PROPERTIES TIMEOUT 120)
endif()
# F6: the console worker starts only once its session is Active and its Wayland socket accepts connections.
add_executable(ConsoleWorkerLaunchReadinessTest ConsoleWorkerLaunchReadinessTest.cpp
    ${CMAKE_SOURCE_DIR}/server/ConsoleWorkerLauncher.cpp ${CMAKE_SOURCE_DIR}/server/ConsoleSeat.cpp)
target_include_directories(ConsoleWorkerLaunchReadinessTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ConsoleWorkerLaunchReadinessTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::DBus Qt6::Network)
add_test(NAME ConsoleWorkerLaunchReadinessTest COMMAND ConsoleWorkerLaunchReadinessTest)
set_tests_properties(ConsoleWorkerLaunchReadinessTest PROPERTIES TIMEOUT 60)

# --- AUD-FIX2 ---
# F5: a software-encoded session's DmaBufHandler releases its EGL context, display, GBM device and fds.
pkg_check_modules(GBM IMPORTED_TARGET gbm)
if(TARGET PkgConfig::GBM AND TARGET K::KPipeWireDmaBuf)
    add_executable(DmaBufHandlerLeakTest DmaBufHandlerLeakTest.cpp)
    target_link_libraries(DmaBufHandlerLeakTest PRIVATE Qt6::Test Qt6::Gui K::KPipeWire K::KPipeWireDmaBuf PkgConfig::GBM)
    add_test(NAME DmaBufHandlerLeakTest COMMAND DmaBufHandlerLeakTest)
    set_tests_properties(DmaBufHandlerLeakTest PROPERTIES TIMEOUT 60)
endif()
# F1: SoftwareEncoding codec/backend policy (thresholds, hysteresis, CPU guard, switch interval).
add_executable(CodecPolicyTest CodecPolicyTest.cpp)
target_include_directories(CodecPolicyTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(CodecPolicyTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME CodecPolicyTest COMMAND CodecPolicyTest)
# OPT-055 K5: LogThrottle/LogEdge and the Info default of the farside.server category.
add_executable(LogThrottleTest LogThrottleTest.cpp ${CMAKE_BINARY_DIR}/src/krdp_logging.cpp)
target_include_directories(LogThrottleTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
target_link_libraries(LogThrottleTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME LogThrottleTest COMMAND LogThrottleTest)
# AUD-FIX13: a slow link or a slow client, by the socket's evidence.
add_executable(LinkEvidenceTest LinkEvidenceTest.cpp)
target_include_directories(LinkEvidenceTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(LinkEvidenceTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME LinkEvidenceTest COMMAND LinkEvidenceTest)
# AUD-FIX14: the a312776 (Wi-Fi) and 573fa31 gate traces replayed through LinkEvidence and the codec policy.
add_executable(SlowLinkReplayTest SlowLinkReplayTest.cpp)
target_include_directories(SlowLinkReplayTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(SlowLinkReplayTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME SlowLinkReplayTest COMMAND SlowLinkReplayTest)
# F1: the per-codec hardware/software encoder probe, with hardware forced off.
add_executable(EncoderSupportTest EncoderSupportTest.cpp)
target_link_libraries(EncoderSupportTest PRIVATE Qt6::Test KRdp PkgConfig::AVCodec)
add_test(NAME EncoderSupportTest COMMAND EncoderSupportTest)
set_tests_properties(EncoderSupportTest PROPERTIES TIMEOUT 60)
# F1/F5: software H.264 with VA-API unusable, and no per-session fd/thread leak (private PipeWire).
include(SoftwareEncodeSessionTest.cmake)
# F1: the codec id always matches the encoder KPipeWire actually settled on.
add_executable(EncoderSelectionTest EncoderSelectionTest.cpp)
target_include_directories(EncoderSelectionTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(EncoderSelectionTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(EncoderSelectionTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME EncoderSelectionTest COMMAND EncoderSelectionTest)
# F1: VideoStream answers `codec` from the host's encoders and drops a private codec the encoder lacks.
# AV1-Q: quality -> quantiser per codec and AV1 tile layouts of the linked KPipeWire.
add_executable(EncoderTuningTest EncoderTuningTest.cpp)
target_link_libraries(EncoderTuningTest PRIVATE Qt6::Test Qt6::Core K::KPipeWireRecord)
add_test(NAME EncoderTuningTest COMMAND EncoderTuningTest)
add_executable(VideoStreamCodecTest VideoStreamCodecTest.cpp)
target_include_directories(VideoStreamCodecTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(VideoStreamCodecTest PRIVATE Qt6::Test Qt6::Core KRdp)
add_test(NAME VideoStreamCodecTest COMMAND VideoStreamCodecTest)

# --- STATS ---
# STATS-S5: the KRDPCTL `stats` request, records and reporter (STATS-PANEL-DESIGN.md §5).
add_executable(StatsRequestTest StatsRequestTest.cpp)
target_include_directories(StatsRequestTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(StatsRequestTest PRIVATE Qt6::Test Qt6::Core KRdp)
add_test(NAME StatsRequestTest COMMAND StatsRequestTest)
# --- end STATS ---

add_executable(FarsideMigrationTest FarsideMigrationTest.cpp)
target_include_directories(FarsideMigrationTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(FarsideMigrationTest PRIVATE Qt6::Test KRdpLegacyMigration)
add_test(NAME FarsideMigrationTest COMMAND FarsideMigrationTest)

# Shared broker preferences and the bounded, per-uid configuration reader.
add_executable(DisplayWakePolicyTest DisplayWakePolicyTest.cpp)
target_include_directories(DisplayWakePolicyTest PRIVATE ${CMAKE_SOURCE_DIR}/server ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(DisplayWakePolicyTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME DisplayWakePolicyTest COMMAND DisplayWakePolicyTest)

add_executable(DisplayWakeGuardTest DisplayWakeGuardTest.cpp ../server/DisplayWakeGuard.cpp)
target_include_directories(DisplayWakeGuardTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(DisplayWakeGuardTest PRIVATE Qt6::Test Qt6::Core Qt6::DBus)
# Explicitly invoked inside a fresh private dbus-run-session on Sol; do not add
# this daemon integration check to the default ctest set or run it on Hal.

add_executable(BrokerUserSettingsTest BrokerUserSettingsTest.cpp)
add_executable(BrokerHostSettingsTest BrokerHostSettingsTest.cpp)
add_executable(BrokerHostAdminTest BrokerHostAdminTest.cpp)
target_link_libraries(BrokerHostAdminTest PRIVATE Qt6::Test KRdpBrokerHostAdmin)
add_test(NAME BrokerHostAdminTest COMMAND BrokerHostAdminTest)
add_executable(PrivateExecutionContextTest PrivateExecutionContextTest.cpp)
target_link_libraries(PrivateExecutionContextTest PRIVATE Qt6::Test)
target_include_directories(PrivateExecutionContextTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_compile_definitions(PrivateExecutionContextTest PRIVATE SERVER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/server")
add_test(NAME PrivateExecutionContextTest COMMAND PrivateExecutionContextTest)
add_executable(BrokerHostPublicSnapshotTest BrokerHostPublicSnapshotTest.cpp)
target_link_libraries(BrokerHostPublicSnapshotTest PRIVATE Qt6::Test KRdpBrokerHostPublicSnapshot)
add_test(NAME BrokerHostPublicSnapshotTest COMMAND BrokerHostPublicSnapshotTest)
add_executable(BrokerHostRuntimeTest BrokerHostRuntimeTest.cpp)
target_link_libraries(BrokerHostRuntimeTest PRIVATE Qt6::Test KRdpBrokerHostRuntime)
add_test(NAME BrokerHostRuntimeTest COMMAND BrokerHostRuntimeTest)
add_executable(BrokerHostRuntimeNativeTest BrokerHostRuntimeNativeTest.cpp)
target_link_libraries(BrokerHostRuntimeNativeTest PRIVATE Qt6::Test KRdpBrokerHostRuntimeReader)
add_executable(BrokerHostSettingsModelTest BrokerHostSettingsModelTest.cpp)
target_link_libraries(BrokerHostSettingsModelTest PRIVATE Qt6::Test kcm_krdpserver_core)
target_compile_definitions(BrokerHostSettingsModelTest PRIVATE HOST_PROTOCOL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/host-settings-protocol.py")
add_test(NAME BrokerHostSettingsModelTest COMMAND BrokerHostSettingsModelTest)
add_executable(BrokerHostsPageTest BrokerHostsPageTest.cpp)
add_executable(BrokerMainPageTest BrokerMainPageTest.cpp)
target_link_libraries(BrokerMainPageTest PRIVATE Qt6::Test Qt6::Quick Qt6::Qml Qt6::Widgets KF6::I18nQml kcm_krdpserver_core)
target_compile_definitions(BrokerMainPageTest PRIVATE MAIN_PROTOCOL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/host-settings-protocol.py" MAIN_PAGE="${CMAKE_SOURCE_DIR}/src/kcm/ui/BrokerMainPage.qml" MAIN_AUTH_PROTOCOL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/authentication-admin-protocol.py")
add_test(NAME BrokerMainPageTest COMMAND BrokerMainPageTest)
set_tests_properties(BrokerMainPageTest PROPERTIES TIMEOUT 60 LABELS "gui;non-hal"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=org.kde.desktop;DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent-farside-test-bus;LANGUAGE=en_US;LC_ALL=C.UTF-8")
target_link_libraries(BrokerHostsPageTest PRIVATE Qt6::Test Qt6::Quick Qt6::Qml Qt6::Widgets KF6::I18nQml kcm_krdpserver_core)
target_compile_definitions(BrokerHostsPageTest PRIVATE HOST_PROTOCOL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/host-settings-protocol.py" HOST_PAGE="${CMAKE_SOURCE_DIR}/src/kcm/ui/BrokerHostsPage.qml")
add_test(NAME BrokerHostsPageTest COMMAND BrokerHostsPageTest)
set_tests_properties(BrokerHostsPageTest PROPERTIES TIMEOUT 60 LABELS "gui;non-hal"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_CONTROLS_STYLE=org.kde.desktop;DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent")
target_compile_definitions(BrokerHostSettingsTest PRIVATE HOST_SETTINGS_SOURCE="${CMAKE_SOURCE_DIR}")
target_link_libraries(BrokerHostSettingsTest PRIVATE Qt6::Test KRdpBrokerHostSettings)
add_test(NAME BrokerHostSettingsTest COMMAND BrokerHostSettingsTest)
add_executable(BrokerHostSettingsNativeProbe BrokerHostSettingsNativeProbe.cpp)
target_link_libraries(BrokerHostSettingsNativeProbe PRIVATE KRdpBrokerHostSettings)
# Actual systemd transient fixture units run manually on Sol, never default CTest.
target_link_libraries(BrokerUserSettingsTest PRIVATE Qt6::Test KRdpBrokerUserSettings)
add_test(NAME BrokerUserSettingsTest COMMAND BrokerUserSettingsTest)
add_executable(LegacySettingsMigrationTest LegacySettingsMigrationTest.cpp)
target_include_directories(LegacySettingsMigrationTest BEFORE PRIVATE ${CMAKE_BINARY_DIR}/src/kcm)
target_link_libraries(LegacySettingsMigrationTest PRIVATE Qt6::Test KRdpSettingsMigration kcm_krdpserver_core)
add_test(NAME LegacySettingsMigrationTest COMMAND LegacySettingsMigrationTest)
set_tests_properties(LegacySettingsMigrationTest PROPERTIES TIMEOUT 30 LABELS "pure")
add_executable(BrokerPreferencesTest BrokerPreferencesTest.cpp)
target_link_libraries(BrokerPreferencesTest PRIVATE Qt6::Test kcm_krdpserver_core)
add_test(NAME BrokerPreferencesTest COMMAND BrokerPreferencesTest)
add_executable(BrokerPreferencesNativeTest BrokerPreferencesNativeTest.cpp)
target_link_libraries(BrokerPreferencesNativeTest PRIVATE Qt6::Test kcm_krdpserver_core)
# Manual Sol private-home namespace gate; never register against real HOME.
add_executable(BrokerPreferencesPageTest BrokerPreferencesPageTest.cpp)
target_link_libraries(BrokerPreferencesPageTest PRIVATE Qt6::Test Qt6::Quick Qt6::Qml Qt6::Widgets KF6::I18nQml kcm_krdpserver_core)
target_compile_definitions(BrokerPreferencesPageTest PRIVATE PREFERENCES_PAGE="${CMAKE_SOURCE_DIR}/src/kcm/ui/BrokerPreferencesPage.qml")
add_test(NAME BrokerPreferencesPageTest COMMAND BrokerPreferencesPageTest)
set_tests_properties(BrokerPreferencesPageTest PROPERTIES TIMEOUT 45 LABELS "gui;non-hal"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=org.kde.desktop")
add_executable(BrokerAuthenticationTest BrokerAuthenticationTest.cpp)
target_link_libraries(BrokerAuthenticationTest PRIVATE Qt6::Test KRdpBrokerAuthentication)
add_test(NAME BrokerAuthenticationTest COMMAND BrokerAuthenticationTest)
add_executable(BrokerAuthenticationAdminTest BrokerAuthenticationAdminTest.cpp)
target_link_libraries(BrokerAuthenticationAdminTest PRIVATE Qt6::Test KRdpBrokerAuthentication)
add_test(NAME BrokerAuthenticationAdminTest COMMAND BrokerAuthenticationAdminTest)
add_executable(BrokerAuthenticationSettingsTest BrokerAuthenticationSettingsTest.cpp)
target_link_libraries(BrokerAuthenticationSettingsTest PRIVATE Qt6::Test kcm_krdpserver_core)
target_compile_definitions(BrokerAuthenticationSettingsTest PRIVATE AUTH_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/authentication-admin-protocol.py")
add_test(NAME BrokerAuthenticationSettingsTest COMMAND BrokerAuthenticationSettingsTest)
add_executable(BrokerServicesTest BrokerServicesTest.cpp)
target_link_libraries(BrokerServicesTest PRIVATE Qt6::Test kcm_krdpserver_core)
add_test(NAME BrokerServicesTest COMMAND BrokerServicesTest)
add_executable(BrokerServicesBusTest BrokerServicesBusTest.cpp)
target_link_libraries(BrokerServicesBusTest PRIVATE Qt6::Test kcm_krdpserver_core)
# Private D-Bus and actual system read-only checks run manually on Sol. Never
# add this daemon-dependent executable to Hal's pure test selection.
add_executable(BrokerSignInPageTest BrokerSignInPageTest.cpp)
target_link_libraries(BrokerSignInPageTest PRIVATE Qt6::Test Qt6::Quick Qt6::Qml Qt6::Widgets KF6::I18nQml kcm_krdpserver_core)
target_compile_definitions(BrokerSignInPageTest PRIVATE AUTH_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/authentication-admin-protocol.py" AUTH_PAGE="${CMAKE_SOURCE_DIR}/src/kcm/ui/BrokerSignInPage.qml")
add_test(NAME BrokerSignInPageTest COMMAND BrokerSignInPageTest)
set_tests_properties(BrokerSignInPageTest PROPERTIES TIMEOUT 45 LABELS "gui;non-hal"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=org.kde.desktop")
add_executable(BrokerAuthenticationVirtualProbe BrokerAuthenticationVirtualProbe.cpp
    ../server/VirtualSessionHostController.cpp ../server/VirtualSessionTransport.cpp ../server/VirtualStockClient.cpp
    ../server/VirtualSessionControl.cpp ../server/VirtualSessionSupervisor.cpp ../server/ConsoleWorkerEndpoint.cpp
    ../server/ConsoleWorkerSession.cpp ../server/WorkerCodecBridge.cpp ../server/VirtualResize.cpp)
target_link_libraries(BrokerAuthenticationVirtualProbe PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus
    KRdp KRdpGuardianClient KRdpBrokerUserSettings KRdpBrokerAuthentication)
# Explicit Sol-only bounded real-RDP gate, never a default CTest/Hal daemon test.
foreach(target VirtualSessionTransportTest VirtualStockClientTest VirtualSessionHostControllerTest
        ConsoleHostControllerTest ConsoleHostLifecycleTest ConsoleRefusalLoopbackTest KrdpctlV2LoopbackTest)
    if(TARGET ${target})
        target_link_libraries(${target} PRIVATE KRdpBrokerUserSettings)
    endif()
endforeach()

# OPT-054: pure RDP input/wire/held-key/physical-activity gate; no host devices or daemons.
add_executable(GameInputTest GameInputTest.cpp)
target_include_directories(GameInputTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(GameInputTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui KRdp)
add_test(NAME GameInputTest COMMAND GameInputTest)

# OPT-055 K3 / T-R5: accept-error (EMFILE) recovery and bounded Server teardown.
add_executable(ServerAcceptRecoveryTest ServerAcceptRecoveryTest.cpp)
target_include_directories(ServerAcceptRecoveryTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(ServerAcceptRecoveryTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(ServerAcceptRecoveryTest PRIVATE Qt6::Test Qt6::Core Qt6::Network KRdp)
add_test(NAME ServerAcceptRecoveryTest COMMAND ServerAcceptRecoveryTest)
set_tests_properties(ServerAcceptRecoveryTest PROPERTIES TIMEOUT 60)
# N1: the same cases with Qt's own UNIX event dispatcher instead of glib.
add_test(NAME ServerAcceptRecoveryTestNoGlib COMMAND ServerAcceptRecoveryTest)
set_tests_properties(ServerAcceptRecoveryTestNoGlib PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_NO_GLIB=1")

# --- OPT-055 K6: krdp safety net ---
set(_krdp_k6_src ${CMAKE_SOURCE_DIR}/src)
add_executable(SystemdNotifyTest SystemdNotifyTest.cpp ${_krdp_k6_src}/SystemdNotify.cpp)
target_include_directories(SystemdNotifyTest PRIVATE ${_krdp_k6_src} ${CMAKE_BINARY_DIR}/src)
target_link_libraries(SystemdNotifyTest PRIVATE Qt6::Test Qt6::Core)
if(Systemd_FOUND)
    target_link_libraries(SystemdNotifyTest PRIVATE Systemd::systemd)
    target_compile_definitions(SystemdNotifyTest PRIVATE KRDP_HAVE_SYSTEMD)
endif()
add_test(NAME SystemdNotifyTest COMMAND SystemdNotifyTest)
set_tests_properties(SystemdNotifyTest PROPERTIES TIMEOUT 60)

add_executable(StallDetectorTest StallDetectorTest.cpp ${_krdp_k6_src}/StallDetector.cpp)
target_include_directories(StallDetectorTest PRIVATE ${_krdp_k6_src} ${CMAKE_BINARY_DIR}/src)
target_link_libraries(StallDetectorTest PRIVATE Qt6::Test Qt6::Core)
target_link_options(StallDetectorTest PRIVATE -rdynamic) # backtrace_symbols_fd names the test function
add_test(NAME StallDetectorTest COMMAND StallDetectorTest)
set_tests_properties(StallDetectorTest PROPERTIES TIMEOUT 60)

# The tests that throw, and the helpers that catch, are built with exceptions; the tree is not.
add_executable(TerminateHandlerTest TerminateHandlerTest.cpp ${_krdp_k6_src}/TerminateHandler.cpp)
target_include_directories(TerminateHandlerTest PRIVATE ${_krdp_k6_src} ${CMAKE_BINARY_DIR}/src)
target_link_libraries(TerminateHandlerTest PRIVATE Qt6::Test Qt6::Core)
target_compile_options(TerminateHandlerTest PRIVATE -fexceptions)
set_source_files_properties(${_krdp_k6_src}/TerminateHandler.cpp PROPERTIES COMPILE_OPTIONS -fexceptions)
add_test(NAME TerminateHandlerTest COMMAND TerminateHandlerTest)

add_executable(SafeThreadTest SafeThreadTest.cpp ${_krdp_k6_src}/SafeThread.cpp)
target_include_directories(SafeThreadTest PRIVATE ${_krdp_k6_src} ${CMAKE_BINARY_DIR}/src)
target_link_libraries(SafeThreadTest PRIVATE Qt6::Test Qt6::Core)
set_source_files_properties(${_krdp_k6_src}/SafeThread.cpp PROPERTIES COMPILE_OPTIONS -fexceptions)
add_test(NAME SafeThreadTest COMMAND SafeThreadTest)
# T-K6e helper (run only in a transient unit on Sol): the daemon wiring plus an optional forced stall.
add_executable(SystemdWatchdogProbe SystemdWatchdogProbe.cpp ${_krdp_k6_src}/SystemdNotify.cpp
    ${_krdp_k6_src}/StallDetector.cpp ${_krdp_k6_src}/TerminateHandler.cpp)
target_include_directories(SystemdWatchdogProbe PRIVATE ${_krdp_k6_src} ${CMAKE_BINARY_DIR}/src)
target_link_libraries(SystemdWatchdogProbe PRIVATE Qt6::Core)
if(Systemd_FOUND)
    target_link_libraries(SystemdWatchdogProbe PRIVATE Systemd::systemd)
    target_compile_definitions(SystemdWatchdogProbe PRIVATE KRDP_HAVE_SYSTEMD)
endif()
# --- end OPT-055 K6 ---

# OPT-055 wave 1: a missing Plasma Wayland interface grant closes the session with a reason, never the process.
add_executable(PlasmaScreencastUnavailableTest PlasmaScreencastUnavailableTest.cpp)
target_include_directories(PlasmaScreencastUnavailableTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
target_link_libraries(PlasmaScreencastUnavailableTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui KRdp)
add_test(NAME PlasmaScreencastUnavailableTest COMMAND PlasmaScreencastUnavailableTest -platform offscreen)
set_tests_properties(PlasmaScreencastUnavailableTest PROPERTIES TIMEOUT 60)
