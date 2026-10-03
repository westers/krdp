// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// This file is roughly based on grd-session-rdp.c from gnome-remote-desktop,
// which is:
//
// SPDX-FileCopyrightText: 2020-2023 Pascal Nowack
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "RdpConnection.h"
#include "AdaptiveQuality.h"
#include "DeviceConsent.h"
#include "MicrophonePcmQueue.h"
#include "CameraAvailability.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QPointer>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <cstddef>

#include <linux/sockios.h>

#include <freerdp/channels/wtsvc.h>
#include <freerdp/freerdp.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/audin.h>
#include <freerdp/server/disp.h>
#include <freerdp/server/rdpsnd.h>
#include "ExternalAudioQueue.h"
#include <freerdp/server/server-common.h>
#include <freerdp/server/rdpecam-enumerator.h>
#include <freerdp/server/rdpecam.h>

#include <freerdp/channels/drdynvc.h>
#include <freerdp/codec/audio.h>
#include <winpr/sysinfo.h>

#include "Clipboard.h"
#include "Cursor.h"
#include "InputHandler.h"
#include "LayoutControl.h"
#include "NetworkDetection.h"
#include "PeerContext_p.h"
#include "PipeWireMicrophone.h"
#include "PipeWireCamera.h"
#include "PipeWireAudioPlayback.h"
#include "RemoteCameraSet.h"
#include "PreAuthChannelGate.h"
#include "Server.h"
#include "VideoStream.h"

#include <KUser>
#include <QScopeGuard>

#include "RenderNodes.h"
#include "krdp_logging.h"

namespace fs = std::filesystem;

namespace KRdp
{

namespace
{
struct RenderNodeInfo {
    QByteArray renderNode;
    QByteArray vendorId;
};

QByteArray readTrimmedFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return file.readAll().trimmed();
}

QByteArray preferredIntelDriver()
{
    constexpr auto iHdDriverPath = "/usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so";
    constexpr auto i965DriverPath = "/usr/lib/x86_64-linux-gnu/dri/i965_drv_video.so";
    if (QFile::exists(QLatin1StringView(iHdDriverPath))) {
        return "iHD";
    }
    if (QFile::exists(QLatin1StringView(i965DriverPath))) {
        return "i965";
    }
    return "iHD";
}

std::vector<RenderNodeInfo> renderNodes()
{
    std::vector<RenderNodeInfo> nodes;
    // AUD-FIX8 B3: stat()-based (a bind-mounted node in a sandbox is DT_REG to readdir()).
    const auto entries = RenderNodes::list();
    nodes.reserve(entries.size());

    for (const auto &path : entries) {
        if (::access(QFile::encodeName(path).constData(), R_OK) != 0) {
            continue;
        }
        const QString entry = path.section(QLatin1Char('/'), -1);
        const auto vendorPath = QStringLiteral("/sys/class/drm/%1/device/vendor").arg(entry);
        const auto vendorId = readTrimmedFile(vendorPath);
        if (vendorId.isEmpty()) {
            continue;
        }

        nodes.push_back(RenderNodeInfo{
            .renderNode = QFile::encodeName(path),
            .vendorId = vendorId,
        });
    }

    return nodes;
}

QByteArray preferredMixedGpuDriver(const std::vector<RenderNodeInfo> &nodes)
{
    bool hasNvidia = false;
    bool hasAmd = false;
    bool hasIntel = false;

    for (const auto &node : nodes) {
        hasNvidia = hasNvidia || (node.vendorId == "0x10de");
        hasAmd = hasAmd || (node.vendorId == "0x1002" || node.vendorId == "0x1022");
        hasIntel = hasIntel || (node.vendorId == "0x8086");
    }

    if (!hasNvidia) {
        return {};
    }
    if (hasAmd) {
        return "radeonsi";
    }
    if (hasIntel) {
        return preferredIntelDriver();
    }
    return {};
}

QString vendorSummary(const std::vector<RenderNodeInfo> &nodes)
{
    QStringList values;
    values.reserve(nodes.size());
    for (const auto &node : nodes) {
        values.push_back(QStringLiteral("%1=%2").arg(QString::fromLatin1(node.renderNode), QString::fromLatin1(node.vendorId)));
    }
    return values.join(QStringLiteral(", "));
}

// Tracks whether the value currently in LIBVA_DRIVER_NAME was set by us, so a
// later config change can replace our own choice while an externally-provided
// value is always left untouched.
bool g_autoAppliedVaapiDriver = false;

// The layout control static virtual channel (slice 2c, OPT-044). Seven
// characters: CHANNEL_NAME_LEN is the limit for a static channel name.
char ControlChannelName[] = "KRDPCTL";
const AUDIO_FORMAT RemoteMicrophoneFormat{
    WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0, nullptr,
};

// How long a device may take to become ready once its channel was opened
// (DEVICES-DESIGN.md §3). FreeRDP's AUDIN thread would otherwise poll a
// channel the client never accepts for as long as the connection lasts.
constexpr auto DeviceReadyDeadline = std::chrono::seconds(5);
// Microphone/camera: how often the session loop checks whether a host
// application is capturing (the `inUse` push).
constexpr auto InUsePollInterval = std::chrono::milliseconds(500);

struct AudinDelivery {
    KRdp::DeviceConsent *consent = nullptr;
    // Set by the session thread once the client accepted the channel (the
    // PipeWire source only exists from then on); read by the AUDIN thread.
    std::atomic<PipeWireMicrophone *> endpoint = nullptr;
    uint64_t generation = 0;
    KRdp::MicrophonePcmQueue *external = nullptr;
    // The client's Open Reply (AUDIN thread): -1 none yet, else its Result
    // (0: the client opened its capture device; anything else: it refused).
    std::atomic<int64_t> openResult = -1;

