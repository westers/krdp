# CTest labels, applied after every test is registered (included at the end of CMakeLists.txt).
#
#   unit      no daemon, no bus, no GPU/device node, no listening TCP socket: safe on a work desktop
#             (`ctest -L unit`). Fixtures may spawn our own short-lived helpers or `sleep`, use private
#             sockets under TMPDIR and fake tools on PATH. This list is explicit on purpose: a new test
#             is NOT unit until someone has checked what it starts and adds it here.
#   loopback  listens on 127.0.0.1 or runs krdpctl-probe / openssl against a test server, or binds
#             sockets in the real XDG_RUNTIME_DIR.
#   daemon    starts or talks to a daemon: private PipeWire/WirePlumber/dbus-daemon/KWin, the session or
#             system bus, bwrap sandboxes, shell fixtures run through dbus-run-session. Not on Hal.
#   gpu       opens /dev/dri or /dev/nvidia* (encoder probes, DMA-BUF, VAAPI/NVENC sessions).
#   root      would need root. No registered test does (those with a root-only path skip as root).
# `gui` / `non-hal` / `pure` are older labels set next to their tests and are kept.
# Evidence for the lists: docs/testing.md ("How the labels were derived").
set(KRDP_TEST_LABEL_unit
    appstreamtest ClientQualityTest AdaptiveQualityTest PeerContextLifecycleTest VirtualSessionStateTest
    VirtualSessionRegistryTest VirtualSessionJournalTest VirtualSessionKeeperProcessTest
    VirtualSessionLaunchPlanTest VirtualSessionAccountEnvironmentTest VirtualSystemBusTest
    VirtualSessionServiceScopeTest VirtualSessionServiceOwnerTest VirtualSessionOwnerWatchTest
    VirtualSessionPamTest VirtualSessionServicePlanTest VirtualDeviceEntryTest VirtualSessionStorageTest
    VirtualSessionGuardianTest VirtualSessionGuardianClientTest VirtualSessionSupervisorTest
    VirtualSessionControlTest VirtualSessionTransportTest VirtualStockClientTest CaptureWorkerModeTest
    SurfaceLayoutTest MultiLayoutTest FrameQueuePolicyTest EncoderFailurePolicyTest StreamResumePolicyTest
    EncoderWatchdogTest ClientDisplayInfoTest OutputSnapshotTest RemoteTopologyCatalogTest
    RemoteTopologyDraftTest VirtualInitialBootstrapTest ConsoleTopologyPlanTest ConsoleTopologyReadbackTest
    RemoteTopologyFitTest VirtualInitialLayoutTest RetainedKScreenReadbackTest RemoteTopologyProtocolTest
    RetainedMultiCaptureTest RetainedMultiInputTest TakeoverDetectorTest ConsoleSeatTest
    ConsoleSeatWatcherTest ConsoleControlTest ConsoleResizeTest ConsoleResizeExecutorTest
    ConsoleResizeSessionTest SurfaceChainTest H264KeyframeSizeTest VirtualResizeTest VirtualResizeSessionTest
    ConsoleInputStateTest ConsoleHandoffTest ConsoleWorkerWireTest ConsoleClipboardPolicyTest
    HostClipboardTest SelectionReaderTest PngMimeDataTest ConsoleVirtualOutputPlanTest
    ConsoleReplaceSnapshotGateTest ConsoleReleaseLockGuardTest ConsoleWorkerEndpointTest ClientFileRelayTest
    HostFileRelayTest ConsoleWorkerWatchdogTest CursorShapeTest ConsoleWorkerOrderTest
    ConsoleWorkerSessionTest ConsoleHostControllerTest RdpAudioPriorityTest PipeWireAudioRoutingTest
    ClipboardTextTest ClipboardImageTest HtmlClipboardTest ClipboardMimeTest LayoutControlTest LayoutOwnerTest
    LayoutSessionDiffTest VideoCodecSupportTest AvcCodecSelectionTest GfxSurfaceCommandTest
    PreAuthChannelGateTest ListenAddressTest ScreencastTargetTest PressedInputTrackerTest ClipboardBridgeTest
    ClipboardFilesBridgeTest ClipboardClientFilesBridgeTest ClipboardHandoffTest KcfgSingleSourceTest
    ServerCertificateTest ServerSettingsPolicyTest KcmSettingsTest KcmCoexistenceTest
    ConsoleWorkerEndpointHardeningTest OutputRestoreJournalTest ConsoleOutputJournalTest
    ConsoleHostLifecycleTest KrdpctlV2Test RemoteCameraSetTest CameraPrerequisitesTest DeviceControlTest
    WaylandRequestVersionTest SystemdUnitPolicyTest PlasmaBackendProbeTest ConsoleWorkerLaunchReadinessTest
    CodecPolicyTest CodecSelectionTest LogThrottleTest ConsoleReclaimEpisodeTest LinkEvidenceTest
    SlowLinkReplayTest EncoderSelectionTest EncoderTuningTest CodecRequestTest VideoCodecHostTest
    VideoStreamCodecTest StatsRequestTest ClipboardFileNamesTest ClipboardFileDescriptorTest
    ClipboardFileRequestTest ClipboardFileLimitsTest ClipboardFilePacingTest ClipboardFileStagingTest
    ClipboardFileServeTest ClipboardTransferTest ClipboardFilePushTest ClipboardFileFetchTest HostFilesTest
    HostFileStagingTest HostFileStagerTest ClipboardFileQosTest DeliveryStatsTest FarsideMigrationTest
    DisplayWakePolicyTest BrokerHostAdminTest PrivateExecutionContextTest BrokerHostPublicSnapshotTest
    BrokerHostRuntimeTest BrokerHostSettingsModelTest BrokerSettingsApplyTest BrokerHostSettingsTest
    BrokerUserSettingsTest LegacySettingsMigrationTest BrokerPreferencesTest BrokerAuthenticationAdminTest
    BrokerAuthenticationSettingsTest BrokerServicesTest GameInputTest SystemdNotifyTest StallDetectorTest
    TerminateHandlerTest SafeThreadTest PlasmaScreencastUnavailableTest InputMappingTest LockKeySyncTest
    WheelScaleTest DeskActivityTest)
