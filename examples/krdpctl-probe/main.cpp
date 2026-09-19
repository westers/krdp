// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

/*
 * krdpctl-probe: a minimal libfreerdp client that speaks the `KRDPCTL` static
 * virtual channel (slice 2c layout control, OPT-044), so the server side can
 * be exercised before the own client does.
 *
 *   krdpctl-probe HOST PORT USER PASSWORD --query            send {"type":"query"}, print
 *                                                            every record, exit after the
 *                                                            first `layout`
 *   krdpctl-probe HOST PORT USER PASSWORD --apply FILE.json  send FILE's object as `apply`,
 *                                                            print records until Ctrl-C
 *   krdpctl-probe HOST PORT USER PASSWORD --apply-seq A.json B.json …
 *                                                            send A as `apply`, then each
 *                                                            following file 8 s after the
 *                                                            previous apply was answered
 *                                                            (`layout` or `error`); a live
 *                                                            re-apply sequence on one
 *                                                            connection (Task 4)
 *   krdpctl-probe HOST PORT USER PASSWORD --silent           join the channel, send nothing
 *                                                            (exercises the server's 3 s gate)
 *   options: --gfx              also load the standard channel add-ins (drdynvc, rdpgfx)
 *                               and FreeRDP's software gdi, so the probe is a complete
 *                               AVC420 client; only useful on a libfreerdp built
 *                               WITH_GFX_H264 (buzz's Debian 3.22, not Ubuntu's 3.31).
 *                               With it every ResetGraphics (desktop size and monitor
 *                               rects) and the first frame per surface are reported on
 *                               stderr, plus a frame count at exit.
 *            --timeout SECONDS  give up after this long (default 15 for --query, none
 *                               otherwise)
 *            --no-pong          do not answer the server's `ping` (by default every ping
 *                               is answered with `pong`, as a layout owner must); lets
 *                               the heartbeat release be watched
 *
 * Records go to stdout, one compact JSON object per line, each prefixed with
 * the local time it was read (HH:MM:SS.zzz); everything else to stderr with
 * the same prefix, so a record can be placed against the server's log and
 * against the ResetGraphics line --gfx prints. The password is never printed.
 *
 * Without --gfx no dynamic channel exists, so the server never opens its
 * RDPGFX pipeline for this connection: the GCC early capability flag
 * (SupportGraphicsPipeline) is all KRDP checks at capabilities time, and a
 * drdynvc that never joins is simply never set up. That is what makes the
 * probe usable from a machine whose libfreerdp cannot decode H.264.
 */

#include <atomic>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>

#include <QByteArray>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTime>

#include <freerdp/channels/channels.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/svc.h>
#include <winpr/wlog.h>
#include <winpr/wtsapi.h>

#include "LayoutControl.h"

namespace
{
enum class Mode {
    Query,
    Apply,
    Silent,
};

// Between two applies of --apply-seq, counted from the answer to the
// previous one; and how long an unanswered apply holds the sequence up.
constexpr qint64 ApplySequenceGapMs = 8000;
constexpr qint64 ApplySequenceReplyCapMs = 30000;

struct Probe {
    Mode mode = Mode::Silent;
    /** The `apply` bodies, in order: one for --apply, several for --apply-seq. */
    QList<QJsonObject> applyBodies;
    // The first apply is sent from the channel thread (on CHANNEL_EVENT_CONNECTED),
    // the later ones from the main loop, which also reads these to pace them:
    // atomics for visibility (the two never send concurrently, the main loop
    // only sends after the first was answered).
    std::atomic<int> nextApply = 0;
    /** Answers (`layout`/`error`) to OUR applies seen so far (channel thread); never more than nextApply. */
    std::atomic<int> appliesAnswered = 0;
    bool gfx = false;
    bool pong = true;
    int timeoutSeconds = 0;
    // --gfx evidence: frames seen per RDPGFX surface id.
    std::map<UINT16, unsigned> framesPerSurface;

    // Channel plumbing, filled by the entry point and the init event.
    CHANNEL_ENTRY_POINTS_FREERDP_EX entryPoints{};
    CHANNEL_DEF channelDef{};
    void *initHandle = nullptr;
    DWORD openHandle = 0;
    bool channelOpen = false;
    KRdp::LayoutControl::Deframer deframer;

