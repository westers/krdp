// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// This file is roughly based on grd-session-rdp.c from gnome-remote-desktop,
// which is:
//
// SPDX-FileCopyrightText: 2020-2023 Pascal Nowack
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "RdpConnection.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QThread>

#include <freerdp/channels/wtsvc.h>
#include <freerdp/freerdp.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/audin.h>
#include <freerdp/server/rdpsnd.h>
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
#include "Server.h"
#include "VideoStream.h"

#include <KUser>

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
    const QDir driDir(QStringLiteral("/dev/dri"));
    const auto entries = driDir.entryList({QStringLiteral("renderD*")}, QDir::System | QDir::Readable, QDir::Name);
    nodes.reserve(entries.size());

    for (const auto &entry : entries) {
        const auto vendorPath = QStringLiteral("/sys/class/drm/%1/device/vendor").arg(entry);
        const auto vendorId = readTrimmedFile(vendorPath);
        if (vendorId.isEmpty()) {
            continue;
        }

        nodes.push_back(RenderNodeInfo{
            .renderNode = QFile::encodeName(driDir.absoluteFilePath(entry)),
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

UINT audinData(audin_server_context *audin, const SNDIN_DATA *data)
{
    // The PCM is deliberately not discarded silently: the next OPT-050 slice
    // connects this callback to the per-session PipeWire virtual microphone.
    // Keeping channel negotiation here first gives the client and server a
    // standards-compliant lifecycle to exercise independently of media I/O.
    if (!data || !data->Data) {
        return ERROR_INVALID_DATA;
    }
    auto *endpoint = static_cast<PipeWireMicrophone *>(audin->userdata);
    if (endpoint) {
        endpoint->write(QByteArray(reinterpret_cast<const char *>(Stream_Buffer(data->Data)), int(Stream_Length(data->Data))));
    }
    return CHANNEL_RC_OK;
}

void rdpsndActivated(RdpsndServerContext *rdpsnd)
{
    for (size_t client = 0; client < rdpsnd->num_client_formats; ++client) {
        for (size_t server = 0; server < rdpsnd->num_server_formats; ++server) {
            if (!audio_format_compatible(&rdpsnd->server_formats[server], &rdpsnd->client_formats[client])) {
                continue;
            }
            if (rdpsnd->SelectFormat(rdpsnd, UINT16(client)) == CHANNEL_RC_OK) {
                if (auto *active = static_cast<std::atomic_bool *>(rdpsnd->data)) {
                    active->store(true);
                }
                qCInfo(KRDP) << "RDPSND selected client format" << client;
                return;
            }
        }
    }
    qCWarning(KRDP) << "RDPSND client offered no compatible format";
}

UINT cameraSelectVersion(CamDevEnumServerContext *context, const CAM_SELECT_VERSION_REQUEST *request)
{
    CAM_SELECT_VERSION_RESPONSE response{};
    response.Header = request->Header;
    response.Header.MessageId = CAM_MSG_ID_SelectVersionResponse;
    return context->SelectVersionResponse(context, &response);
}

struct RemoteCamera {
    CameraDeviceServerContext *context = nullptr;
    bool activated = false;
    bool receivedSample = false;
    CAM_MEDIA_TYPE_DESCRIPTION format{};
    QString loopbackDevice;
    bool streamStarted = false;
    std::unique_ptr<PipeWireCamera> endpoint;
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
    qCInfo(KRDP) << "RDPECAM success response" << (camera && camera->activated ? "stream started" : "device activated");
    if (!camera->activated) {
        camera->activated = true;
        CAM_STREAM_LIST_REQUEST request{};
        return context->StreamListRequest(context, &request);
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
    camera->endpoint = std::make_unique<PipeWireCamera>();
    const uint32_t fps = camera->format.FrameRateDenominator ? camera->format.FrameRateNumerator / camera->format.FrameRateDenominator : 30;
    if (!camera->endpoint->start(QString::number(reinterpret_cast<quintptr>(camera)), camera->format.Width, camera->format.Height, fps, camera->loopbackDevice)) {
        qCWarning(KRDP) << "Failed to create PipeWire remote camera source";
        camera->endpoint.reset();
        return ERROR_INTERNAL_ERROR;
    }
    qCInfo(KRDP) << "RDPECAM virtual camera available; waiting for a local consumer" << camera->format.Width << 'x' << camera->format.Height
                 << "format" << camera->format.Format;
    return CHANNEL_RC_OK;
}

bool startCameraIfRequested(RemoteCamera *camera)
{
    if (!camera || !camera->endpoint || camera->streamStarted || !camera->endpoint->captureRequested()) {
        return true;
    }
    CAM_START_STREAMS_REQUEST request{};
    request.N_Infos = 1;
    request.StartStreamsInfo[0].StreamIndex = 0;
    request.StartStreamsInfo[0].MediaTypeDescription = camera->format;
    const UINT status = camera->context->StartStreamsRequest(camera->context, &request);
    if (status != CHANNEL_RC_OK) {
        qCWarning(KRDP) << "RDPECAM could not start camera on local demand" << status;
        return false;
    }
    camera->streamStarted = true;
    qCInfo(KRDP) << "RDPECAM starting camera for a local PipeWire/V4L2 consumer";
    return true;
}

UINT cameraSample(CameraDeviceServerContext *context, const CAM_SAMPLE_RESPONSE *response)
{
    auto *camera = static_cast<RemoteCamera *>(context->userdata);
    if (!camera || !camera->endpoint || response->StreamIndex != 0 || !response->Sample || !response->SampleSize) return ERROR_INVALID_DATA;
    if (!camera->receivedSample) {
        camera->receivedSample = true;
        qCInfo(KRDP) << "RDPECAM receiving camera samples (first frame bytes)" << response->SampleSize;
    }
    camera->endpoint->writeMjpeg(QByteArray(reinterpret_cast<const char *>(response->Sample), response->SampleSize));
    CAM_SAMPLE_REQUEST request{};
    request.StreamIndex = 0;
    const UINT status = context->SampleRequest(context, &request);
    if (status != CHANNEL_RC_OK) qCWarning(KRDP) << "RDPECAM follow-up sample request failed" << status;
    return status;
}

struct RemoteCameraCollection {
    std::vector<std::unique_ptr<RemoteCamera>> cameras;
    QString loopbackDevice;
};

UINT cameraAdded(CamDevEnumServerContext *enumerator, const CAM_DEVICE_ADDED_NOTIFICATION *device)
{
    qCInfo(KRDP) << "RDPECAM client camera available:" << QString::fromUtf16(reinterpret_cast<const char16_t *>(device->DeviceName)) << device->VirtualChannelName;
    auto *collection = static_cast<RemoteCameraCollection *>(enumerator->userdata);
    if (!collection || !device->VirtualChannelName) {
        return ERROR_INVALID_DATA;
    }
    auto camera = std::make_unique<RemoteCamera>();
    camera->loopbackDevice = collection->loopbackDevice;
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
    collection->cameras.push_back(std::move(camera));
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

    if (qEnvironmentVariableIsSet("KRDP_FORCE_VAAPI_DRIVER")) {
        const auto forcedDriver = qgetenv("KRDP_FORCE_VAAPI_DRIVER");
        if (!forcedDriver.isEmpty()) {
            qputenv("LIBVA_DRIVER_NAME", forcedDriver);
            g_autoAppliedVaapiDriver = true;
            qCInfo(KRDP) << "Using forced VAAPI driver from KRDP_FORCE_VAAPI_DRIVER:" << forcedDriver;
        }
        return;
    }
    if (qEnvironmentVariableIntValue("KRDP_AUTO_VAAPI_DRIVER") == 0 && qEnvironmentVariableIsSet("KRDP_AUTO_VAAPI_DRIVER")) {
        qCDebug(KRDP) << "Skipping automatic VAAPI driver selection due to KRDP_AUTO_VAAPI_DRIVER=0";
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

static int pamAuthenticate(const QString &user, const QString &password)
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
        return -1;
    }

    pam_status = pam_authenticate(info.handle, 0);

    if (pam_status != PAM_SUCCESS) {
        qWarning() << "pam_authenticate failure:" << pam_strerror(info.handle, pam_status);
        return -1;
    }

    pam_status = pam_acct_mgmt(info.handle, 0);

    if (pam_status != PAM_SUCCESS) {
        qWarning() << "pam_acct_mgmt failure:" << pam_strerror(info.handle, pam_status);
        return -1;
    }

    return 1;
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

class KRDP_NO_EXPORT RdpConnection::Private
{
public:
    Server *server = nullptr;

    State state = State::Initial;

    qintptr socketHandle;

    std::unique_ptr<InputHandler> inputHandler;
    std::unique_ptr<VideoStream> videoStream;
    std::unique_ptr<Cursor> cursor;
    std::unique_ptr<NetworkDetection> networkDetection;
    std::unique_ptr<Clipboard> clipboard;

    RdpsndServerContext *rdpsnd = nullptr;
    audin_server_context *audin = nullptr;
    std::unique_ptr<PipeWireMicrophone> microphoneEndpoint;
    std::unique_ptr<PipeWireAudioPlayback> audioPlaybackEndpoint;
    std::atomic<bool> remoteAudioPlayback = false;
    std::atomic<bool> microphone = false;
    std::atomic<bool> camera = false;
    std::atomic_bool rdpsndActive = false;
    CamDevEnumServerContext *cameraEnumerator = nullptr;
    RemoteCameraCollection remoteCameras;

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
};

RdpConnection::RdpConnection(Server *server, qintptr socketHandle)
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
    d->server = server;
    d->socketHandle = socketHandle;

    d->inputHandler = std::make_unique<InputHandler>(this);
    d->videoStream = std::make_unique<VideoStream>(this);
    connect(d->videoStream.get(), &VideoStream::closed, this, [this]() {
        if (d->state == State::Running || d->state == State::Streaming) {
            qCDebug(KRDP) << "Video stream closed, closing session";
            d->peer->Close(d->peer);
        }
    });
    d->cursor = std::make_unique<Cursor>(this);
    d->networkDetection = std::make_unique<NetworkDetection>(this);
    d->clipboard = std::make_unique<Clipboard>(this);

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
        freerdp_peer_free(d->peer);
    }
}

RdpConnection::State RdpConnection::state() const
{
    return d->state;
}

void RdpConnection::setState(KRdp::RdpConnection::State newState)
{
    if (newState == d->state) {
        return;
    }

    d->state = newState;
    Q_EMIT stateChanged(newState);
}

void RdpConnection::close(RdpConnection::CloseReason reason)
{
    switch (reason) {
    case CloseReason::VideoInitFailed:
        freerdp_set_error_info(d->peer->context->rdp, ERRINFO_GRAPHICS_SUBSYSTEM_FAILED);
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
    }
}

void RdpConnection::setMediaPolicy(bool remoteAudioPlayback, bool microphone, bool camera)
{
    d->remoteAudioPlayback.store(remoteAudioPlayback);
    d->microphone.store(microphone);
    d->camera.store(camera);
    qCInfo(KRDP) << "Conferencing media policy: playback" << remoteAudioPlayback << "microphone" << microphone << "camera" << camera;
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
    setState(State::Starting);

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
        qCWarning(KRDP) << "Could not read certificate file" << d->server->tlsCertificate().string();
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

    d->inputHandler->initialize(d->peer->context->input);
    context->inputHandler = d->inputHandler.get();

    context->networkDetection = d->networkDetection.get();
    d->networkDetection->initialize();

    if (!d->peer->Initialize(d->peer)) {
        qCWarning(KRDP) << "Unable to initialize peer";
        return;
    }

    qCDebug(KRDP) << "Session setup completed, start processing...";

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

        // Initialize any dynamic channels once the dynamic channel channel is setup.
        if (d->peer->connected && WTSVirtualChannelManagerIsChannelJoined(context->virtualChannelManager, DRDYNVC_SVC_CHANNEL_NAME)) {
            auto state = WTSVirtualChannelManagerGetDrdynvcState(context->virtualChannelManager);
            // Dynamic channels can only be set up properly once the dynamic channel channel is properly setup.
            if (state == DRDYNVC_STATE_READY) {
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
            qCDebug(KRDP) << "Unable to check Virtual Channel Manager file descriptor, closing connection";
            break;
        }

        if (d->peer->connected && WTSVirtualChannelManagerIsChannelJoined(context->virtualChannelManager, CLIPRDR_SVC_CHANNEL_NAME)) {
            if (!d->clipboard->initialize()) {
                break;
            }
        }

        if (!initializeAudioChannels()) {
            break;
        }

        for (const auto &camera : d->remoteCameras.cameras) {
            if (!startCameraIfRequested(camera.get())) {
                break;
            }
        }

        if (d->rdpsnd && d->rdpsndActive.load() && d->audioPlaybackEndpoint) {
            const QByteArray pcm = d->audioPlaybackEndpoint->take();
            if (!pcm.isEmpty() && d->rdpsnd->SendSamples) {
                const auto frames = size_t(pcm.size() / d->rdpsnd->src_format->nBlockAlign);
                if (frames > 0) {
                    d->rdpsnd->SendSamples(d->rdpsnd, pcm.constData(), frames, UINT16(GetTickCount64() & 0xffff));
                }
            }
        }

        // KRDPCTL (OPT-044): opened as soon as the join shows, not once
        // connected (see openControlChannel()); the client's records arrive
        // through CheckFileDescriptor() above, queued on the channel.
        openControlChannel();
        if (!readControlChannel()) {
            break;
        }

        d->networkDetection->update();
    }

    qCDebug(KRDP) << "Closing session";
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
    // The MCS channel join is complete by the time FreeRDP asks for the
    // capabilities, so hasControlChannel() is exact for the slot below.
    openControlChannel();
    qCInfo(KRDP) << "Client display: desktop" << info.desktopSize << "monitors" << info.monitors.size()
                 << "monitorLayoutPdu" << freerdp_settings_get_bool(settings, FreeRDP_SupportMonitorLayoutPdu)
                 << "KRDPCTL" << d->controlChannelOpen.load();
    Q_EMIT clientDisplayInfoReceived();

    return true;
}

bool RdpConnection::onActivate()
{
    return true;
}

bool RdpConnection::onPostConnect()
{
    qCInfo(KRDP) << "New client connected:" << d->peer->hostname << freerdp_peer_os_major_type_string(d->peer) << freerdp_peer_os_minor_type_string(d->peer);

    rdpSettings *settings = d->peer->context->settings;

    if (!freerdp_settings_set_bool(settings, FreeRDP_AutoLogonEnabled, true)) {
        return false;
    }

    const QString username = QString::fromLatin1(freerdp_settings_get_string(settings, FreeRDP_Username));
    const QString password = QString::fromLatin1(freerdp_settings_get_string(settings, FreeRDP_Password));

    bool authenticated = false;
    if (d->server->usePAMAuthentication()) {
        qCDebug(KRDP) << "Attempting authenticating user with PAM";
        if (username == KUser().loginName() && pamAuthenticate(username, password) >= 0) {
            qCDebug(KRDP) << "PAM authentication succeeded for user" << username;
            authenticated = true;
        }
    }

    if (!authenticated) {
        const auto users = d->server->users();
        for (auto user : users) {
            if (user.password.isEmpty()) {
                return false;
            }
            if (user.name == username && user.password == password) {
                qCDebug(KRDP) << "User" << username << "authenticated successfully";
                authenticated = true;
                break;
            }
        }
    }

    // Static channels must be initialized from PostConnect. Delaying RDPSND
    // until the run loop can put its formats PDU on the wire while a client is
    // still in licensing, which FreeRDP correctly rejects as an unexpected
    // channel message. AUDIN is created here too, then opened from the loop
    // only once DRDYNVC reaches READY.
    return authenticated && initializeAudioChannels();
}

bool RdpConnection::onClose()
{
    if (d->rdpsnd) {
        d->rdpsndActive.store(false);
        if (d->rdpsnd->Close) {
            d->rdpsnd->Close(d->rdpsnd);
        }
        rdpsnd_server_context_free(d->rdpsnd);
        d->rdpsnd = nullptr;
    }
    d->audioPlaybackEndpoint.reset();
    if (d->cameraEnumerator) {
        d->cameraEnumerator->Close(d->cameraEnumerator);
        cam_dev_enum_server_context_free(d->cameraEnumerator);
        d->cameraEnumerator = nullptr;
    }
    d->remoteCameras.cameras.clear();
    if (d->audin) {
        if (d->audin->IsOpen && d->audin->IsOpen(d->audin) && d->audin->Close) {
            d->audin->Close(d->audin);
        }
        audin_server_context_free(d->audin);
        d->audin = nullptr;
    }
    d->microphoneEndpoint.reset();
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

bool RdpConnection::initializeAudioChannels()
{
    auto context = reinterpret_cast<PeerContext *>(d->peer->context);
    const auto vcm = context->virtualChannelManager;

    if (!d->audin && d->microphone.load()) {
        d->microphoneEndpoint = std::make_unique<PipeWireMicrophone>();
        if (!d->microphoneEndpoint->start(QString::number(reinterpret_cast<quintptr>(this), 16))) {
            qCWarning(KRDP) << "Could not create PipeWire remote microphone";
            d->microphoneEndpoint.reset();
            return false;
        }
        d->audin = audin_server_context_new(vcm);
        if (!d->audin) {
            qCWarning(KRDP) << "Could not create AUDIN server context";
            return false;
        }
        d->audin->rdpcontext = d->peer->context;
        d->audin->userdata = d->microphoneEndpoint.get();
        d->audin->Data = audinData;
        if (!audin_server_set_formats(d->audin, 1, &RemoteMicrophoneFormat)) {
            qCWarning(KRDP) << "Could not set AUDIN formats";
            return false;
        }
    }

    if (d->audin && WTSVirtualChannelManagerIsChannelJoined(vcm, DRDYNVC_SVC_CHANNEL_NAME)
        && WTSVirtualChannelManagerGetDrdynvcState(vcm) == DRDYNVC_STATE_READY
        && d->audin->IsOpen && !d->audin->IsOpen(d->audin)) {
        if (!d->audin->Open || !d->audin->Open(d->audin)) {
            qCWarning(KRDP) << "Could not open AUDIN";
            return false;
        }
        qCInfo(KRDP) << "AUDIN channel opened";
    }

    if (d->remoteAudioPlayback.load() && WTSVirtualChannelManagerIsChannelJoined(vcm, RDPSND_CHANNEL_NAME) && !d->rdpsnd) {
        d->rdpsnd = rdpsnd_server_context_new(vcm);
        if (!d->rdpsnd) {
            qCWarning(KRDP) << "Could not create RDPSND server context";
            return false;
        }
        d->rdpsnd->rdpcontext = d->peer->context;
        d->rdpsnd->data = &d->rdpsndActive;
        d->rdpsnd->Activated = rdpsndActivated;
        d->rdpsnd->num_server_formats = server_rdpsnd_get_formats(&d->rdpsnd->server_formats);
        if (d->rdpsnd->num_server_formats == 0) {
            qCWarning(KRDP) << "RDPSND has no server formats";
            return false;
        }
        d->rdpsnd->src_format = &d->rdpsnd->server_formats[0];
        if (!d->rdpsnd->Initialize || d->rdpsnd->Initialize(d->rdpsnd, TRUE) != CHANNEL_RC_OK) {
            qCWarning(KRDP) << "Could not initialize RDPSND";
            return false;
        }
        d->audioPlaybackEndpoint = std::make_unique<PipeWireAudioPlayback>();
        // WirePlumber resolves this symbolic target to the current default
        // desktop sink; PipeWire exposes that sink's monitor to Capture
        // streams, keeping application audio distinct from microphone input.
        if (!d->audioPlaybackEndpoint->start(QStringLiteral("@DEFAULT_AUDIO_SINK@"))) {
            qCWarning(KRDP) << "Could not capture PipeWire desktop audio";
            d->audioPlaybackEndpoint.reset();
        }
        qCInfo(KRDP) << "RDPSND channel initialized after explicit media consent";
    }
    if (d->camera.load() && !d->cameraEnumerator && WTSVirtualChannelManagerGetDrdynvcState(vcm) == DRDYNVC_STATE_READY) {
        d->cameraEnumerator = cam_dev_enum_server_context_new(vcm);
        if (d->cameraEnumerator) {
            d->cameraEnumerator->rdpcontext = d->peer->context;
            d->remoteCameras.loopbackDevice = d->server->cameraLoopbackDevice();
            d->cameraEnumerator->userdata = &d->remoteCameras;
            d->cameraEnumerator->SelectVersionRequest = cameraSelectVersion;
            d->cameraEnumerator->DeviceAddedNotification = cameraAdded;
        }
        if (!d->cameraEnumerator || d->cameraEnumerator->Initialize(d->cameraEnumerator, FALSE) != CHANNEL_RC_OK
            || d->cameraEnumerator->Open(d->cameraEnumerator) != CHANNEL_RC_OK) {
            qCWarning(KRDP) << "Could not initialize RDPECAM enumerator";
            if (d->cameraEnumerator) cam_dev_enum_server_context_free(d->cameraEnumerator);
            d->cameraEnumerator = nullptr;
            return false;
        }
        qCInfo(KRDP) << "RDPECAM enumerator opened after explicit media consent";
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