set(KRDP_TEST_LABEL_loopback
    BrokerAuthenticationTest ClipboardFileLargeLoopbackTest ClipboardFileLoopbackTest
    ClipboardFilePushLoopbackTest ClipboardStallLoopbackTest ConsoleRefusalLoopbackTest CursorLoopbackTest
    DeviceLoopbackTest KrdpctlV2LoopbackTest RdpConnectionInitTest RdpPreAuthLoopbackTest
    ServerAcceptRecoveryTest ServerAcceptRecoveryTestNoGlib VideoFlowLoopbackTest
    VirtualSessionHostControllerTest WorkerEndToEndTest)
set(KRDP_TEST_LABEL_daemon
    BrokerPreferencesPageTest BrokerSignInPageTest DeviceLoopbackTest DeviceToggleLoopTest KcmUiTest
    PipeWireMicrophoneInUseTest PipeWireMicrophonePcmTest PipeWireMicrophoneTest RenderAccessTest
    SoftwareEncodeSessionTest VirtualSessionAudioGraphTest VirtualSessionAudioPcmTest
    VirtualSessionAudioPulseTest VirtualSessionBusTest VirtualSessionLockConfigTest
    VirtualSessionLoginRecoveryTest VirtualSessionLoginTest VirtualSystemBusLauncherTest WorkerEndToEndTest)
set(KRDP_TEST_LABEL_gpu
    DeviceToggleLoopTest DmaBufHandlerLeakTest EncoderSupportTest RenderAccessTest SoftwareEncodeSessionTest
    VirtualSessionHostControllerTest WorkerEndToEndTest)
set(KRDP_TEST_LABEL_root)
foreach(_krdp_label unit loopback daemon gpu root)
    foreach(_krdp_test IN LISTS KRDP_TEST_LABEL_${_krdp_label})
        if(TEST ${_krdp_test})
            set_property(TEST ${_krdp_test} APPEND PROPERTY LABELS ${_krdp_label})
        endif()
    endforeach()
endforeach()