    /** Session thread, while no AUDIN thread is running. */
    void reset(KRdp::DeviceConsent *newConsent = nullptr, uint64_t newGeneration = 0, KRdp::MicrophonePcmQueue *newExternal = nullptr)
    {
        consent = newConsent;
        endpoint = nullptr;
        generation = newGeneration;
        external = newExternal;
        openResult = -1;
    }
};

UINT audinData(audin_server_context *audin, const SNDIN_DATA *data)
{
    if (!data || !data->Data) {
        return ERROR_INVALID_DATA;
    }
    auto *delivery = static_cast<AudinDelivery *>(audin->userdata);
    const size_t bytes = Stream_Length(data->Data);
    if (bytes > 192000 || bytes % 4 != 0) return ERROR_INVALID_DATA;
    PipeWireMicrophone *endpoint = delivery ? delivery->endpoint.load() : nullptr;
    if (delivery && delivery->consent && (endpoint || delivery->external)) {
        delivery->consent->deliver(delivery->generation, [&] {
            const QByteArray pcm(reinterpret_cast<const char *>(Stream_Buffer(data->Data)), int(bytes));
            if (delivery->external) delivery->external->write(delivery->generation, pcm);
            else endpoint->write(pcm);
        });
    }
    return CHANNEL_RC_OK;
}

UINT audinOpenReply(audin_server_context *audin, const SNDIN_OPEN_REPLY *reply)
{
    // MS-RDPEAI 2.2.2.4: Result is an HRESULT, zero when the client opened its
    // capture device. The session loop turns it into `on` or `declined`.
    if (auto *delivery = static_cast<AudinDelivery *>(audin->userdata); delivery && reply) {
        delivery->openResult.store(int64_t(reply->Result));
    }
    qCInfo(KRDP) << "AUDIN Open Reply, result" << (reply ? reply->Result : 0);
    return CHANNEL_RC_OK;
}

// RDPSND bookkeeping shared with FreeRDP's RDPSND thread (Activated).
struct RdpsndState {
    std::atomic_bool active = false;
    // The client format Activated selected; re-selected after SNDC_CLOSE,
    // which makes FreeRDP forget it.
    std::atomic<int> clientFormat = -1;
};

void rdpsndActivated(RdpsndServerContext *rdpsnd)
{
    for (size_t client = 0; client < rdpsnd->num_client_formats; ++client) {
        for (size_t server = 0; server < rdpsnd->num_server_formats; ++server) {
            if (!audio_format_compatible(&rdpsnd->server_formats[server], &rdpsnd->client_formats[client])) {
                continue;
            }
            if (rdpsnd->SelectFormat(rdpsnd, UINT16(client)) == CHANNEL_RC_OK) {
                if (auto *state = static_cast<RdpsndState *>(rdpsnd->data)) {
                    state->clientFormat.store(int(client));
                    state->active.store(true);
                }
                const auto &format = rdpsnd->client_formats[client];
                // The index alone cannot distinguish PCM from a compressed
                // format. These are negotiated descriptors, not measured
                // wire throughput (especially for variable-rate codecs).
                qCInfo(KRDP) << "RDPSND selected client format" << client
                            << "tag" << format.wFormatTag
                            << "rate" << format.nSamplesPerSec
                            << "channels" << format.nChannels
                            << "bits" << format.wBitsPerSample
                            << "declaredBytesPerSecond" << format.nAvgBytesPerSec;
                return;
            }
        }
    }
    qCWarning(KRDP) << "RDPSND client offered no compatible format";
}

struct RemoteCamera {
    CameraDeviceServerContext *context = nullptr;
    bool activated = false;
    bool receivedSample = false;
    CAM_MEDIA_TYPE_DESCRIPTION format{};
    QString loopbackDevice;
    std::atomic<bool> streamStarted = false;
    std::atomic<bool> startPending = false;
    std::atomic<bool> stopPending = false;
    std::unique_ptr<PipeWireCamera> endpoint;
    RdpConnection *connection = nullptr;
    bool external = false;
    std::atomic<bool> *workerCapture = nullptr;
    std::atomic<bool> *framePending = nullptr;
    std::atomic<quint64> *currentEpoch = nullptr;
    quint64 epoch = 0;
    // Set (device thread) once endpoint exists; the session thread only
    // touches endpoint after seeing it.
    std::atomic<bool> endpointStarted = false;
    ~RemoteCamera()
    {
        if (context) {
            context->Close(context);
            camera_device_server_context_free(context);
        }
    }
};

UINT cameraSuccess(CameraDeviceServerContext *context, const CAM_SUCCESS_RESPONSE *)
{
    auto *camera = static_cast<RemoteCamera *>(context->userdata);
    if (camera && !camera->activated) {
        qCInfo(KRDP) << "RDPECAM success response device activated";
        camera->activated = true;
        CAM_STREAM_LIST_REQUEST request{};
        return context->StreamListRequest(context, &request);
    }
    if (camera && camera->startPending.exchange(false)) {
        qCInfo(KRDP) << "RDPECAM success response stream started";
        // Demand may have ended while the asynchronous start reply was in
        // flight. The session loop will send StopStreams before another pull.
        if (camera->external && !camera->workerCapture->load()) return CHANNEL_RC_OK;
    } else if (camera && camera->stopPending.exchange(false)) {
        qCInfo(KRDP) << "RDPECAM stream stopped after worker demand ended";
        return CHANNEL_RC_OK;
    } else {
        qCWarning(KRDP) << "RDPECAM unexpected success response";
        return CHANNEL_RC_OK;
    }
    // RDPECAM is pull based: a started stream does not produce a frame until
    // the server asks for one, and each subsequent frame needs another pull.
    CAM_SAMPLE_REQUEST request{};
    request.StreamIndex = 0;
    const UINT status = context->SampleRequest(context, &request);
    qCInfo(KRDP) << "RDPECAM initial sample request status" << status;
    return status;
}

BOOL cameraChannelAssigned(CameraDeviceServerContext *context, UINT32 channelId)
{
    qCInfo(KRDP) << "RDPECAM device channel ready" << channelId;
    CAM_ACTIVATE_DEVICE_REQUEST activate{};
    return context->ActivateDeviceRequest(context, &activate) == CHANNEL_RC_OK;
}

UINT cameraStreamList(CameraDeviceServerContext *context, const CAM_STREAM_LIST_RESPONSE *response)
{
    if (response->N_Descriptions == 0) {
        return ERROR_NOT_FOUND;
    }
    CAM_MEDIA_TYPE_LIST_REQUEST request{};
    request.StreamIndex = 0;
    return context->MediaTypeListRequest(context, &request);
}

UINT cameraMediaTypes(CameraDeviceServerContext *context, const CAM_MEDIA_TYPE_LIST_RESPONSE *response)
{
    auto *camera = static_cast<RemoteCamera *>(context->userdata);
    if (response->N_Descriptions == 0) {
        return ERROR_NOT_FOUND;
    }
    if (camera->endpointStarted.load()) {
        return CHANNEL_RC_OK; // one media type list per device; never replace a published endpoint
    }
    // The bundled V4L backend currently advertises H.264 for this webcam yet
    // emits MJPEG frames (see its cam_v4l_stream_start log). Keep the advertised
    // type for the protocol, then identify/decode the actual JPEG samples.
    const CAM_MEDIA_TYPE_DESCRIPTION *selected = &response->MediaTypeDescriptions[0];
    for (size_t i = 0; i < response->N_Descriptions; ++i) {
        if (response->MediaTypeDescriptions[i].Format == CAM_MEDIA_FORMAT_MJPG) {
            selected = &response->MediaTypeDescriptions[i];
            break;
        }
    }
    camera->format = *selected;
    const uint32_t fps = camera->format.FrameRateDenominator ? camera->format.FrameRateNumerator / camera->format.FrameRateDenominator : 30;
    if (camera->external) {
        if (!camera->connection || !camera->format.Width || camera->format.Width > 4096
            || !camera->format.Height || camera->format.Height > 4096 || !fps || fps > 120) return ERROR_INVALID_DATA;
        camera->endpointStarted.store(true);
        if (camera->epoch == camera->currentEpoch->load())
            Q_EMIT camera->connection->externalCameraFormat(camera->epoch, camera->format.Width, camera->format.Height, fps);
        return CHANNEL_RC_OK;
    }
    camera->endpoint = std::make_unique<PipeWireCamera>();
    if (!camera->endpoint->start(QString::number(reinterpret_cast<quintptr>(camera)), camera->format.Width, camera->format.Height, fps, camera->loopbackDevice)) {
        qCWarning(KRDP) << "Failed to create PipeWire remote camera source";
        camera->endpoint.reset();
        return ERROR_INTERNAL_ERROR;
    }
    camera->endpointStarted.store(true);
    qCInfo(KRDP) << "RDPECAM virtual camera available; waiting for a local consumer" << camera->format.Width << 'x' << camera->format.Height
                 << "format" << camera->format.Format;
    return CHANNEL_RC_OK;
}

bool startCameraIfRequested(RemoteCamera *camera)
{
    if (!camera || !camera->endpointStarted.load()) return true;
    const bool demand = camera->external ? camera->workerCapture->load() : camera->endpoint->captureRequested();
    if (camera->external && camera->streamStarted.load() && !demand
        && !camera->startPending.load() && !camera->stopPending.load()) {
        CAM_STOP_STREAMS_REQUEST stop{};
        camera->stopPending.store(true);
        const UINT status = camera->context->StopStreamsRequest(camera->context, &stop);
        if (status != CHANNEL_RC_OK) {
            camera->stopPending.store(false);
            qCWarning(KRDP) << "RDPECAM could not stop camera after worker demand ended" << status;
            return false;
        }
        camera->streamStarted.store(false);
        return true;
    }
    if (camera->streamStarted.load() || camera->startPending.load() || camera->stopPending.load() || !demand) {
        return true;
    }
    CAM_START_STREAMS_REQUEST request{};
    request.N_Infos = 1;
    request.StartStreamsInfo[0].StreamIndex = 0;
    request.StartStreamsInfo[0].MediaTypeDescription = camera->format;
    camera->startPending.store(true);
    const UINT status = camera->context->StartStreamsRequest(camera->context, &request);
    if (status != CHANNEL_RC_OK) {
        camera->startPending.store(false);
        qCWarning(KRDP) << "RDPECAM could not start camera on local demand" << status;
        return false;
    }
    camera->streamStarted.store(true);
    qCInfo(KRDP) << "RDPECAM starting camera for a local PipeWire/V4L2 consumer";
    return true;
}

UINT cameraSample(CameraDeviceServerContext *context, const CAM_SAMPLE_RESPONSE *response)
{
    auto *camera = static_cast<RemoteCamera *>(context->userdata);
    if (!camera || (!camera->external && !camera->endpoint) || response->StreamIndex != 0 || !response->Sample || !response->SampleSize) return ERROR_INVALID_DATA;
    if (camera->external && !camera->streamStarted.load()) return CHANNEL_RC_OK;
    if (!camera->receivedSample) {
        camera->receivedSample = true;
        qCInfo(KRDP) << "RDPECAM receiving camera samples (first frame bytes)" << response->SampleSize;
    }
    if (camera->external) {
        // The FreeRDP callback must never queue an unbounded number of webcam frames
        // while the desktop worker is slow or absent. The broker acknowledges even
        // a dropped frame after it has tried to write the worker socket.
        if (response->SampleSize <= 8 * 1024 * 1024 && camera->epoch == camera->currentEpoch->load()
            && !camera->framePending->exchange(true)) {
            Q_EMIT camera->connection->externalCameraFrame(camera->epoch,
                QByteArray(reinterpret_cast<const char *>(response->Sample), response->SampleSize));
        }
    } else {
        camera->endpoint->writeMjpeg(QByteArray(reinterpret_cast<const char *>(response->Sample), response->SampleSize));
    }
    CAM_SAMPLE_REQUEST request{};
    request.StreamIndex = 0;
    const UINT status = context->SampleRequest(context, &request);
    if (status != CHANNEL_RC_OK) qCWarning(KRDP) << "RDPECAM follow-up sample request failed" << status;
    return status;
}

struct RemoteCameraCollection {
    RemoteCameraSet<RemoteCamera> cameras;
    QString loopbackDevice;
    RdpConnection *connection = nullptr;
    bool external = false; // selected before initialization, like external microphone
    std::atomic<bool> workerReady = false;
    std::atomic<bool> workerCapture = false;
    std::atomic<bool> workerInUse = false;
    std::atomic<bool> framePending = false;
    std::atomic<quint64> epoch = 1;
    // The enumerator answered Select Version (enumerator thread): the client
    // accepted the channel, whether or not it then offers a camera.
    std::atomic<bool> versionSeen = false;
};

UINT cameraSelectVersion(CamDevEnumServerContext *context, const CAM_SELECT_VERSION_REQUEST *request)
{
    if (auto *collection = static_cast<RemoteCameraCollection *>(context->userdata)) {
        collection->versionSeen.store(true);
    }
    CAM_SELECT_VERSION_RESPONSE response{};
    response.Header = request->Header;
    response.Header.MessageId = CAM_MSG_ID_SelectVersionResponse;
    return context->SelectVersionResponse(context, &response);
}

UINT cameraAdded(CamDevEnumServerContext *enumerator, const CAM_DEVICE_ADDED_NOTIFICATION *device)
{
    qCInfo(KRDP) << "RDPECAM client camera available:" << QString::fromUtf16(reinterpret_cast<const char16_t *>(device->DeviceName)) << device->VirtualChannelName;
    auto *collection = static_cast<RemoteCameraCollection *>(enumerator->userdata);
    if (!collection || !device->VirtualChannelName) {
        return ERROR_INVALID_DATA;
    }
    // AUD-D1: a client may announce the same device again (for example when
    // it re-enumerates). A second device context on the same channel name
    // would fight the first for the DVC and publish a second PipeWire node.
    const QByteArray channelName(device->VirtualChannelName);
    if (collection->cameras.contains(channelName)) {
        qCInfo(KRDP) << "RDPECAM ignoring repeated DeviceAddedNotification for" << channelName;
        return CHANNEL_RC_OK;
    }
    if (collection->external && collection->cameras.size() >= 1) {
        qCInfo(KRDP) << "RDPECAM worker bridge is using the first offered camera";
        return CHANNEL_RC_OK;
    }
    auto camera = std::make_unique<RemoteCamera>();
    camera->loopbackDevice = collection->loopbackDevice;
    camera->connection = collection->connection;
    camera->external = collection->external;
    camera->workerCapture = &collection->workerCapture;
    camera->framePending = &collection->framePending;
    camera->currentEpoch = &collection->epoch;
    camera->epoch = collection->epoch.load();
    camera->context = camera_device_server_context_new(enumerator->vcm);
    if (!camera->context) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    camera->context->virtualChannelName = _strdup(device->VirtualChannelName);
    camera->context->protocolVersion = device->Header.Version;
    camera->context->userdata = camera.get();
    camera->context->ChannelIdAssigned = cameraChannelAssigned;
    camera->context->SuccessResponse = cameraSuccess;
    camera->context->StreamListResponse = cameraStreamList;
    camera->context->MediaTypeListResponse = cameraMediaTypes;
    camera->context->SampleResponse = cameraSample;
    if (!camera->context->virtualChannelName || camera->context->Initialize(camera->context, FALSE) != CHANNEL_RC_OK
        || camera->context->Open(camera->context) != CHANNEL_RC_OK) {
        return ERROR_INTERNAL_ERROR;
    }
    // Only this enumerator thread adds, so the name is still free.
    collection->cameras.insert(channelName, std::move(camera));
    return CHANNEL_RC_OK;
}

UINT cameraRemoved(CamDevEnumServerContext *enumerator, const CAM_DEVICE_REMOVED_NOTIFICATION *device)
{
    auto *collection = static_cast<RemoteCameraCollection *>(enumerator->userdata);
    if (!collection || !device->VirtualChannelName) {
        return ERROR_INVALID_DATA;
    }
    const QByteArray channelName(device->VirtualChannelName);
    std::unique_ptr<RemoteCamera> camera = collection->cameras.take(channelName);
    if (!camera) {
        // Not an error: the add may have failed, or been a duplicate.
        qCInfo(KRDP) << "RDPECAM DeviceRemovedNotification for an unknown camera" << channelName;
        return CHANNEL_RC_OK;
    }
    qCInfo(KRDP) << "RDPECAM client camera removed:" << channelName;
    // Outside the set's lock: ~RemoteCamera closes the device channel and
    // joins its FreeRDP thread (no callback can still use the endpoint), then
    // destroys the PipeWire endpoint, which removes the virtual camera node.
    camera.reset();
    return CHANNEL_RC_OK;
}
}
}

void KRdp::selectVaapiDriver()
{
    if (qEnvironmentVariableIsSet("LIBVA_DRIVER_NAME") && !g_autoAppliedVaapiDriver) {
        // Respect an externally-provided driver.
        return;
    }

    if (qEnvironmentVariableIsSet("FARSIDE_FORCE_VAAPI_DRIVER")) {
        const auto forcedDriver = qgetenv("FARSIDE_FORCE_VAAPI_DRIVER");
        if (!forcedDriver.isEmpty()) {
            qputenv("LIBVA_DRIVER_NAME", forcedDriver);
            g_autoAppliedVaapiDriver = true;
            qCInfo(KRDP) << "Using forced VAAPI driver from FARSIDE_FORCE_VAAPI_DRIVER:" << forcedDriver;
        }
        return;
    }
    if (qEnvironmentVariableIntValue("FARSIDE_AUTO_VAAPI_DRIVER") == 0 && qEnvironmentVariableIsSet("FARSIDE_AUTO_VAAPI_DRIVER")) {
        qCDebug(KRDP) << "Skipping automatic VAAPI driver selection due to FARSIDE_AUTO_VAAPI_DRIVER=0";
        return;
    }

    const auto nodes = renderNodes();
    if (nodes.empty()) {
        return;
    }

    const auto driver = preferredMixedGpuDriver(nodes);
    if (driver.isEmpty()) {
        return;
    }

    qputenv("LIBVA_DRIVER_NAME", driver);
    g_autoAppliedVaapiDriver = true;
    qCInfo(KRDP) << "Auto-selected VAAPI driver" << driver << "based on render-node vendors:" << vendorSummary(nodes);
}