    std::atomic<bool> sentRequest = false;
    std::atomic<bool> gotLayout = false;
    std::atomic<bool> failed = false;
};

struct ProbeContext {
    rdpClientContext common;
    Probe *probe;
};

std::atomic<bool> g_interrupted = false;

void onSignal(int)
{
    g_interrupted = true;
}

Probe *probeOf(rdpContext *context)
{
    return context ? reinterpret_cast<ProbeContext *>(context)->probe : nullptr;
}

QByteArray stamp()
{
    return QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")).toLatin1();
}

/** One stderr line, time-stamped: "HH:mm:ss.zzz krdpctl-probe: ...". */
void logf(const char *format, ...)
{
    std::fprintf(stderr, "%s krdpctl-probe: ", stamp().constData());
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void printRecord(const QJsonObject &record)
{
    const QByteArray line = stamp() + ' ' + QJsonDocument(record).toJson(QJsonDocument::Compact);
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

bool sendRecord(Probe *probe, const QJsonObject &record)
{
    if (!probe->channelOpen) {
        logf("cannot send, channel not open");
        return false;
    }
    const QByteArray framed = KRdp::LayoutControl::frame(record);
    // libfreerdp keeps the pointer until CHANNEL_EVENT_WRITE_COMPLETE, which
    // hands it back as the user data to free.
    auto *buffer = static_cast<char *>(std::malloc(size_t(framed.size())));
    if (!buffer) {
        return false;
    }
    std::memcpy(buffer, framed.constData(), size_t(framed.size()));
    const UINT rc = probe->entryPoints.pVirtualChannelWriteEx(probe->initHandle, probe->openHandle, buffer, ULONG(framed.size()), buffer);
    if (rc != CHANNEL_RC_OK) {
        logf("VirtualChannelWriteEx failed: %u", rc);
        std::free(buffer);
        return false;
    }
    logf("sent %s (%lld bytes)", qPrintable(record.value(QLatin1String("type")).toString()), static_cast<long long>(framed.size()));
    return true;
}

/** The next `apply` of the sequence (the only one, for --apply). */
void sendNextApply(Probe *probe)
{
    const int index = probe->nextApply.load();
    if (index >= probe->applyBodies.size()) {
        return;
    }
    QJsonObject body = probe->applyBodies.at(index);
    body.insert(QStringLiteral("type"), QStringLiteral("apply"));
    probe->nextApply = index + 1;
    if (probe->applyBodies.size() > 1) {
        logf("apply %d of %lld", index + 1, static_cast<long long>(probe->applyBodies.size()));
    }
    sendRecord(probe, body);
}

void sendRequest(Probe *probe)
{
    if (probe->sentRequest) {
        return;
    }
    probe->sentRequest = true;
    switch (probe->mode) {
    case Mode::Query:
        sendRecord(probe, QJsonObject{{QStringLiteral("type"), QStringLiteral("query")}});
        break;
    case Mode::Apply:
        sendNextApply(probe);
        break;
    case Mode::Silent:
        logf("channel open, sending nothing");
        break;
    }
}

// ---- KRDPCTL static channel: the in-process VirtualChannelEntryEx ----

VOID VCAPITYPE openEvent(LPVOID userParam, DWORD openHandle, UINT event, LPVOID data, UINT32 dataLength, UINT32 totalLength, UINT32 dataFlags)
{
    Q_UNUSED(openHandle)
    Q_UNUSED(totalLength)
    Q_UNUSED(dataFlags)
    auto *probe = static_cast<Probe *>(userParam);
    switch (event) {
    case CHANNEL_EVENT_DATA_RECEIVED:
        // Chunks arrive in order with CHANNEL_FLAG_FIRST/LAST; the deframer
        // does not care where a record is cut, so feed them as they come.
        probe->deframer.feed(QByteArray(static_cast<const char *>(data), int(dataLength)));
        while (auto record = probe->deframer.next()) {
            printRecord(*record);
            const QString type = record->value(QLatin1String("type")).toString();
            if (type == QLatin1String("layout") || type == QLatin1String("error")) {
                if (type == QLatin1String("layout")) {
                    probe->gotLayout = true;
                }
                // Only an answer to one of our applies paces the sequence: a
                // `layout` pushed for another reason (an owner change, a
                // viewer re-description) arrives with none outstanding.
                if (probe->appliesAnswered.load() < probe->nextApply.load()) {
                    ++probe->appliesAnswered;
                }
            } else if (type == QLatin1String("ping")) {
                // The owner's heartbeat (design §3). Answered from the
                // channel thread: VirtualChannelWriteEx only queues.
                if (probe->pong) {
                    sendRecord(probe, QJsonObject{{QStringLiteral("type"), QStringLiteral("pong")}});
                } else {
                    logf("ping ignored (--no-pong)");
                }
            }
        }
        if (probe->deframer.overflowed()) {
            logf("server announced a record over 64 KiB; giving up");
            probe->failed = true;
        }
        break;
    case CHANNEL_EVENT_WRITE_COMPLETE:
    case CHANNEL_EVENT_WRITE_CANCELLED:
        std::free(data);
        break;
    default:
        break;
    }
}

VOID VCAPITYPE initEvent(LPVOID userParam, LPVOID initHandle, UINT event, LPVOID data, UINT dataLength)
{
    Q_UNUSED(data)
    Q_UNUSED(dataLength)
    auto *probe = static_cast<Probe *>(userParam);
    switch (event) {
    case CHANNEL_EVENT_CONNECTED: {
        const UINT rc = probe->entryPoints.pVirtualChannelOpenEx(initHandle, &probe->openHandle, probe->channelDef.name, openEvent);
        if (rc != CHANNEL_RC_OK) {
            logf("VirtualChannelOpenEx failed: %u", rc);
            probe->failed = true;
            return;
        }
        probe->channelOpen = true;
        logf("KRDPCTL connected");
        sendRequest(probe);
        break;
    }
    case CHANNEL_EVENT_DISCONNECTED:
        if (probe->channelOpen) {
            probe->entryPoints.pVirtualChannelCloseEx(initHandle, probe->openHandle);
            probe->channelOpen = false;
        }
        break;
    default:
        break;
    }
}

BOOL VCAPITYPE krdpctlEntryEx(PCHANNEL_ENTRY_POINTS_EX entryPoints, PVOID initHandle)
{
    auto *ex = reinterpret_cast<CHANNEL_ENTRY_POINTS_FREERDP_EX *>(entryPoints);
    if (ex->cbSize < sizeof(CHANNEL_ENTRY_POINTS_FREERDP_EX) || ex->MagicNumber != FREERDP_CHANNEL_MAGIC_NUMBER) {
        return FALSE;
    }
    auto *probe = static_cast<Probe *>(ex->pExtendedData);
    probe->entryPoints = *ex;
    probe->initHandle = initHandle;
    // CHANNEL_DEF::name is CHANNEL_NAME_LEN + 1 bytes and the struct was
    // value-initialised, so the seven characters leave their terminator.
    std::memcpy(probe->channelDef.name, "KRDPCTL", CHANNEL_NAME_LEN);
    probe->channelDef.options = CHANNEL_OPTION_INITIALIZED | CHANNEL_OPTION_ENCRYPT_RDP;
    const UINT rc = ex->pVirtualChannelInitEx(probe, nullptr, initHandle, &probe->channelDef, 1, VIRTUAL_CHANNEL_VERSION_WIN2000, initEvent);
    if (rc != CHANNEL_RC_OK) {
        logf("VirtualChannelInitEx failed: %u", rc);
        return FALSE;
    }
    return TRUE;
}

// ---- --gfx evidence: what the server describes and sends ----

pcRdpgfxResetGraphics g_gdiResetGraphics = nullptr;
pcRdpgfxSurfaceCommand g_gdiSurfaceCommand = nullptr;
// gfx->custom is gdi's own (rdpGdi *), so the probe is reached another way.
Probe *g_gfxProbe = nullptr;

UINT probeResetGraphics(RdpgfxClientContext *gfx, const RDPGFX_RESET_GRAPHICS_PDU *pdu)
{
    std::fprintf(stderr, "%s krdpctl-probe: ResetGraphics desktop %ux%u monitors %u:", stamp().constData(), pdu->width, pdu->height, pdu->monitorCount);
    for (UINT32 i = 0; i < pdu->monitorCount; ++i) {
        const auto &monitor = pdu->monitorDefArray[i];
        std::fprintf(stderr, " [%d,%d %dx%d%s]", monitor.left, monitor.top, monitor.right - monitor.left + 1, monitor.bottom - monitor.top + 1, (monitor.flags & MONITOR_PRIMARY) ? " primary" : "");
    }
    std::fputc('\n', stderr);
    return g_gdiResetGraphics ? g_gdiResetGraphics(gfx, pdu) : CHANNEL_RC_OK;
}

UINT probeSurfaceCommand(RdpgfxClientContext *gfx, const RDPGFX_SURFACE_COMMAND *cmd)
{
    if (auto *probe = g_gfxProbe) {
        const unsigned count = ++probe->framesPerSurface[cmd->surfaceId];
        if (count == 1) {
            logf("first frame on surface %u (codec %u, %ux%u)", cmd->surfaceId, cmd->codecId, cmd->width, cmd->height);
        }
    }
    return g_gdiSurfaceCommand ? g_gdiSurfaceCommand(gfx, cmd) : CHANNEL_RC_OK;
}

void onChannelConnected(void *context, const ChannelConnectedEventArgs *e)
{
    // Subscribed after the generic handler, so gdi has installed its
    // callbacks by the time this runs; wrap them.
    if (std::strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) != 0) {
        return;
    }
    auto *gfx = static_cast<RdpgfxClientContext *>(e->pInterface);
    g_gfxProbe = probeOf(static_cast<rdpContext *>(context));
    g_gdiResetGraphics = gfx->ResetGraphics;
    g_gdiSurfaceCommand = gfx->SurfaceCommand;
    gfx->ResetGraphics = probeResetGraphics;
    gfx->SurfaceCommand = probeSurfaceCommand;
}

// ---- freerdp instance callbacks ----

BOOL preConnect(freerdp *instance)
{
    auto *probe = probeOf(instance->context);
    if (probe->gfx) {
        // The pubsub lives on the context, so this survives the channel
        // reload below; the generic handler does gdi_graphics_pipeline_init()
        // when rdpgfx comes up.
        PubSub_SubscribeChannelConnected(instance->context->pubSub, freerdp_client_OnChannelConnectedEventHandler);
        PubSub_SubscribeChannelDisconnected(instance->context->pubSub, freerdp_client_OnChannelDisconnectedEventHandler);
        PubSub_SubscribeChannelConnected(instance->context->pubSub, onChannelConnected);
    }
    return TRUE;
}

/*
 * Where channels are registered in FreeRDP 3: freerdp_connect() runs
 * PreConnect, then throws context->channels away and creates a fresh one
 * (utils_reload_channels()), calls this hook to fill it, and only then
 * freerdp_channels_pre_connect(). A channel loaded from PreConnect is gone
 * before the connect starts - and client-common's default LoadChannels loads
 * every add-in the settings ask for, which is why the hook has to be
 * replaced rather than added to.
 */
BOOL loadChannels(freerdp *instance)
{
    auto *probe = probeOf(instance->context);
    if (probe->gfx && !freerdp_client_load_addins(instance->context->channels, instance->context->settings)) {
        logf("freerdp_client_load_addins failed");
        return FALSE;
    }
    if (freerdp_channels_client_load_ex(instance->context->channels, instance->context->settings, krdpctlEntryEx, probe) != 0) {
        logf("could not register the KRDPCTL channel");
        return FALSE;
    }
    return TRUE;
}

BOOL desktopResize(rdpContext *context)
{
    // gdi's ResetGraphics handler calls this unconditionally (a client is
    // expected to own the window); resizing gdi's own buffer is all a
    // windowless client has to do.
    const auto width = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
    const auto height = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
    return gdi_resize(context->gdi, width, height);
}

BOOL postConnect(freerdp *instance)
{
    auto *probe = probeOf(instance->context);
    if (probe->gfx) {
        if (!gdi_init(instance, PIXEL_FORMAT_BGRA32)) {
            logf("gdi_init failed");
            return FALSE;
        }
        instance->context->update->DesktopResize = desktopResize;
    }
    logf("connected (security protocol %u)", freerdp_settings_get_uint32(instance->context->settings, FreeRDP_SelectedProtocol));
    return TRUE;
}

void postDisconnect(freerdp *instance)
{
    auto *probe = probeOf(instance->context);
    if (probe->gfx) {
        gdi_free(instance);
    }
}

DWORD verifyCertificate(freerdp *, const char *host, UINT16 port, const char *, const char *subject, const char *issuer, const char *fingerprint, DWORD)
{
    logf("accepting certificate of %s:%u for this run\n  subject %s\n  issuer %s\n  fingerprint %s", host, port, subject, issuer, fingerprint);
    return 2; // accepted for this session only, nothing stored
}

DWORD verifyChangedCertificate(freerdp *, const char *host, UINT16 port, const char *, const char *subject, const char *issuer, const char *fingerprint, const char *, const char *, const char *, DWORD)
{
    logf("accepting CHANGED certificate of %s:%u for this run\n  subject %s\n  issuer %s\n  fingerprint %s", host, port, subject, issuer, fingerprint);
    return 2;
}

bool applySettings(rdpSettings *s, const QString &host, int port, const QString &user, const QString &password, bool gfx)
{
    bool ok = true;
    // Keep libfreerdp's certificate store out of ~/.config/freerdp: a
    // throwaway per-boot directory that the temporary accept in
    // verifyCertificate() never writes to anyway.
    const QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR", QDir::tempPath());
    const QString configPath = runtimeDir + QStringLiteral("/krdpctl-probe");
    QDir().mkpath(configPath);
    ok &= freerdp_settings_set_string(s, FreeRDP_ConfigPath, configPath.toUtf8().constData());

    ok &= freerdp_settings_set_string(s, FreeRDP_ServerHostname, host.toUtf8().constData());
    ok &= freerdp_settings_set_uint32(s, FreeRDP_ServerPort, UINT32(port));
    ok &= freerdp_settings_set_string(s, FreeRDP_Username, user.toUtf8().constData());
    ok &= freerdp_settings_set_string(s, FreeRDP_Password, password.toUtf8().constData());
    // KRDP: TLS only, NLA off, credentials checked in its PostConnect.
    ok &= freerdp_settings_set_bool(s, FreeRDP_TlsSecurity, TRUE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_NlaSecurity, FALSE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_RdpSecurity, FALSE);
    // Neither IgnoreCertificate (skips the callback, prints nothing) nor
    // AutoAcceptCertificate (accepts before the callback, with libfreerdp's
    // "host key changed" banner and a store write): VerifyCertificateEx
    // above accepts every certificate for the run and prints its fingerprint.
    ok &= freerdp_settings_set_bool(s, FreeRDP_IgnoreCertificate, FALSE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_AutoAcceptCertificate, FALSE);

    // What KRDP requires at capabilities time, whether or not the pipeline
    // is ever opened (see the file comment).
    ok &= freerdp_settings_set_bool(s, FreeRDP_SupportGraphicsPipeline, TRUE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_GfxH264, TRUE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_GfxAVC444, FALSE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_GfxAVC444v2, FALSE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_GfxProgressive, FALSE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_GfxThinClient, FALSE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_GfxSmallCache, FALSE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_RemoteFxCodec, FALSE);
    ok &= freerdp_settings_set_uint32(s, FreeRDP_ColorDepth, 32);
    ok &= freerdp_settings_set_bool(s, FreeRDP_SoftwareGdi, TRUE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_DesktopResize, TRUE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_NetworkAutoDetect, TRUE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_SupportDisplayControl, FALSE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_RedirectClipboard, FALSE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_SupportDynamicChannels, gfx ? TRUE : FALSE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_UseMultimon, FALSE);
    ok &= freerdp_settings_set_uint32(s, FreeRDP_MonitorCount, 0);
    ok &= freerdp_settings_set_uint32(s, FreeRDP_DesktopWidth, 1920);
    ok &= freerdp_settings_set_uint32(s, FreeRDP_DesktopHeight, 1080);
    ok &= freerdp_settings_set_string(s, FreeRDP_ClientHostname, "krdpctl-probe");
    ok &= freerdp_settings_set_uint32(s, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
    ok &= freerdp_settings_set_uint32(s, FreeRDP_OsMinorType, OSMINORTYPE_NATIVE_WAYLAND);
    return ok;
}

int usage()
{
    std::fprintf(stderr,
                 "usage: krdpctl-probe HOST PORT USER PASSWORD (--query | --apply FILE.json | --apply-seq A.json B.json ... | --silent) [--gfx] [--timeout SECONDS] [--no-pong]\n");
    return 2;
}

bool done(const Probe *probe)
{
    if (probe->failed) {
        return true;
    }
    return probe->mode == Mode::Query && probe->gotLayout;
}
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        return usage();
    }
    const QString host = QString::fromLocal8Bit(argv[1]);
    const int port = QString::fromLocal8Bit(argv[2]).toInt();
    const QString user = QString::fromLocal8Bit(argv[3]);
    const QString password = QString::fromLocal8Bit(argv[4]);
    if (port <= 0 || port > 65535) {
        return usage();
    }

    Probe probe;
    bool modeGiven = false;
    for (int i = 5; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--query")) {
            probe.mode = Mode::Query;
            modeGiven = true;
        } else if (arg == QLatin1String("--silent")) {
            probe.mode = Mode::Silent;
            modeGiven = true;
        } else if ((arg == QLatin1String("--apply") || arg == QLatin1String("--apply-seq")) && i + 1 < argc) {
            // --apply takes one file; --apply-seq every following argument
            // up to the next option.
            const bool sequence = arg == QLatin1String("--apply-seq");
            do {
                QFile file(QString::fromLocal8Bit(argv[++i]));
                if (!file.open(QIODevice::ReadOnly)) {
                    logf("cannot read %s", argv[i]);
                    return 2;
                }
                QJsonParseError error;
                const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
                if (!doc.isObject()) {
                    logf("%s is not a JSON object: %s", argv[i], qPrintable(error.errorString()));
                    return 2;
                }
                probe.applyBodies.push_back(doc.object());
            } while (sequence && i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0);
            probe.mode = Mode::Apply;
            modeGiven = true;
        } else if (arg == QLatin1String("--gfx")) {
            probe.gfx = true;
        } else if (arg == QLatin1String("--no-pong")) {
            probe.pong = false;
        } else if (arg == QLatin1String("--timeout") && i + 1 < argc) {
            probe.timeoutSeconds = QString::fromLocal8Bit(argv[++i]).toInt();
        } else {
            return usage();
        }
    }
    if (!modeGiven) {
        return usage();
    }
    if (probe.timeoutSeconds <= 0 && probe.mode == Mode::Query) {
        probe.timeoutSeconds = 15;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // stdout is for records only: winpr's console appender would otherwise
    // put everything up to INFO there (the channel add-ins --gfx loads log
    // a few lines at that level).
    wLog *root = WLog_GetRoot();
    if (WLog_SetLogAppenderType(root, WLOG_APPENDER_CONSOLE)) {
        WLog_ConfigureAppender(WLog_GetLogAppender(root), "outputstream", const_cast<char *>("stderr"));
    }
    // libfreerdp prints its "REMOTE HOST IDENTIFICATION HAS CHANGED" banner
    // at ERROR level for every certificate its store does not know, which
    // is every one this probe sees: verifyCertificate() prints the
    // fingerprint instead, and a failed handshake is still reported through
    // freerdp_connect()'s last error below.
    WLog_SetLogLevel(WLog_Get("com.freerdp.crypto"), WLOG_FATAL);

    RDP_CLIENT_ENTRY_POINTS entryPoints{};
    entryPoints.Size = sizeof(entryPoints);
    entryPoints.Version = RDP_CLIENT_INTERFACE_VERSION;
    entryPoints.ContextSize = sizeof(ProbeContext);
    entryPoints.GlobalInit = []() -> BOOL {
        return TRUE;
    };
    entryPoints.GlobalUninit = []() {};
    entryPoints.ClientNew = [](freerdp *, rdpContext *) -> BOOL {
        return TRUE;
    };
    entryPoints.ClientFree = [](freerdp *, rdpContext *) {};
    entryPoints.ClientStart = [](rdpContext *) -> int {
        return 0;
    };
    entryPoints.ClientStop = [](rdpContext *) -> int {
        return 0;
    };

    rdpContext *context = freerdp_client_context_new(&entryPoints);
    if (!context) {
        logf("freerdp_client_context_new failed");
        return 1;
    }
    reinterpret_cast<ProbeContext *>(context)->probe = &probe;

    freerdp *instance = context->instance;
    instance->PreConnect = preConnect;
    instance->LoadChannels = loadChannels;
    instance->PostConnect = postConnect;
    instance->PostDisconnect = postDisconnect;
    instance->VerifyCertificateEx = verifyCertificate;
    instance->VerifyChangedCertificateEx = verifyChangedCertificate;

    if (!applySettings(context->settings, host, port, user, password, probe.gfx)) {
        logf("settings rejected");
        freerdp_client_context_free(context);
        return 1;
    }

    logf("connecting to %s:%d as %s%s", qPrintable(host), port, qPrintable(user), probe.gfx ? " (with gfx)" : "");
    int exitCode = 0;
    if (!freerdp_connect(instance)) {
        const UINT32 code = freerdp_get_last_error(context);
        logf("connect failed: %s (0x%08x)", freerdp_get_last_error_string(code), code);
        freerdp_client_context_free(context);
        return 1;
    }

    const QDeadlineTimer deadline = probe.timeoutSeconds > 0 ? QDeadlineTimer(qint64(probe.timeoutSeconds) * 1000) : QDeadlineTimer(QDeadlineTimer::Forever);
    // --apply-seq pacing: the next apply goes ApplySequenceGapMs after the
    // previous one was answered (or ApplySequenceReplyCapMs after it was
    // sent, if never answered). Answers are counted on the channel thread.
    int answersPaced = 0;
    QDeadlineTimer nextApplyAt(QDeadlineTimer::Forever);
    QDeadlineTimer replyCap(QDeadlineTimer::Forever);
    HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};
    while (!g_interrupted && !done(&probe)) {
        if (probe.mode == Mode::Apply && probe.nextApply.load() < probe.applyBodies.size() && probe.sentRequest.load()) {
            const int answered = probe.appliesAnswered.load();
            if (answered > answersPaced) {
                answersPaced = answered;
                nextApplyAt = QDeadlineTimer(ApplySequenceGapMs);
                replyCap = QDeadlineTimer(QDeadlineTimer::Forever);
            } else if (replyCap.isForever() && nextApplyAt.isForever()) {
                replyCap = QDeadlineTimer(ApplySequenceReplyCapMs);
            }
            if (nextApplyAt.hasExpired() || replyCap.hasExpired()) {
                if (replyCap.hasExpired()) {
                    logf("apply %d unanswered after %lld s; sending the next one anyway", probe.nextApply.load(), static_cast<long long>(ApplySequenceReplyCapMs / 1000));
                }
                nextApplyAt = QDeadlineTimer(QDeadlineTimer::Forever);
                replyCap = QDeadlineTimer(QDeadlineTimer::Forever);
                sendNextApply(&probe);
            }
        }
        if (deadline.hasExpired()) {
            logf("timeout after %d s", probe.timeoutSeconds);
            exitCode = probe.mode == Mode::Query ? 3 : 0;
            break;
        }
        if (freerdp_shall_disconnect_context(context)) {
            const UINT32 info = freerdp_error_info(instance);
            logf("server ended the session: %s (0x%08x)", freerdp_get_error_info_string(info), info);
            exitCode = done(&probe) ? 0 : 4;
            break;
        }
        const DWORD count = freerdp_get_event_handles(context, handles, MAXIMUM_WAIT_OBJECTS);
        if (count == 0) {
            logf("no event handles");
            exitCode = 1;
            break;
        }
        const DWORD status = WaitForMultipleObjects(count, handles, FALSE, 100);
        if (status == WAIT_FAILED) {
            logf("WaitForMultipleObjects failed");
            exitCode = 1;
            break;
        }
        if (!freerdp_check_event_handles(context)) {
            if (freerdp_shall_disconnect_context(context)) {
                const UINT32 info = freerdp_error_info(instance);
                logf("server ended the session: %s (0x%08x)", freerdp_get_error_info_string(info), info);
            } else {
                const UINT32 code = freerdp_get_last_error(context);
                logf("connection lost: %s (0x%08x)", freerdp_get_last_error_string(code), code);
            }
            exitCode = done(&probe) ? 0 : 4;
            break;
        }
    }
    if (probe.failed) {
        exitCode = 1;
    }
    if (g_interrupted) {
        logf("interrupted");
    }
    if (probe.gfx) {
        unsigned total = 0;
        QString perSurface;
        for (const auto &[surface, count] : probe.framesPerSurface) {
            total += count;
            perSurface += QStringLiteral(" surface %1: %2").arg(surface).arg(count);
        }
        logf("%u frame(s) received%s", total, qPrintable(perSurface));
    }

    freerdp_disconnect(instance);
    freerdp_client_context_free(context);
    return exitCode;
}