namespace KRdp
{

#include <security/pam_appl.h>

typedef struct {
    QByteArray user;
    QByteArray password;
} RDPConnectionAuthData;

typedef struct {
    pam_handle_t *handle;
    struct pam_conv pamc;
    RDPConnectionAuthData appdata;
} RdpConnectionPamHandle;

static int pam_conv(int num_msg, const struct pam_message **msg, struct pam_response **resp, void *appdata_ptr)
{
    int pam_status = PAM_CONV_ERR;
    RDPConnectionAuthData *appdata = NULL;
    struct pam_response *response = NULL;
    WINPR_ASSERT(num_msg >= 0);
    appdata = (RDPConnectionAuthData *)appdata_ptr;
    WINPR_ASSERT(appdata);

    if (!(response = (struct pam_response *)calloc((size_t)num_msg, sizeof(struct pam_response))))
        return PAM_BUF_ERR;

    for (int index = 0; index < num_msg; index++) {
        switch (msg[index]->msg_style) {
        case PAM_PROMPT_ECHO_ON:
            response[index].resp = _strdup(appdata->user.constData());

            if (!response[index].resp)
                goto out_fail;

            response[index].resp_retcode = PAM_SUCCESS;
            break;

        case PAM_PROMPT_ECHO_OFF:
            response[index].resp = _strdup(appdata->password.constData());

            if (!response[index].resp)
                goto out_fail;

            response[index].resp_retcode = PAM_SUCCESS;
            break;

        default:
            pam_status = PAM_CONV_ERR;
            goto out_fail;
        }
    }

    *resp = response;
    return PAM_SUCCESS;
out_fail:

    for (int index = 0; index < num_msg; ++index) {
        if (response[index].resp) {
            memset(response[index].resp, 0, strlen(response[index].resp));
            free(response[index].resp);
        }
    }

    memset(response, 0, sizeof(struct pam_response) * (size_t)num_msg);
    free(response);
    *resp = NULL;
    return pam_status;
}

static std::optional<quint32> pamAuthenticate(const QString &user, const QString &password)
{
    int pam_status = 0;
    RdpConnectionPamHandle info = {0};

    info.appdata.user = user.toLatin1();
    info.appdata.password = password.toLatin1();
    info.pamc.conv = &pam_conv;
    info.pamc.appdata_ptr = &info.appdata;

    pam_status = pam_start("login", 0, &info.pamc, &info.handle);

    if (pam_status != PAM_SUCCESS) {
        qWarning() << "pam_start failure:" << pam_strerror(info.handle, pam_status);
        if (info.handle) pam_end(info.handle, pam_status);
        return {};
    }

    const auto finish = qScopeGuard([&] { pam_end(info.handle, pam_status); });

    pam_status = pam_authenticate(info.handle, 0);

    if (pam_status != PAM_SUCCESS) {
        qWarning() << "pam_authenticate failure:" << pam_strerror(info.handle, pam_status);
        return {};
    }

    pam_status = pam_acct_mgmt(info.handle, 0);

    if (pam_status != PAM_SUCCESS) {
        qWarning() << "pam_acct_mgmt failure:" << pam_strerror(info.handle, pam_status);
        return {};
    }
    // PAM modules may canonicalize/map the login. Resolve the final identity,
    // not the client-supplied username or a later KRDPCTL claim.
    const void *canonicalUser = nullptr;
    pam_status = pam_get_item(info.handle, PAM_USER, &canonicalUser);
    if (pam_status != PAM_SUCCESS || !canonicalUser || !*static_cast<const char *>(canonicalUser)) {
        return {};
    }
    const KUser account(QString::fromLocal8Bit(static_cast<const char *>(canonicalUser)));
    if (!account.isValid() || !account.userId().isValid()) {
        return {};
    }
    return quint32(account.userId().nativeId());
}

/**
 * FreeRDP callback for the capabilities event.
 */
BOOL peerCapabilities(freerdp_peer *peer)
{
    auto context = reinterpret_cast<PeerContext *>(peer->context);
    if (context->connection->onCapabilities()) {
        return TRUE;
    }

    return FALSE;
}

/**
 * FreeRDP callback for the post connect event.
 */
BOOL peerPostConnect(freerdp_peer *peer)
{
    auto context = reinterpret_cast<PeerContext *>(peer->context);
    if (context->connection->onPostConnect()) {
        return TRUE;
    }

    return FALSE;
}

/**
 * FreeRDP callback for the activate event.
 */
BOOL peerActivate(freerdp_peer *peer)
{
    auto context = reinterpret_cast<PeerContext *>(peer->context);
    if (context->connection->onActivate()) {
        return TRUE;
    }

    return FALSE;
}

BOOL suppressOutput(rdpContext *context, uint8_t allow, const RECTANGLE_16 *)
{
    auto peerContext = reinterpret_cast<PeerContext *>(context);
    if (peerContext->connection->onSuppressOutput(allow)) {
        return TRUE;
    }

    return FALSE;
}

// AUD-FIX10: FreeRDP_RefreshRect is advertised; a Refresh Rect PDU asks for a repaint. The
// encoded stream has no partial repaint, so every surface gets a keyframe (rate-limited).
BOOL refreshRect(rdpContext *context, BYTE count, const RECTANGLE_16 *)
{
    auto peerContext = reinterpret_cast<PeerContext *>(context);
    if (peerContext && peerContext->connection) {
        peerContext->connection->onRefreshRect(count);
    }
    return TRUE;
}

class KRDP_NO_EXPORT RdpConnection::Private
{
public:
    Server *server = nullptr;

    // AUD-P7: written on the main thread (initialize) and the session thread
    // (run/onClose), read from both and from VideoStream's threads.
    std::atomic<State> state = State::Initial;
    // Zero means no OS identity; uid+1 also represents uid0 without ambiguity.
    std::atomic<quint64> authenticatedPamUid = 0;
    std::atomic<quint64> authenticatedUserUid = 0;
    // AUD-S1: set once, on the session thread, when PostConnect authentication
    // succeeded. Until then channelGate drops every channel PDU, no input
    // callback is installed, KRDPCTL is not opened and clientDisplayInfoReceived
    // is not emitted.
    std::atomic<bool> authenticated = false;
    PreAuthChannelGate channelGate;

    qintptr socketHandle;

    std::unique_ptr<InputHandler> inputHandler;
    std::unique_ptr<VideoStream> videoStream;
    std::unique_ptr<Cursor> cursor;
    std::unique_ptr<NetworkDetection> networkDetection;
    std::unique_ptr<Clipboard> clipboard;

    /**
     * One device's consent and state (DEVICES-DESIGN.md §4). The consent and
     * the pending requests are written from any thread; everything below
     * "session thread" is only touched by the session loop's reconcilers.
     */
    struct DeviceSlot {
        DeviceConsent consent;
        std::atomic<bool> silenceHost = false; // playback: for the latest `on`
        std::mutex requestsMutex;
        // (generation the request produced, requestId): answered once that
        // generation is settled.
        std::vector<std::pair<uint64_t, QString>> requests;
        mutable std::mutex statusMutex;
        DeviceStatus published; // the last status, for deviceStatus()/`query`
        // Session thread.
        uint64_t applied = 0; // the consent generation the reconciler acted on
        uint64_t settled = 0; // the generation `status` is the outcome of
        DeviceStatus status;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::steady_clock::time_point nextInUsePoll;
        bool cameraRemoved = false; // camera: turned to error because the client removed it
    };
    std::array<DeviceSlot, 3> devices;
    DeviceSlot &slot(MediaDevice device)
    {
        return devices[size_t(device)];
    }
    std::atomic<bool> standardConsentPending = false;
    // The standard media channels the client joined, recorded at authentication.
    std::atomic<bool> joinedRdpsnd = false;
    std::atomic<bool> joinedDrdynvc = false;
    // KRDPCTL `capabilities` was queued, and then a record came back from the
    // client: our own client (isOwnClient()).
    std::atomic<bool> capabilitiesSent = false;
    std::atomic<bool> ownClient = false;

    RdpsndServerContext *rdpsnd = nullptr;
    RdpsndState rdpsndState;
    bool rdpsndClosed = false; // session thread: SNDC_CLOSE sent, the format must be selected again
    bool playbackSending = false; // session thread: the current playback period is `on`
    audin_server_context *audin = nullptr;
    std::unique_ptr<PipeWireMicrophone> microphoneEndpoint;
    AudinDelivery microphoneDelivery;
    bool externalMicrophone = false; // Chosen once, before queued initialize().
    MicrophonePcmQueue microphonePcm;
    std::unique_ptr<PipeWireAudioPlayback> audioPlaybackEndpoint;
    ExternalAudioQueue externalAudio;
    std::atomic<bool> externalAudioPlayback = false;
    std::atomic<int> audioPriorityOverride = -1;
    std::atomic<bool> audioPriorityDefault = false;
    CamDevEnumServerContext *cameraEnumerator = nullptr;
    RemoteCameraCollection remoteCameras;

    /** Any thread, after a consent change: keep the dependent queues in step. */
    void consentChanged(MediaDevice device)
    {
        if (device == MediaDevice::Microphone) {
            microphonePcm.reset(slot(MediaDevice::Microphone).consent.snapshot().generation);
        } else if (device == MediaDevice::Playback) {
            externalAudio.setEnabled(slot(MediaDevice::Playback).consent.snapshot().enabled && externalAudioPlayback.load());
        }
    }

    freerdp_peer *peer = nullptr;

    std::jthread thread;

    std::mutex clientDisplayMutex;
    ClientDisplay::Info clientDisplay;

    // KRDPCTL (OPT-044). The handle is opened and read on the session
    // thread; sendControlRecord() writes through it from any thread, so the
    // mutex orders those writes against the close in onClose().
    std::mutex controlChannelMutex;
    HANDLE controlChannel = nullptr;
    // Session thread only: the join was seen and the open attempted, once.
    bool controlChannelTried = false;
    // Whether the client has a usable control channel (joined AND opened);
    // set on the session thread before clientDisplayInfoReceived() is
    // emitted, read from wherever.
    std::atomic<bool> controlChannelOpen = false;
    LayoutControl::Deframer controlDeframer;

    // MS-RDPEDISP (setDisplayControlEnabled()). The context is created, used
    // and freed on the session thread; its own reader thread only calls
    // DispMonitorLayout, which emits displayLayoutRequested().
    std::atomic<bool> displayControlEnabled = false;
    bool displayControlTried = false;
    DispServerContext *displayControl = nullptr;
    std::atomic<UINT32> displayControlChannelId = 0;
    std::atomic<bool> displayControlCapsPending = false;
};

RdpConnection::RdpConnection(Server *server, qintptr socketHandle)
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
    d->server = server;
    d->socketHandle = socketHandle;
    d->remoteCameras.connection = this;

    d->inputHandler = std::make_unique<InputHandler>(this);
    d->videoStream = std::make_unique<VideoStream>(this);
    connect(d->videoStream.get(), &VideoStream::closed, this, [this]() {
        if (d->state == State::Running || d->state == State::Streaming) {
            qCInfo(KRDP) << "Video stream closed, closing session";
            d->peer->Close(d->peer);
        }
    });
    d->cursor = std::make_unique<Cursor>(this);
    d->networkDetection = std::make_unique<NetworkDetection>(this);
    d->clipboard = std::make_unique<Clipboard>(this);
    // AUD-FIX5: the clipboard asks the client for nothing before the client has a picture.
    connect(d->videoStream.get(), &VideoStream::graphicsDelivered, d->clipboard.get(), &Clipboard::setGraphicsDelivered, Qt::QueuedConnection);

    QMetaObject::invokeMethod(this, &RdpConnection::initialize, Qt::QueuedConnection);
}

RdpConnection::~RdpConnection()
{
    if (d->state == State::Streaming) {
        d->peer->Close(d->peer);
    }

    if (d->thread.joinable()) {
        d->thread.request_stop();
        d->thread.join();
    }

    if (d->peer) {
        // The transport owns the socket once the peer is initialized
        // (freerdp_peer::sockfd is -1 from then on) and nothing else closes
        // it: without this every closed connection kept its descriptor, and a
        // client dropped by the handshake timeout never saw the close.
        // freerdp_peer_context_free() frees the transport (which disconnects
        // it, closing the socket once), the rdp state and, through
        // freePeerContext(), the virtual channel manager; before AUD-INT only
        // the peer struct was freed and every closed connection leaked its
        // context. Safe here: the session thread has been joined and onClose()
        // already closed every channel opened on that channel manager.
        // freerdp_peer_free() then closes a socket the transport never took.
        freerdp_peer_context_free(d->peer);
        freerdp_peer_free(d->peer);
        d->peer = nullptr;
    }
}

RdpConnection::State RdpConnection::state() const
{
    return d->state;
}

std::optional<quint32> RdpConnection::authenticatedPamUid() const
{
    const auto encoded = d->authenticatedPamUid.load();
    return encoded ? std::optional<quint32>(quint32(encoded - 1)) : std::nullopt;
}

std::optional<quint32> RdpConnection::authenticatedUserUid() const
{
    const auto encoded = d->authenticatedUserUid.load();
    return encoded ? std::optional<quint32>(quint32(encoded - 1)) : std::nullopt;
}

bool RdpConnection::isAuthenticated() const
{
    return d->authenticated.load();
}

rdpContext *RdpConnection::freerdpContext() const
{
    return d->peer ? d->peer->context : nullptr;
}

void RdpConnection::setState(KRdp::RdpConnection::State newState)
{
    if (d->state.exchange(newState) == newState) {
        return;
    }

    Q_EMIT stateChanged(newState);
}

void RdpConnection::closeWithErrorInfo(quint32 errorInfo)
{
    if (d->peer && d->peer->context && d->peer->context->rdp) {
        freerdp_set_error_info(d->peer->context->rdp, errorInfo);
        // As for AuthenticationFailed: a client that already processed the
        // Deactivate All that Close() sends first ignores a later Set Error Info.
        freerdp_send_error_info(d->peer->context->rdp);
    }
    close(CloseReason::None);
}

void RdpConnection::setDisplayControlEnabled(bool enabled)
{
    d->displayControlEnabled.store(enabled);
}

void RdpConnection::close(RdpConnection::CloseReason reason)
{
    switch (reason) {
    case CloseReason::VideoInitFailed:
        if (d->peer && d->peer->context) {
            freerdp_set_error_info(d->peer->context->rdp, ERRINFO_GRAPHICS_SUBSYSTEM_FAILED);
        }
        break;
    case CloseReason::AuthenticationFailed:
        freerdp_set_error_info(d->peer->context->rdp, ERRINFO_SERVER_INSUFFICIENT_PRIVILEGES);
        // Send it now as well: Close() sends Deactivate All first, and a client in the
        // deactivated state ignores the Set Error Info that follows it, reporting the
        // ultimatum as ERRINFO_LOGOFF_BY_USER instead.
        freerdp_send_error_info(d->peer->context->rdp);
        break;
    case CloseReason::None:
        break;
    }

    if (d->peer) { // may be null if creating the peer failed
        d->peer->Close(d->peer);
    }
}

InputHandler *RdpConnection::inputHandler() const
{
    return d->inputHandler.get();
}

KRdp::VideoStream *RdpConnection::videoStream() const
{
    return d->videoStream.get();
}

Cursor *RdpConnection::cursor() const
{
    return d->cursor.get();
}

Clipboard *RdpConnection::clipboard() const
{
    return d->clipboard.get();
}

ClientDisplay::Info RdpConnection::clientDisplayInfo() const
{
    std::lock_guard lock(d->clientDisplayMutex);
    return d->clientDisplay;
}

qint64 RdpConnection::socketQueuedBytes() const
{
    if (d->socketHandle < 0 || d->state == State::Closed) {
        return -1;
    }
    int queued = 0;
    if (::ioctl(int(d->socketHandle), SIOCOUTQ, &queued) != 0) {
        return -1;
    }
    return queued;
}

std::optional<RdpConnection::TcpInfo> RdpConnection::tcpInfo() const
{
    if (d->socketHandle < 0 || d->state == State::Closed) {
        return std::nullopt;
    }
    tcp_info info{};
    socklen_t length = sizeof(info);
    if (::getsockopt(int(d->socketHandle), IPPROTO_TCP, TCP_INFO, &info, &length) != 0 || length < offsetof(tcp_info, tcpi_total_retrans) + sizeof(info.tcpi_total_retrans)) {
        return std::nullopt;
    }
    TcpInfo result{qint64(info.tcpi_rtt), qint64(info.tcpi_rttvar), quint64(info.tcpi_total_retrans)};
    if (length >= offsetof(tcp_info, tcpi_delivery_rate) + sizeof(info.tcpi_delivery_rate)) {
        // glibc's tcp_info leaves out <linux/tcp.h>'s tcpi_delivery_rate_app_limited: bit 0 of the
        // byte after tcpi_snd_wscale/tcpi_rcv_wscale (the one before tcpi_rto).
        static_assert(offsetof(tcp_info, tcpi_rto) == 8, "tcp_info layout: the app-limited bit is byte 7");
        const auto *bytes = reinterpret_cast<const unsigned char *>(&info);
        result.deliveryRateBytesPerSecond = quint64(info.tcpi_delivery_rate);
        result.deliveryRateAppLimited = (bytes[offsetof(tcp_info, tcpi_rto) - 1] & 0x1) != 0;
    }
    if (length >= offsetof(tcp_info, tcpi_min_rtt) + sizeof(info.tcpi_min_rtt)) {
        result.minRttUs = qint64(info.tcpi_min_rtt == ~0u ? 0 : info.tcpi_min_rtt);
    }
    if (length >= offsetof(tcp_info, tcpi_rwnd_limited) + sizeof(info.tcpi_rwnd_limited)) {
        result.busyUs = quint64(info.tcpi_busy_time);
        result.rwndLimitedUs = quint64(info.tcpi_rwnd_limited);
    }
    return result;
}

NetworkDetection *RdpConnection::networkDetection() const
{
    return d->networkDetection.get();
}

bool RdpConnection::hasControlChannel() const
{
    return d->controlChannelOpen.load();
}

void RdpConnection::sendControlRecord(const QJsonObject &record)
{
    const QByteArray data = LayoutControl::frame(record);
    std::lock_guard lock(d->controlChannelMutex);
    if (!d->controlChannel) {
        qCWarning(KRDP) << "KRDPCTL: dropping a" << record.value(QLatin1String("type")).toString() << "record, the channel is not open";
        return;
    }
    // Only queues the bytes with the channel manager (and wakes the session
    // thread through its event handle); WTSVirtualChannelManagerCheckFileDescriptor()
    // in run() is what sends them.
    ULONG written = 0;
    if (!WTSVirtualChannelWrite(d->controlChannel, const_cast<char *>(data.constData()), ULONG(data.size()), &written)) {
        qCWarning(KRDP) << "KRDPCTL: could not queue a" << record.value(QLatin1String("type")).toString() << "record";
    } else if (record.value(QLatin1String("type")).toString() == QLatin1String("capabilities")) {
        d->capabilitiesSent.store(true);
    }
}

void RdpConnection::requestDevice(MediaDevice device, DeviceControl::Action action, bool silenceHost, const QString &requestId)
{
    auto &slot = d->slot(device);
    if (action == DeviceControl::Action::Query) {
        Q_EMIT deviceState(device, deviceStatus(device), requestId);
        return;
    }
    uint64_t generation = 0;
    if (action == DeviceControl::Action::Off) {
        slot.consent.setEnabled(false);
        generation = slot.consent.snapshot().generation;
    } else {
        // Before renew(): the session loop reads it once it sees the new generation.
        slot.silenceHost.store(device == MediaDevice::Playback && action == DeviceControl::Action::On && silenceHost);
        generation = slot.consent.renew();
    }
    d->consentChanged(device);
    {
        // Even without a requestId: the answer is then uncorrelated, but sent.
        std::lock_guard lock(slot.requestsMutex);
        slot.requests.emplace_back(generation, requestId);
    }
    qCInfo(KRDP) << "Device request:" << DeviceControl::deviceName(device) << "action" << int(action) << "silence host" << slot.silenceHost.load()
                 << "generation" << generation;
}

void RdpConnection::setDeviceEnabled(MediaDevice device, bool enabled, bool silenceHost)
{
    auto &slot = d->slot(device);
    if (device == MediaDevice::Camera && d->remoteCameras.external) {
        d->remoteCameras.epoch.fetch_add(1);
        d->remoteCameras.framePending.store(false);
    }
    if (device == MediaDevice::Playback) {
        slot.silenceHost.store(enabled && silenceHost);
    }
    slot.consent.setEnabled(enabled);
    d->consentChanged(device);
}

bool RdpConnection::applyStandardConsent()
{
    if (!d->server || !d->server->standardClientMedia()) {
        return false;
    }
    // The session loop decides per device from what the client joined.
    d->standardConsentPending.store(true);
    return true;
}

std::optional<RdpConnection::StandardMediaChannels> RdpConnection::standardMediaChannels() const
{
    if (!d->server || !d->server->standardClientMedia()) {
        return std::nullopt;
    }
    return StandardMediaChannels{d->joinedRdpsnd.load(), d->joinedDrdynvc.load()};
}

DeviceStatus RdpConnection::deviceStatus(MediaDevice device) const
{
    auto &slot = d->slot(device);
    std::lock_guard lock(slot.statusMutex);
    return slot.published;
}

bool RdpConnection::isOwnClient() const
{
    return d->ownClient.load();
}

void RdpConnection::publishDevice(MediaDevice device, const DeviceStatus &status, std::optional<uint64_t> settledGeneration)
{
    auto &slot = d->slot(device);
    const bool changed = status != slot.status;
    slot.status = status;
    {
        std::lock_guard lock(slot.statusMutex);
        slot.published = status;
    }
    if (settledGeneration) {
        slot.settled = *settledGeneration;
    }
    if (changed) {
        qCInfo(KRDP) << "Device" << DeviceControl::deviceName(device) << "is" << DeviceControl::stateName(status.state) << "in use" << status.inUse
                     << status.code << status.message;
    }
    // Requests are answered once their generation is settled; a change that
    // answers none is pushed (unsolicited, no requestId). `starting` is never
    // pushed: the client already knows it asked.
    std::vector<std::pair<uint64_t, QString>> answered;
    if (status.state != DeviceStatus::State::Starting) {
        std::lock_guard lock(slot.requestsMutex);
        const auto settled = std::stable_partition(slot.requests.begin(), slot.requests.end(), [&slot](const auto &request) {
            return request.first > slot.settled;
        });
        std::move(settled, slot.requests.end(), std::back_inserter(answered));
        slot.requests.erase(settled, slot.requests.end());
    }
    for (const auto &request : answered) {
        Q_EMIT deviceState(device, status, request.second);
    }
    if (changed && answered.empty() && status.state != DeviceStatus::State::Starting) {
        Q_EMIT deviceState(device, status, QString());
    }
}

void RdpConnection::setExternalAudioPlayback(bool enabled)
{
    d->externalAudioPlayback.store(enabled);
    d->consentChanged(MediaDevice::Playback);
}

void RdpConnection::setAudioPriority(bool enabled)
{
    d->audioPriorityOverride.store(enabled ? 1 : 0);
}

void RdpConnection::setAudioPriorityDefault(bool enabled)
{
    d->audioPriorityDefault.store(enabled);
}

void RdpConnection::clearAudioPriorityOverride()
{
    d->audioPriorityOverride.store(-1);
}

bool RdpConnection::audioPriorityActive() const
{
    const int override = d->audioPriorityOverride.load();
    return AdaptiveQuality::audioPriorityEnabled(override < 0 ? d->audioPriorityDefault.load() : override != 0,
                                                d->slot(MediaDevice::Playback).consent.snapshot().enabled,
                                                d->slot(MediaDevice::Microphone).consent.snapshot().enabled);
}

bool RdpConnection::enableExternalMicrophone()
{
    if (d->state != State::Initial) return false;
    d->externalMicrophone = true;
    return true;
}

bool RdpConnection::enableExternalCamera()
{
    if (d->state != State::Initial) return false;
    d->remoteCameras.external = true;
    return true;
}

void RdpConnection::setExternalCameraState(bool ready, bool capture, bool inUse)
{
    if (!d->remoteCameras.external) return;
    d->remoteCameras.workerReady.store(ready);
    d->remoteCameras.workerCapture.store(ready && capture);
    d->remoteCameras.workerInUse.store(ready && inUse);
}

void RdpConnection::acknowledgeExternalCameraFrame(quint64 epoch)
{
    if (epoch == d->remoteCameras.epoch.load()) d->remoteCameras.framePending.store(false);
}

quint64 RdpConnection::externalCameraEpoch() const
{
    return d->remoteCameras.epoch.load();
}

QByteArray RdpConnection::takeExternalMicrophone()
{
    if (!d->externalMicrophone) return {};
    QByteArray pcm;
    auto &microphone = d->slot(MediaDevice::Microphone).consent;
    const auto consent = microphone.snapshot();
    microphone.deliver(consent.generation, [&] {
        pcm = d->microphonePcm.take(consent.generation);
    });
    return pcm;
}

void RdpConnection::submitExternalAudio(const QByteArray &pcm)
{
    if (!d->externalAudioPlayback.load() || pcm.isEmpty() || pcm.size() % 4 != 0) {
        return;
    }
    d->externalAudio.submit(pcm);
}

void RdpConnection::openDisplayControl()
{
    if (d->displayControlTried || !d->displayControlEnabled.load()) {
        return;
    }
    d->displayControlTried = true;
    const auto vcm = reinterpret_cast<PeerContext *>(d->peer->context)->virtualChannelManager;
    auto *context = disp_server_context_new(vcm);
    if (!context) {
        qCWarning(KRDP) << "Display Control: cannot create the server context";
        return;
    }
    context->custom = this;
    context->rdpcontext = d->peer->context;
    // RDPGFX's limits: 16 monitors; FreeRDP checks each one against 8192.
    context->MaxNumMonitors = ClientDisplay::MaxMonitors;
    context->MaxMonitorAreaFactorA = ClientDisplay::MaxDesktopDimension;
    context->MaxMonitorAreaFactorB = ClientDisplay::MaxDesktopDimension;
    context->ChannelIdAssigned = [](DispServerContext *context, UINT32 channelId) -> BOOL {
        static_cast<RdpConnection *>(context->custom)->d->displayControlChannelId.store(channelId);
        return TRUE;
    };
    context->DispMonitorLayout = [](DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *pdu) -> UINT {
        // The channel's reader thread. FreeRDP has already checked the count
        // against MaxNumMonitors and each size against the protocol's limits.
        QList<VideoMonitor> monitors;
        for (UINT32 i = 0; pdu && i < pdu->NumMonitors; ++i) {
            const auto &monitor = pdu->Monitors[i];
            monitors.append(VideoMonitor{
                .geometry = QRect(monitor.Left, monitor.Top, int(monitor.Width), int(monitor.Height)),
                .primary = (monitor.Flags & DISPLAY_CONTROL_MONITOR_PRIMARY) != 0,
            });
        }
        auto *connection = static_cast<RdpConnection *>(context->custom);
        qCInfo(KRDP) << "Display Control: client asks for" << monitors.size() << "monitor(s)"
                     << (monitors.isEmpty() ? QSize() : monitors.first().geometry.size());
        Q_EMIT connection->displayLayoutRequested(monitors);
        return CHANNEL_RC_OK;
    };
    // The caps PDU may only go out once the client accepted the channel: the
    // channel manager reports every DVC's creation status on the session thread.
    const psDVCCreationStatusCallback created = [](void *userdata, UINT32 channelId, INT32 creationStatus) -> BOOL {
        auto *p = static_cast<Private *>(userdata);
        if (p && channelId != 0 && channelId == p->displayControlChannelId.load() && creationStatus >= 0) {
            p->displayControlCapsPending.store(true);
        }
        return TRUE;
    };
    WTSVirtualChannelManagerSetDVCCreationCallback(vcm, created, d.get());
    if (context->Open(context) != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "Display Control: cannot open the channel";
        WTSVirtualChannelManagerSetDVCCreationCallback(vcm, nullptr, nullptr);
        disp_server_context_free(context);
        return;
    }
    d->displayControl = context;
}

void RdpConnection::closeDisplayControl()
{
    if (!d->displayControl) {
        return;
    }
    if (d->peer && d->peer->context) {
        const auto vcm = reinterpret_cast<PeerContext *>(d->peer->context)->virtualChannelManager;
        WTSVirtualChannelManagerSetDVCCreationCallback(vcm, nullptr, nullptr);
    }
    // Joins the channel's reader thread before freeing.
    disp_server_context_free(d->displayControl);
    d->displayControl = nullptr;
    d->displayControlCapsPending.store(false);
}

void RdpConnection::openControlChannel()
{
    if (d->controlChannelTried) {
        return;
    }
    auto context = reinterpret_cast<PeerContext *>(d->peer->context);
    if (!WTSVirtualChannelManagerIsChannelJoined(context->virtualChannelManager, ControlChannelName)) {
        return;
    }
    d->controlChannelTried = true;
    // Opened as soon as the join is visible, well before the client can send
    // on it (its channel plugins only start after activation): data for a
    // joined-but-unopened static channel is dropped by FreeRDP, not queued.
    HANDLE channel = WTSVirtualChannelOpen(context->virtualChannelManager, WTS_CURRENT_SESSION, ControlChannelName);
    if (!channel) {
        // Treated as a client without the channel: hasControlChannel() stays
        // false, so the session build is not held for a record that cannot
        // arrive.
        qCWarning(KRDP) << "KRDPCTL: the client joined the channel but it could not be opened; serving it as a client without one";
        return;
    }
    {
        std::lock_guard lock(d->controlChannelMutex);
        d->controlChannel = channel;
    }
    d->controlChannelOpen = true;
    qCInfo(KRDP) << "KRDPCTL: channel joined and opened";
}

bool RdpConnection::readControlChannel()
{
    HANDLE channel = nullptr;
    {
        std::lock_guard lock(d->controlChannelMutex);
        channel = d->controlChannel;
    }
    if (!channel) {
        return true;
    }
    // The channel manager queues one message per complete channel PDU (the
    // chunks are reassembled for us); the first call sizes it, the second
    // takes it. The buffer is never empty even for a zero-length message,
    // which WTSVirtualChannelRead would otherwise leave queued forever.
    for (;;) {
        ULONG length = 0;
        if (!WTSVirtualChannelRead(channel, 0, nullptr, 0, &length)) {
            break;
        }
        QByteArray buffer(int(qMax<ULONG>(length, 1)), Qt::Uninitialized);
        ULONG read = 0;
        if (!WTSVirtualChannelRead(channel, 0, buffer.data(), ULONG(buffer.size()), &read)) {
            break;
        }
        buffer.resize(int(read));
        d->controlDeframer.feed(buffer);
        while (auto record = d->controlDeframer.next()) {
            if (d->capabilitiesSent.load()) {
                // It heard `capabilities` and answered: it speaks KRDPCTL v2.
                d->ownClient.store(true);
            }
            Q_EMIT controlRecordReceived(*record);
        }
        // A payload that is not a JSON object is consumed, not a record: it
        // never reaches the gate, but the client is told at once rather than
        // left to wait out the gate's timeout in silence.
        for (int invalid = d->controlDeframer.takeInvalidCount(); invalid > 0; --invalid) {
            qCWarning(KRDP) << "KRDPCTL: record payload is not a JSON object; replying error invalid";
            sendControlRecord(LayoutControl::errorRecord({QStringLiteral("invalid"), QStringLiteral("record payload is not a JSON object")}));
        }
        if (d->controlDeframer.overflowed()) {
            qCWarning(KRDP) << "KRDPCTL: record longer than 64 KiB announced; closing the connection";
            return false;
        }
    }
    return true;
}

void RdpConnection::initialize()
{
    if (d->socketHandle < 0) {
        // A detached connection object (no client socket, see the
        // constructor): there is nothing to set up, and nothing to close.
        qCDebug(KRDP) << "Connection has no socket; not initializing";
        return;
    }

    setState(State::Starting);

    // AUD-P3: every early return below closes the peer (and with it the
    // client socket) and moves the connection to Closed, which is what makes
    // Server drop it. Without this a failed setup leaked the connection, its
    // socket and the client waiting on it.
    auto fail = qScopeGuard([this]() {
        if (d->peer) {
            freerdp_peer_context_free(d->peer); // frees the transport, closing an attached socket
            freerdp_peer_free(d->peer); // closes a socket the transport never took
            d->peer = nullptr;
        } else if (d->socketHandle >= 0) {
            ::close(int(d->socketHandle));
        }
        d->socketHandle = -1;
        // Queued on the server: Server deletes the connection when it sees
        // Closed, and that must not happen inside this call.
        QMetaObject::invokeMethod(
            d->server,
            [connection = QPointer<RdpConnection>(this)]() {
                if (connection) {
                    connection->setState(State::Closed);
                }
            },
            Qt::QueuedConnection);
    });

    d->peer = freerdp_peer_new(d->socketHandle);
    if (!d->peer) {
        qCWarning(KRDP) << "Failed to create peer";
        return;
    }

    // Create an instance of our custom PeerContext extended context as context
    // rather than the plain rdpContext.
    d->peer->ContextSize = sizeof(PeerContext);
    d->peer->ContextNew = (psPeerContextNew)newPeerContext;
    d->peer->ContextFree = (psPeerContextFree)freePeerContext;

    auto result = freerdp_peer_context_new_ex(d->peer, d->server->rdpSettings());
    if (!result) {
        qCWarning(KRDP) << "Failed to create peer context";
        return;
    }

    auto context = reinterpret_cast<PeerContext *>(d->peer->context);
    context->connection = this;

    auto settings = d->peer->context->settings;

    auto certificate = freerdp_certificate_new_from_file(d->server->tlsCertificate().string().data());
    if (!certificate) {
        qCWarning(KRDP) << "Could not read certificate file" << d->server->tlsCertificate().string();
        return;
    }
    freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, certificate, 1);

    auto key = freerdp_key_new_from_file(d->server->tlsCertificateKey().string().data());
    if (!key) {
        qCWarning(KRDP) << "Could not read certificate key file" << d->server->tlsCertificateKey().string();
        return;
    }
    freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1);

    freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, false);
    freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, true);
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, false);

    freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
    // PSEUDO_XSERVER is apparently required for things to work properly.
    freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_PSEUDO_XSERVER);

    // RDPSND is a client-selected static channel. Setting AudioPlayback here
    // makes FreeRDP add it for every client (including clients that selected
    // no audio), so leave it off and only initialize it when a client has
    // explicitly joined RDPSND. AUDIN remains a client-selected DVC.
    freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, false);
    freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, true);

    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

    // The RDPGFX pipeline with H.264. AVC444/AVC444v2 are advertised unless the configuration
    // pins the codec to AVC420; the codec is chosen per client in VideoStream::onCapsAdvertise().
    const bool avc444 = d->videoStream->codecPreference() != CodecPreference::Avc420;
    freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, true);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, avc444);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, avc444);
    freerdp_settings_set_bool(settings, FreeRDP_GfxH264, true);


    freerdp_settings_set_bool(settings, FreeRDP_GfxSmallCache, false);
    freerdp_settings_set_bool(settings, FreeRDP_GfxThinClient, false);

    freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, true);
    freerdp_settings_set_bool(settings, FreeRDP_HasRelativeMouseEvent, true);
    freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, true);
    freerdp_settings_set_bool(settings, FreeRDP_UnicodeInput, true);

    // TODO: Implement network performance detection
    freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, true);

    freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, true);
    freerdp_settings_set_bool(settings, FreeRDP_RemoteConsoleAudio, true);
    freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, false);
    freerdp_settings_set_bool(settings, FreeRDP_NSCodec, false);
    freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, true);
    freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled, true);

    d->peer->Capabilities = peerCapabilities;
    d->peer->Activate = peerActivate;
    d->peer->PostConnect = peerPostConnect;

    d->peer->context->update->SuppressOutput = suppressOutput;
    d->peer->context->update->RefreshRect = refreshRect;

    // AUD-S1: every static (and so every dynamic) channel PDU goes through the
    // gate; the channel manager installed its hook in newPeerContext().
    d->channelGate.install(d->peer);
    // The input callbacks are installed by onAuthenticated(): FreeRDP accepts
    // input PDUs during connection finalization, before PostConnect.
    context->inputHandler = d->inputHandler.get();

    context->networkDetection = d->networkDetection.get();
    d->networkDetection->initialize();

    if (!d->peer->Initialize(d->peer)) {
        qCWarning(KRDP) << "Unable to initialize peer";
        return;
    }

    fail.dismiss();
    qCInfo(KRDP) << "Session setup completed, start processing...";

    // AUD-S2: a client that has not authenticated within the handshake
    // timeout is dropped. A timer rather than a check in run(): a stalled TLS
    // handshake blocks the session thread inside freerdp_tls_accept() until
    // the context's abort event is set or the socket fails.
    QTimer::singleShot(d->server->handshakeTimeout(), this, [this]() {
        if (d->authenticated.load() || d->state == State::Closed || !d->peer || !d->peer->context) {
            return;
        }
        qCWarning(KRDP) << "Client did not authenticate within" << d->server->handshakeTimeout().count() << "ms; closing the connection";
        // The abort event ends a wait inside FreeRDP; shutting the socket
        // down ends a blocking read (the transport owns the descriptor, but
        // it is still the one Server accepted).
        freerdp_abort_connect_context(d->peer->context);
        ::shutdown(int(d->socketHandle), SHUT_RDWR);
    });

    // Perform actual communication on a separate thread.
    d->thread = std::jthread(std::bind(&RdpConnection::run, this, std::placeholders::_1));
    pthread_setname_np(d->thread.native_handle(), "krdp_session");
}

void RdpConnection::run(std::stop_token stopToken)
{
    auto context = reinterpret_cast<PeerContext *>(d->peer->context);
    auto channelEvent = WTSVirtualChannelManagerGetEventHandle(context->virtualChannelManager);

    setState(State::Running);

    while (!stopToken.stop_requested()) {
        std::array<HANDLE, 32> events{channelEvent};
        auto handleCount = d->peer->GetEventHandles(d->peer, events.data() + 1, 31);
        if (handleCount <= 0) {
            qCDebug(KRDP) << "Unable to get transport event handles";
            break;
        }
        // Bounded, not INFINITE: NetworkDetection::update() (RTT probe every
        // 70 ms, bandwidth window stop after 500 ms) must run on an idle link
        // too. Waiting only for socket activity stretched idle bandwidth
        // windows to 1.0-1.6 s in the 2026-09-16 journal.
        WaitForMultipleObjects(1 + handleCount, events.data(), FALSE, 100);

        // Read data from the socket and have FreeRDP process it.
        if (d->peer->CheckFileDescriptor(d->peer) != TRUE) {
            qCDebug(KRDP) << "Unable to check file descriptor";
            break;
        }

        // AUD-S1: nothing below serves a client that has not authenticated.
        // Its channel PDUs never got past the gate, so there is nothing to
        // read; only the channel manager's own queue is still serviced.
        if (!d->authenticated.load()) {
            if (WaitForSingleObject(channelEvent, 0) == WAIT_OBJECT_0 && WTSVirtualChannelManagerCheckFileDescriptor(context->virtualChannelManager) != TRUE) {
                qCWarning(KRDP) << "Unable to check Virtual Channel Manager file descriptor, closing connection";
                break;
            }
            continue;
        }

        // Initialize any dynamic channels once the dynamic channel channel is setup.
        if (d->peer->connected && WTSVirtualChannelManagerIsChannelJoined(context->virtualChannelManager, DRDYNVC_SVC_CHANNEL_NAME)) {
            auto state = WTSVirtualChannelManagerGetDrdynvcState(context->virtualChannelManager);
            // Dynamic channels can only be set up properly once the dynamic channel channel is properly setup.
            if (state == DRDYNVC_STATE_READY) {
                openDisplayControl();
                if (d->videoStream->initialize()) {
                    d->videoStream->setEnabled(true);
                    setState(State::Streaming);
                } else {
                    break;
                }
            } else if (state == DRDYNVC_STATE_NONE) {
                // This ensures that WTSVirtualChannelManagerCheckFileDescriptor() will be called, which initializes the drdynvc channel.
                SetEvent(channelEvent);
            }
        }

        if (WaitForSingleObject(channelEvent, 0) == WAIT_OBJECT_0 && WTSVirtualChannelManagerCheckFileDescriptor(context->virtualChannelManager) != TRUE) {
            qCWarning(KRDP) << "Unable to check Virtual Channel Manager file descriptor, closing connection";
            break;
        }

        if (d->displayControl && d->displayControlCapsPending.exchange(false)) {
            // No reply is expected: the client starts sending layouts when it wants a change.
            if (d->displayControl->DisplayControlCaps(d->displayControl) != CHANNEL_RC_OK) {
                qCWarning(KRDP) << "Display Control: cannot send the capabilities";
            }
        }

        if (d->peer->connected && WTSVirtualChannelManagerIsChannelJoined(context->virtualChannelManager, CLIPRDR_SVC_CHANNEL_NAME)) {
            if (!d->clipboard->initialize()) {
                break;
            }
        }

        if (!reconcileDevices()) {
            break;
        }

        d->remoteCameras.cameras.forEach(startCameraIfRequested);

        if (d->rdpsnd && d->rdpsndState.active.load() && d->playbackSending) {
            const auto send = [&](const QByteArray &pcm) {
                if (pcm.isEmpty() || !d->rdpsnd->SendSamples) return;
                const auto frames = size_t(pcm.size() / d->rdpsnd->src_format->nBlockAlign);
                if (frames > 0) {
                    d->rdpsnd->SendSamples(d->rdpsnd, pcm.constData(), frames, UINT16(GetTickCount64() & 0xffff));
                }
            };
            if (d->externalAudioPlayback.load()) {
                d->externalAudio.deliver(send);
            } else if (d->audioPlaybackEndpoint) {
                send(d->audioPlaybackEndpoint->take());
            }
        }

        // KRDPCTL (OPT-044): opened by onAuthenticated(); the client's
        // records arrive through CheckFileDescriptor() above, queued on the
        // channel.
        if (!readControlChannel()) {
            break;
        }

        d->networkDetection->update();
    }

    qCInfo(KRDP) << "Closing session";
    onClose();
}

bool RdpConnection::onCapabilities()
{
    auto settings = d->peer->context->settings;
    // We only support GraphicsPipeline clients currently as that is required
    // for AVC streaming.
    if (!freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline)) {
        qCWarning(KRDP) << "Client does not support graphics pipeline which is required";
        return false;
    }

    auto colorDepth = freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth);
    if (colorDepth != 32) {
        qCDebug(KRDP) << "Correcting invalid color depth from client:" << colorDepth;
        freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
    }

    if (!freerdp_settings_get_bool(settings, FreeRDP_DesktopResize)) {
        qCWarning(KRDP) << "Client doesn't support resizing, aborting";
        return false;
    }

    if (freerdp_settings_get_uint32(settings,FreeRDP_PointerCacheSize) <= 0) {
        qCWarning(KRDP) << "Client doesn't support pointer caching, aborting";
        return false;
    }

    ClientDisplay::Info info;
    info.desktopSize = QSize(int(freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth)), int(freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight)));
    const auto monitorCount = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
    for (UINT32 i = 0; i < monitorCount; ++i) {
        const auto *monitor = static_cast<const rdpMonitor *>(freerdp_settings_get_pointer_array(settings, FreeRDP_MonitorDefArray, i));
        if (!monitor) {
            break;
        }
        info.monitors.push_back(VideoMonitor{
            .geometry = QRect(monitor->x, monitor->y, monitor->width, monitor->height),
            .primary = monitor->is_primary != 0,
        });
    }
    {
        std::lock_guard lock(d->clientDisplayMutex);
        d->clientDisplay = info;
    }
    qCInfo(KRDP) << "Client display: desktop" << info.desktopSize << "monitors" << info.monitors.size()
                 << "monitorLayoutPdu" << freerdp_settings_get_bool(settings, FreeRDP_SupportMonitorLayoutPdu)
                 << "KRDPCTL joined" << WTSVirtualChannelManagerIsChannelJoined(reinterpret_cast<PeerContext *>(d->peer->context)->virtualChannelManager, ControlChannelName);
    // AUD-S1: the first capabilities exchange precedes PostConnect, so the
    // display info is announced by onAuthenticated(). A reactivation (already
    // authenticated) announces it again, as before.
    if (d->authenticated.load()) {
        Q_EMIT clientDisplayInfoReceived();
    }

    return true;
}

void RdpConnection::onAuthenticated()
{
    d->authenticated.store(true);
    d->channelGate.authorize();
    d->inputHandler->initialize(d->peer->context->input);
    {
        // Brokers read these from the main thread (standardMediaChannels()).
        const auto vcm = reinterpret_cast<PeerContext *>(d->peer->context)->virtualChannelManager;
        d->joinedRdpsnd.store(WTSVirtualChannelManagerIsChannelJoined(vcm, RDPSND_CHANNEL_NAME));
        d->joinedDrdynvc.store(WTSVirtualChannelManagerIsChannelJoined(vcm, DRDYNVC_SVC_CHANNEL_NAME));
    }
    // The MCS channel join was complete long before PostConnect, so
    // hasControlChannel() is exact for the clientDisplayInfoReceived() slot.
    // No client record is lost by opening only now: FreeRDP runs PostConnect
    // right after the client's Font List PDU, in the same receive call, and
    // anything the client sent before that was pre-authentication data.
    openControlChannel();
    qCInfo(KRDP) << "Client authenticated; KRDPCTL" << d->controlChannelOpen.load();
    Q_EMIT clientDisplayInfoReceived();
}

bool RdpConnection::onActivate()
{
    return true;
}

bool RdpConnection::onPostConnect()
{
    d->authenticatedPamUid.store(0);
    d->authenticatedUserUid.store(0);
    qCInfo(KRDP) << "New client connected:" << d->peer->hostname << freerdp_peer_os_major_type_string(d->peer) << freerdp_peer_os_minor_type_string(d->peer);

    rdpSettings *settings = d->peer->context->settings;

    if (!freerdp_settings_set_bool(settings, FreeRDP_AutoLogonEnabled, true)) {
        return false;
    }

    const QString username = QString::fromLatin1(freerdp_settings_get_string(settings, FreeRDP_Username));
    const QString password = QString::fromLatin1(freerdp_settings_get_string(settings, FreeRDP_Password));

    bool authenticated = false;
    std::optional<quint32> pamUid, ownerUid;
    if (d->server->usePAMAuthentication()) {
        qCDebug(KRDP) << "Attempting authenticating user with PAM";
        if (d->server->allowAnyPAMUser() || username == KUser().loginName()) {
            pamUid = pamAuthenticate(username, password);
            if (pamUid && (d->server->allowAnyPAMUser() || *pamUid == KUser().userId().nativeId())
                && d->server->acceptsPamIdentity(*pamUid)) {
                qCInfo(KRDP) << "PAM authentication succeeded for user" << username;
                authenticated = true;
                ownerUid = pamUid;
            } else {
                if (pamUid && !d->server->acceptsPamIdentity(*pamUid))
                    qCInfo(KRDP) << "PAM authenticated account is not admitted by broker policy";
                pamUid.reset();
            }
        }
    }

    if (!authenticated) {
        // The broker policy's aliases/verifiers use Unicode text. Decode the
        // actual UTF-8 RDP fields; preserve the separate legacy/PAM path above.
        ownerUid = d->server->mappedCredentialIdentity(
            QString::fromUtf8(freerdp_settings_get_string(settings, FreeRDP_Username)),
            QString::fromUtf8(freerdp_settings_get_string(settings, FreeRDP_Password)));
        if (ownerUid) {
            qCInfo(KRDP) << "Custom credential authenticated for an explicitly configured desktop owner";
            authenticated = true;
        }
    }

    if (!authenticated && d->server->matchesConfiguredUser(username, password)) {
        qCInfo(KRDP) << "User" << username << "authenticated successfully";
        authenticated = true;
    }

    // Devices (RDPSND, AUDIN, RDPECAM) are created by the session loop's
    // reconcilers, and only once a consent exists: a KRDPCTL `device` record
    // or StandardClientMedia, both after this authentication. That is past
    // licensing, so RDPSND's formats PDU can no longer arrive while the client
    // still rejects channel messages; AUDIN and RDPECAM wait for DRDYNVC READY.
    if (!authenticated) {
        // A standard reason every RDP client understands, instead of a bare drop that FreeRDP
        // reports as a logoff: Close() sends Deactivate All, the Set Error Info PDU and the
        // Disconnect Provider Ultimatum before PostConnect fails the state machine.
        qCInfo(KRDP) << "Authentication failed for user" << username << "- telling the client (ERRINFO_SERVER_INSUFFICIENT_PRIVILEGES)";
        close(CloseReason::AuthenticationFailed);
        return false;
    }
    if (pamUid) {
        d->authenticatedPamUid.store(quint64(*pamUid) + 1);
    }
    if (ownerUid) d->authenticatedUserUid.store(quint64(*ownerUid) + 1);
    onAuthenticated();
    return true;
}

bool RdpConnection::onClose()
{
    d->authenticatedPamUid.store(0);
    d->authenticatedUserUid.store(0);
    d->playbackSending = false;
    if (d->rdpsnd) {
        d->rdpsndState.active.store(false);
        if (d->rdpsnd->Close) {
            (void)d->rdpsnd->Close(d->rdpsnd);
        }
        rdpsnd_server_context_free(d->rdpsnd);
        d->rdpsnd = nullptr;
    }
    // AUD-D1: an isolated endpoint restores the host's default sink with
    // blocking pw-metadata calls (up to several seconds). The job owns the
    // endpoint outright, so this connection can be destroyed before it ends;
    // Server::~Server() drains the queue before the process exits.
    PipeWireAudioPlayback::stopAsync(std::move(d->audioPlaybackEndpoint));
    closeCameras();
    for (const auto device : {MediaDevice::Playback, MediaDevice::Microphone, MediaDevice::Camera}) {
        d->slot(device).consent.setEnabled(false);
        d->consentChanged(device);
    }
    retireMicrophone();
    closeDisplayControl();
    {
        std::lock_guard lock(d->controlChannelMutex);
        if (d->controlChannel) {
            WTSVirtualChannelClose(d->controlChannel);
            d->controlChannel = nullptr;
        }
    }
    d->clipboard->close();
    d->videoStream->close();
    setState(State::Closed);
    return true;
}

bool RdpConnection::reconcileDevices()
{
    const auto vcm = reinterpret_cast<PeerContext *>(d->peer->context)->virtualChannelManager;
    if (d->standardConsentPending.exchange(false)) {
        // StandardClientMedia: the client's own negotiation is the consent.
        // Playback only if it joined RDPSND; the microphone and camera are
        // offered on DRDYNVC, where the client's accept of the AUDIN /
        // RDPECAM channel is its consent (and a refusal just ends it).
        const bool rdpsnd = WTSVirtualChannelManagerIsChannelJoined(vcm, RDPSND_CHANNEL_NAME);
        const bool dynamic = WTSVirtualChannelManagerIsChannelJoined(vcm, DRDYNVC_SVC_CHANNEL_NAME);
        qCInfo(KRDP) << "StandardClientMedia: playback" << rdpsnd << "microphone and camera" << dynamic;
        if (rdpsnd) {
            setDeviceEnabled(MediaDevice::Playback, true);
        }
        if (dynamic) {
            setDeviceEnabled(MediaDevice::Microphone, true);
            setDeviceEnabled(MediaDevice::Camera, true);
        }
    }
    if (!reconcilePlayback() || !reconcileMicrophone() || !reconcileCamera()) {
        return false;
    }
    // Requests for a generation that was already settled (an `off` of a device
    // that is off) are answered here.
    for (const auto device : {MediaDevice::Playback, MediaDevice::Microphone, MediaDevice::Camera}) {
        auto &slot = d->slot(device);
        bool pending = false;
        {
            std::lock_guard lock(slot.requestsMutex);
            pending = std::any_of(slot.requests.cbegin(), slot.requests.cend(), [&slot](const auto &request) {
                return request.first <= slot.settled;
            });
        }
        if (pending) {
            publishDevice(device, slot.status);
        }
    }
    return true;
}

bool RdpConnection::reconcilePlayback()
{
    using State = DeviceStatus::State;
    auto &slot = d->slot(MediaDevice::Playback);
    const auto vcm = reinterpret_cast<PeerContext *>(d->peer->context)->virtualChannelManager;
    const auto consent = slot.consent.snapshot();
    const auto now = std::chrono::steady_clock::now();

    if (slot.applied != consent.generation) {
        slot.applied = consent.generation;
        // End the previous period: stop sending first, then stop the capture
        // off this thread (AUD-D1: pw-metadata calls must never block it).
        d->playbackSending = false;
        PipeWireAudioPlayback::stopAsync(std::move(d->audioPlaybackEndpoint));
        if (!consent.enabled) {
            // RDPSND is a static channel and stays open. The standard off is
            // "no more Wave PDUs"; SNDC_CLOSE on top only goes to our own
            // client (DEVICES-DESIGN.md §7 risk 1: untested against mstsc).
            if (d->rdpsnd && d->rdpsnd->Close && d->rdpsndState.active.load() && !d->rdpsndClosed && isOwnClient()) {
                if (d->rdpsnd->Close(d->rdpsnd) == CHANNEL_RC_OK) {
                    d->rdpsndClosed = true;
                    qCInfo(KRDP) << "RDPSND: sent SNDC_CLOSE to the own client";
                }
            }
            publishDevice(MediaDevice::Playback, {}, consent.generation);
            return true;
        }
        const auto fail = [&](const QString &code, const QString &message) {
            PipeWireAudioPlayback::stopAsync(std::move(d->audioPlaybackEndpoint));
            publishDevice(MediaDevice::Playback, {State::Error, false, code, message}, consent.generation);
        };
        if (!WTSVirtualChannelManagerIsChannelJoined(vcm, RDPSND_CHANNEL_NAME)) {
            fail(DeviceControl::Unavailable, QStringLiteral("the client did not join the audio playback channel (rdpsnd)"));
            return true;
        }
        if (!d->rdpsnd) {
            d->rdpsnd = rdpsnd_server_context_new(vcm);
            if (!d->rdpsnd) {
                fail(DeviceControl::Unavailable, QStringLiteral("could not create the RDPSND server context"));
                return true;
            }
            d->rdpsnd->rdpcontext = d->peer->context;
            d->rdpsnd->data = &d->rdpsndState;
            d->rdpsnd->Activated = rdpsndActivated;
            d->rdpsnd->num_server_formats = server_rdpsnd_get_formats(&d->rdpsnd->server_formats);
            if (d->rdpsnd->num_server_formats == 0 || !d->rdpsnd->Initialize) {
                rdpsnd_server_context_free(d->rdpsnd);
                d->rdpsnd = nullptr;
                fail(DeviceControl::Unavailable, QStringLiteral("RDPSND has no server formats"));
                return true;
            }
            d->rdpsnd->src_format = &d->rdpsnd->server_formats[0];
            if (d->rdpsnd->Initialize(d->rdpsnd, TRUE) != CHANNEL_RC_OK) {
                rdpsnd_server_context_free(d->rdpsnd);
                d->rdpsnd = nullptr;
                fail(DeviceControl::Unavailable, QStringLiteral("could not initialize RDPSND"));
                return true;
            }
            qCInfo(KRDP) << "RDPSND channel initialized";
        }
        if (d->externalAudioPlayback.load()) {
            qCInfo(KRDP) << "RDPSND using PCM supplied by the console capture worker";
        } else {
            d->audioPlaybackEndpoint = std::make_unique<PipeWireAudioPlayback>();
            const bool isolated = slot.silenceHost.load();
            // An isolated sink is the only safe way to silence the host: muting
            // the physical sink can mute its monitor too. The sink becomes the
            // default only for this period and stop() restores it.
            const bool started = isolated ? d->audioPlaybackEndpoint->startIsolated(QString::number(reinterpret_cast<quintptr>(this), 16))
                                          : d->audioPlaybackEndpoint->start(QStringLiteral("@DEFAULT_AUDIO_SINK@"));
            if (!started) {
                fail(DeviceControl::Unavailable, QStringLiteral("could not capture the host's audio from PipeWire"));
                return true;
            }
            if (isolated) {
                qCInfo(KRDP) << "RDPSND capturing the session-private PipeWire sink; new audio will not reach the host speakers";
            }
        }
        slot.deadline = now + DeviceReadyDeadline;
        publishDevice(MediaDevice::Playback, {State::Starting, false, {}, {}});
    }

    if (consent.enabled && slot.status.state == State::Starting) {
        if (d->rdpsndState.active.load()) {
            if (std::exchange(d->rdpsndClosed, false) && d->rdpsnd->SelectFormat) {
                // SNDC_CLOSE made FreeRDP forget the format the client's
                // Activated exchange chose; the client itself still has it.
                (void)d->rdpsnd->SelectFormat(d->rdpsnd, UINT16(d->rdpsndState.clientFormat.load()));
            }
            d->playbackSending = true;
            publishDevice(MediaDevice::Playback, {State::On, false, {}, {}}, consent.generation);
        } else if (now >= slot.deadline) {
            PipeWireAudioPlayback::stopAsync(std::move(d->audioPlaybackEndpoint));
            publishDevice(MediaDevice::Playback,
                          {State::Error, false, DeviceControl::Timeout, QStringLiteral("the client did not activate audio playback within 5 s")},
                          consent.generation);
        }
    }
    return true;
}

bool RdpConnection::retireMicrophone()
{
    if (d->audin) {
        // Close joins FreeRDP's audio reader. Never destroy its userdata or
        // PipeWire endpoint while a callback may still be using either.
        if (d->audin->Close && !d->audin->Close(d->audin)) {
            qCWarning(KRDP) << "Could not close the AUDIN channel";
            return false;
        }
        audin_server_context_free(d->audin);
        d->audin = nullptr;
    }
    d->microphoneDelivery.reset();
    d->microphoneEndpoint.reset();
    return true;
}

bool RdpConnection::reconcileMicrophone()
{
    using State = DeviceStatus::State;
    auto &slot = d->slot(MediaDevice::Microphone);
    const auto vcm = reinterpret_cast<PeerContext *>(d->peer->context)->virtualChannelManager;
    const auto consent = slot.consent.snapshot();
    const auto now = std::chrono::steady_clock::now();

    if (d->audin && (!consent.enabled || d->microphoneDelivery.generation != consent.generation)) {
        if (!retireMicrophone()) {
            return false;
        }
        qCInfo(KRDP) << "AUDIN consent generation retired";
    }
    if (!consent.enabled) {
        slot.applied = consent.generation;
        if (slot.settled != consent.generation || slot.status.state != State::Off) {
            publishDevice(MediaDevice::Microphone, {}, consent.generation);
        }
        return true;
    }
    // Closes the channel and reports \a code; the consent stays, but nothing
    // reopens it before a new request (a new generation).
    const auto fail = [&](const QString &code, const QString &message) {
        if (!retireMicrophone()) {
            return false;
        }
        publishDevice(MediaDevice::Microphone, {State::Error, false, code, message}, consent.generation);
        return true;
    };

    if (slot.applied != consent.generation) {
        slot.applied = consent.generation;
        if (!WTSVirtualChannelManagerIsChannelJoined(vcm, DRDYNVC_SVC_CHANNEL_NAME)) {
            return fail(DeviceControl::Unavailable, QStringLiteral("the client has no dynamic virtual channels"));
        }
        d->audin = audin_server_context_new(vcm);
        if (!d->audin) {
            return fail(DeviceControl::Unavailable, QStringLiteral("could not create the AUDIN server context"));
        }
        d->audin->rdpcontext = d->peer->context;
        d->microphoneDelivery.reset(&slot.consent, consent.generation, d->externalMicrophone ? &d->microphonePcm : nullptr);
        d->audin->userdata = &d->microphoneDelivery;
        d->audin->Data = audinData;
        d->audin->OpenReply = audinOpenReply;
        if (!audin_server_set_formats(d->audin, 1, &RemoteMicrophoneFormat)) {
            return fail(DeviceControl::Unavailable, QStringLiteral("could not set the AUDIN formats"));
        }
        // Until DRDYNVC is READY there is no Open to time; the 5 s deadline
        // restarts at the Open.
        slot.deadline = now + 2 * DeviceReadyDeadline;
        publishDevice(MediaDevice::Microphone, {State::Starting, false, {}, {}});
    }
    if (!d->audin) {
        return true; // settled as an error; waiting for a new request
    }
    if (WTSVirtualChannelManagerGetDrdynvcState(vcm) == DRDYNVC_STATE_READY && d->audin->IsOpen && !d->audin->IsOpen(d->audin)) {
        if (!d->audin->Open || !d->audin->Open(d->audin)) {
            return fail(DeviceControl::Unavailable, QStringLiteral("could not open the AUDIN channel"));
        }
        slot.deadline = now + DeviceReadyDeadline;
        qCInfo(KRDP) << "AUDIN channel opened";
    }

    if (slot.status.state == State::Starting) {
        const auto result = d->microphoneDelivery.openResult.load();
        if (result > 0) {
            return fail(DeviceControl::Declined, QStringLiteral("the client could not open its microphone (result 0x%1)").arg(quint64(result), 8, 16, QLatin1Char('0')));
        }
        if (result == 0) {
            if (!d->externalMicrophone && !d->microphoneEndpoint) {
                // The node only appears once the client accepted: a client
                // that refuses the channel never shows a dead microphone.
                auto endpoint = std::make_unique<PipeWireMicrophone>();
                if (!endpoint->start(QString::number(reinterpret_cast<quintptr>(this), 16))) {
                    return fail(DeviceControl::Unavailable, QStringLiteral("could not create the PipeWire remote microphone"));
                }
                d->microphoneEndpoint = std::move(endpoint);
                d->microphoneDelivery.endpoint.store(d->microphoneEndpoint.get());
            }
            const auto state = d->microphoneEndpoint ? d->microphoneEndpoint->state() : PipeWireMicrophone::State::Ready;
            if (state == PipeWireMicrophone::State::Ready) {
                publishDevice(MediaDevice::Microphone, {State::On, false, {}, {}}, consent.generation);
                slot.nextInUsePoll = now;
            } else if (state == PipeWireMicrophone::State::Failed) {
                return fail(DeviceControl::Unavailable, QStringLiteral("the PipeWire remote microphone failed"));
            }
        }
        if (slot.status.state == State::Starting && now >= slot.deadline) {
            return fail(DeviceControl::Timeout, QStringLiteral("the client did not open the microphone within 5 s"));
        }
        return true;
    }

    if (slot.status.state == State::On && d->microphoneEndpoint) {
        if (d->microphoneEndpoint->state() == PipeWireMicrophone::State::Failed) {
            return fail(DeviceControl::Unavailable, QStringLiteral("the PipeWire remote microphone failed"));
        }
        if (now >= slot.nextInUsePoll) {
            slot.nextInUsePoll = now + InUsePollInterval;
            const bool inUse = d->microphoneEndpoint->consumerActive();
            if (inUse != slot.status.inUse) {
                auto status = slot.status;
                status.inUse = inUse;
                publishDevice(MediaDevice::Microphone, status);
            }
        }
    }
    return true;
}

void RdpConnection::closeCameras()
{
    if (d->cameraEnumerator) {
        // Joins the enumerator thread first: nothing adds or removes a camera after this.
        if (d->cameraEnumerator->Close) {
            (void)d->cameraEnumerator->Close(d->cameraEnumerator);
        }
        cam_dev_enum_server_context_free(d->cameraEnumerator);
        d->cameraEnumerator = nullptr;
    }
    // Destroyed outside the set's lock: each ~RemoteCamera closes its device
    // channel and joins its thread before its PipeWire node is removed.
    auto cameras = d->remoteCameras.cameras.takeAll();
    cameras.clear();
}

bool RdpConnection::reconcileCamera()
{
    using State = DeviceStatus::State;
    auto &slot = d->slot(MediaDevice::Camera);
    const auto vcm = reinterpret_cast<PeerContext *>(d->peer->context)->virtualChannelManager;
    const auto consent = slot.consent.snapshot();
    const auto now = std::chrono::steady_clock::now();

    if (d->cameraEnumerator && (!consent.enabled || slot.applied != consent.generation)) {
        // Off, or `reselect` (a new generation): close; a reopen makes the
        // client enumerate its cameras again.
        closeCameras();
    }
    if (!consent.enabled) {
        slot.applied = consent.generation;
        if (slot.settled != consent.generation || slot.status.state != State::Off) {
            publishDevice(MediaDevice::Camera, {}, consent.generation);
        }
        return true;
    }
    const auto fail = [&](const QString &code, const QString &message) {
        closeCameras();
        publishDevice(MediaDevice::Camera, {State::Error, false, code, message}, consent.generation);
    };

    if (slot.applied != consent.generation) {
        if (!d->remoteCameras.external) {
            const auto reason = CameraAvailability::reason(d->server->cameraLoopbackDevice());
            if (!reason.isEmpty()) {
                slot.applied = consent.generation;
                fail(DeviceControl::Unavailable, reason);
                return true;
            }
        }
        if (!WTSVirtualChannelManagerIsChannelJoined(vcm, DRDYNVC_SVC_CHANNEL_NAME)) {
            slot.applied = consent.generation;
            fail(DeviceControl::Unavailable, QStringLiteral("the client has no dynamic virtual channels"));
            return true;
        }
        if (WTSVirtualChannelManagerGetDrdynvcState(vcm) != DRDYNVC_STATE_READY) {
            return true; // not yet
        }
        slot.applied = consent.generation;
        slot.cameraRemoved = false;
        d->remoteCameras.versionSeen.store(false);
        d->cameraEnumerator = cam_dev_enum_server_context_new(vcm);
        if (d->cameraEnumerator) {
            d->cameraEnumerator->rdpcontext = d->peer->context;
            d->remoteCameras.loopbackDevice = d->server->cameraLoopbackDevice();
            d->cameraEnumerator->userdata = &d->remoteCameras;
            d->cameraEnumerator->SelectVersionRequest = cameraSelectVersion;
            d->cameraEnumerator->DeviceAddedNotification = cameraAdded;
            d->cameraEnumerator->DeviceRemovedNotification = cameraRemoved;
        }
        if (!d->cameraEnumerator || d->cameraEnumerator->Initialize(d->cameraEnumerator, FALSE) != CHANNEL_RC_OK
            || d->cameraEnumerator->Open(d->cameraEnumerator) != CHANNEL_RC_OK) {
            if (d->cameraEnumerator) {
                cam_dev_enum_server_context_free(d->cameraEnumerator);
                d->cameraEnumerator = nullptr;
            }
            fail(DeviceControl::Unavailable, QStringLiteral("could not open the camera channel (RDPECAM)"));
            return true;
        }
        qCInfo(KRDP) << "RDPECAM enumerator opened";
        slot.deadline = now + DeviceReadyDeadline;
        publishDevice(MediaDevice::Camera, {State::Starting, false, {}, {}});
        return true;
    }
    if (!d->cameraEnumerator) {
        return true;
    }

    size_t ready = 0;
    QString cameraError;
    d->remoteCameras.cameras.forEach([&ready, &cameraError](RemoteCamera *camera) {
        if (!camera->external && camera->endpoint && !camera->endpoint->error().isEmpty()) {
            cameraError = camera->endpoint->error();
            return false;
        }
        if (camera->endpointStarted.load() && (camera->external ? camera->connection->d->remoteCameras.workerReady.load() : camera->endpoint->ready())) {
            ++ready;
        }
        return true;
    });
    if (!cameraError.isEmpty()) {
        fail(DeviceControl::Unavailable, cameraError);
        return true;
    }
    switch (slot.status.state) {
    case State::Starting:
        if (ready > 0) {
            publishDevice(MediaDevice::Camera, {State::On, false, {}, {}}, consent.generation);
            slot.nextInUsePoll = now;
        } else if (now >= slot.deadline) {
            if (d->remoteCameras.versionSeen.load()) {
                fail(DeviceControl::Unavailable, QStringLiteral("the client offered no camera"));
            } else {
                fail(DeviceControl::Timeout, QStringLiteral("the client did not open the camera channel within 5 s"));
            }
        }
        return true;
    case State::On:
        if (ready == 0) {
            // DeviceRemovedNotification took the last camera. The enumerator
            // stays open: a camera the client adds again turns it back on.
            slot.cameraRemoved = true;
            publishDevice(MediaDevice::Camera, {State::Error, false, DeviceControl::Unavailable, QStringLiteral("the client removed its camera")});
            return true;
        }
        if (now >= slot.nextInUsePoll) {
            slot.nextInUsePoll = now + 2 * InUsePollInterval; // the loopback check walks /proc
            bool inUse = false;
            d->remoteCameras.cameras.forEach([&inUse](RemoteCamera *camera) {
                inUse = camera->endpointStarted.load() && (camera->external ? camera->connection->d->remoteCameras.workerInUse.load() : camera->endpoint->consumerActive());
                return !inUse;
            });
            if (inUse != slot.status.inUse) {
                auto status = slot.status;
                status.inUse = inUse;
                publishDevice(MediaDevice::Camera, status);
            }
        }
        return true;
    case State::Error:
        if (slot.cameraRemoved && ready > 0) {
            slot.cameraRemoved = false;
            publishDevice(MediaDevice::Camera, {State::On, false, {}, {}});
            slot.nextInUsePoll = now;
        }
        return true;
    case State::Off:
        return true;
    }
    return true;
}

bool RdpConnection::onSuppressOutput(uint8_t allow)
{
    if (allow) {
        d->videoStream->setEnabled(true);
    } else {
        d->videoStream->setEnabled(false);
    }

    return true;
}

void RdpConnection::onRefreshRect(int areas)
{
    qCDebug(KRDP) << "Refresh Rect for" << areas << "area(s)";
    d->videoStream->requestRefresh();
}

freerdp_peer *RdpConnection::rdpPeer() const
{
    return d->peer;
}

rdpContext *RdpConnection::rdpPeerContext() const
{
    return d->peer->context;
}
}

#include "moc_RdpConnection.cpp"
