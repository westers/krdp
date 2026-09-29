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
 *            --media            also join the standard audio channels the way a stock
 *                               client with sound and a microphone does: RDPSND (static,
 *                               the silent `fake` backend) and, over drdynvc, AUDIN
 *                               (ALSA capture). No H.264 is needed; RDPGFX is not
 *                               loaded (unless --gfx).
 *            --microphone DEV   what --media's AUDIN captures: `probe` (the default) is a
 *                               built-in device that delivers 20 ms of silence every
 *                               20 ms and needs no audio server; anything else is an
 *                               ALSA capture device name (`null` never blocks, so it
 *                               floods the channel)
 *            --no-krdpctl       do not join KRDPCTL at all: a stock RDP client (with --silent)
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
 *            --raw FILE.json    send FILE's object verbatim after the handshake and after
 *                               any --apply/--apply-seq: `type` is taken from the file as-is
 *                               (unlike --apply/--query, which set it) and `v` is added only
 *                               if the file omits it, so a deliberately wrong `v` can be
 *                               tested too. Repeatable, sent in order >= 300 ms apart. Every
 *                               reply received before --timeout is printed to stdout as
 *                               `reply: <json>`; the probe exits 0 whether or not that reply
 *                               is an `error` (it is printed either way). For exercising
 *                               records --apply/--query cannot send, e.g. a raw `chroma`
 *                               request.
 *            --raw-gap MS       the pause between two --raw sends (default 300)
 *            --gfx-count        with --gfx: do not decode the frames (gdi's surface command is
 *                               not called, so any payload is accepted); instead print
 *                               `frame seq N surface S` for every frame whose data ends in
 *                               the 16-byte marker "KRDPSEQ:" + a little-endian uint64 (the
 *                               in-flight window loopback test feeds such frames). Frame
 *                               acknowledgements are sent as usual.
 *            --refresh-rect-idle MS  with --gfx: once frames arrived and none has for MS, send
 *                               RDP Refresh Rect PDUs for the whole desktop, once, and log
 *                               `refresh rect sent (N)` (what krdp-client 0.5.5 does when a
 *                               private codec shows nothing on an idle desktop; AUD-FIX10)
 *            --refresh-rect-count N  that many Refresh Rect PDUs back to back (default 1)
 *            --gfx-ack-delay MS with --gfx: acknowledge every frame MS after its EndFrame
 *                               instead of at once, each on its own clock (pipelined, like a
 *                               client whose decode and present take that long; AUD-FIX7 F2).
 *            --clipboard MODE   also load the standard clipboard channel (cliprdr) and, on the
 *                               server's Monitor Ready, announce text the way wlfreerdp3 does
 *                               (one format list per Wayland MIME type: three lists at once).
 *                               Every server data request is logged as `clipboard data request
 *                               for format N`, then MODE: `answer` (UTF-16 text at once), `slow`
 *                               (after 3 s), `never` (no answer; the client carries on) or
 *                               `stall` (no answer, and the whole client stops reading from the
 *                               server, as wlfreerdp3 3.22 does when its synchronous read of the
 *                               local clipboard never returns: AUD-FIX5), or
 *                               `refuse-then-stall` (wlfreerdp3 3.22 as seen live, AUD-FIX6 F1:
 *                               the first request is refused, a real copy on the client
 *                               follows 1 s later - `clipboard: copied` - and any later
 *                               request stalls the client as `stall` does). In every mode a
 *                               server format list with text is requested at once, the text is
 *                               logged as `clipboard: server text "..."`, and then announced
 *                               back (`clipboard: echoed`), the way Klipper re-owns what the
 *                               client just pasted from the server.
 *            --log-pointer      log every RDP pointer update the server sends, as it arrives:
 *                               `pointer: new WxH hot X,Y bpp B cache I` (PointerNew; `large`
 *                               for PointerLarge, `color` for PointerColor), `pointer: cached I`,
 *                               `pointer: system null|default` (FIX-CURSOR)
 *            --disp WxH         also load the standard Display Control channel
 *                               (MS-RDPEDISP, over drdynvc) and, once the server's caps
 *                               arrive, ask for a one-monitor WxH desktop the way a
 *                               stock client does when its window is resized
 *
 * Records go to stdout, one compact JSON object per line, each prefixed with
 * the local time it was read (HH:MM:SS.zzz); everything else to stderr with
 * the same prefix, so a record can be placed against the server's log and
 * against the ResetGraphics line --gfx prints. The password is never printed;
 * give PASSWORD as `-` to read it from the first line of stdin instead of the
 * command line.
 * Once any --raw was given, replies are printed as `reply: <json>` (no time
 * prefix) instead, so a test script can grep for them independent of the
 * clock.
 *
 * Without --gfx no dynamic channel exists, so the server never opens its
 * RDPGFX pipeline for this connection: the GCC early capability flag
 * (SupportGraphicsPipeline) is all KRDP checks at capabilities time, and a
 * drdynvc that never joins is simply never set up. That is what makes the
 * probe usable from a machine whose libfreerdp cannot decode H.264.
 */

#include <atomic>
#include <utility>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <QByteArray>
#include <QDataStream>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTime>

#include <freerdp/addin.h>
#include <freerdp/channels/channels.h>
#include <freerdp/client/audin.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/disp.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/pointer.h>
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
// Between two --raw sends: fixed, unlike --apply-seq's pacing, since a raw
// record (e.g. `chroma`) need not get any reply at all to count as accepted.
constexpr qint64 RawSequenceGapMs = 300; // --raw-gap overrides it

struct Probe {
    Mode mode = Mode::Silent;
    /** Join KRDPCTL (off with --no-krdpctl: behave like a stock RDP client). */
    bool krdpctl = true;
    /** The `apply` bodies, in order: one for --apply, several for --apply-seq. */
    QList<QJsonObject> applyBodies;
    // The first apply is sent from the channel thread (on CHANNEL_EVENT_CONNECTED),
    // the later ones from the main loop, which also reads these to pace them:
    // atomics for visibility (the two never send concurrently, the main loop
    // only sends after the first was answered).
    std::atomic<int> nextApply = 0;
    /** Answers (`layout`/`error`) to OUR applies seen so far (channel thread); never more than nextApply. */
    std::atomic<int> appliesAnswered = 0;
    /** The --raw bodies, in order, sent verbatim (see the file comment); empty when --raw was not given. */
    QList<QJsonObject> rawBodies;
    /** Sent from the main loop only, after sentRequest; atomic for the same reason as nextApply. */
    std::atomic<int> nextRaw = 0;
    qint64 rawGapMs = RawSequenceGapMs;
    bool gfx = false;
    bool media = false;
    /** --log-pointer: log the server's pointer updates (the originals still run). */
    bool logPointer = false;
    pPointerSystem pointerSystem = nullptr;
    pPointerColor pointerColor = nullptr;
    pPointerNew pointerNew = nullptr;
    pPointerCached pointerCached = nullptr;
    pPointerLarge pointerLarge = nullptr;
    /** --disp: the one-monitor layout to ask for over MS-RDPEDISP (empty: channel not loaded). */
    UINT32 dispWidth = 0;
    UINT32 dispHeight = 0;
    QByteArray microphoneDevice = "probe";
    bool pong = true;
    int timeoutSeconds = 0;
    // --gfx evidence: frames seen per RDPGFX surface id.
    std::map<UINT16, unsigned> framesPerSurface;
    /** --gfx-count: count and report markers instead of decoding. */
    bool gfxCountOnly = false;
    /** --gfx-ack-delay: acknowledge each frame this long after its EndFrame (0: FreeRDP acks at once). */
    int gfxAckDelayMs = 0;
    /** --refresh-rect-idle / --refresh-rect-count (0: never). */
    int refreshRectIdleMs = 0;
    int refreshRectCount = 1;
    /** Surface commands seen (any surface), and when the last one arrived (ms since epoch). */
    std::atomic<quint64> gfxFrames = 0;
    std::atomic<qint64> lastGfxFrameAt = 0;
    /** --clipboard MODE (empty: no cliprdr). */
    QByteArray clipboardMode;
    /** --clipboard stall: a data request arrived; the main loop stops reading. */
    std::atomic<bool> clipboardStalled = false;
    /** --clipboard refuse-then-stall: data requests seen so far. */
    std::atomic<int> clipboardRequests = 0;
    /** --clipboard refuse-then-stall: when the main loop makes the client's "real copy" (ms since epoch, 0 = none). */
    std::atomic<qint64> clipboardCopyAt = 0;
    CliprdrClientContext *cliprdr = nullptr;
    /** Set before the disconnect: releases a stalled or slow clipboard handler. */
    std::atomic<bool> stopping = false;

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

/** Same job as printRecord(), for replies once --raw is in play: a stable, greppable line. */
void printReply(const QJsonObject &record)
{
    const QByteArray line = QByteArrayLiteral("reply: ") + QJsonDocument(record).toJson(QJsonDocument::Compact);
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

/**
 * Frames \a record exactly as given, adding "v" only when the object does not already
 * have one. Unlike KRdp::LayoutControl::frame() (which always stamps the canonical
 * ProtocolVersion), this is what --raw uses so a file can send a deliberately wrong
 * "v" to test that path.
 */
QByteArray frameVerbatim(const QJsonObject &record)
{
    QJsonObject toSend = record;
    if (!toSend.contains(QLatin1String("v"))) {
        toSend.insert(QStringLiteral("v"), KRdp::LayoutControl::ProtocolVersion);
    }
    const QByteArray payload = QJsonDocument(toSend).toJson(QJsonDocument::Compact);
    QByteArray out;
    QDataStream stream(&out, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << static_cast<quint32>(payload.size());
    out.append(payload);
    return out;
}

/** \a verbatim selects frameVerbatim() (for --raw) over KRdp::LayoutControl::frame(). */
bool sendRecord(Probe *probe, const QJsonObject &record, bool verbatim = false)
{
    if (!probe->channelOpen) {
        logf("cannot send, channel not open");
        return false;
    }
    const QByteArray framed = verbatim ? frameVerbatim(record) : KRdp::LayoutControl::frame(record);
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

/** The next --raw record of the sequence, framed verbatim (see frameVerbatim()). */
void sendNextRaw(Probe *probe)
{
    const int index = probe->nextRaw.load();
    if (index >= probe->rawBodies.size()) {
        return;
    }
    const QJsonObject body = probe->rawBodies.at(index);
    probe->nextRaw = index + 1;
    if (probe->rawBodies.size() > 1) {
        logf("raw %d of %lld", index + 1, static_cast<long long>(probe->rawBodies.size()));
    }
    sendRecord(probe, body, /*verbatim=*/true);
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
            if (probe->rawBodies.isEmpty()) {
                printRecord(*record);
            } else {
                printReply(*record);
            }
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
pcRdpgfxEndFrame g_gdiEndFrame = nullptr;
pcRdpgfxOnOpen g_previousOnOpen = nullptr;

/**
 * --gfx-ack-delay: FreeRDP's own acknowledgement is switched off in OnOpen, and this thread
 * sends each frame's FRAME_ACKNOWLEDGE its delay after the frame's EndFrame.
 */
struct DelayedAcks {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::pair<std::chrono::steady_clock::time_point, RDPGFX_FRAME_ACKNOWLEDGE_PDU>> due;
    RdpgfxClientContext *gfx = nullptr;
    UINT32 decoded = 0;
    bool active = false;
    std::jthread thread;

    void start(RdpgfxClientContext *context)
    {
        std::lock_guard lock(mutex);
        gfx = context;
        if (active) {
            return;
        }
        active = true;
        thread = std::jthread([this](std::stop_token token) {
            std::unique_lock lock(mutex);
            while (!token.stop_requested()) {
                if (due.empty()) {
                    wake.wait_for(lock, std::chrono::milliseconds(50));
                    continue;
                }
                const auto at = due.front().first;
                if (std::chrono::steady_clock::now() < at) {
                    wake.wait_until(lock, at);
                    continue;
                }
                auto ack = due.front().second;
                due.pop_front();
                auto *context = gfx;
                lock.unlock();
                if (context && context->FrameAcknowledge) {
                    (void)context->FrameAcknowledge(context, &ack);
                }
                lock.lock();
            }
        });
    }
    void add(UINT32 frameId, int delayMs)
    {
        std::lock_guard lock(mutex);
        RDPGFX_FRAME_ACKNOWLEDGE_PDU ack{};
        ack.queueDepth = QUEUE_DEPTH_UNAVAILABLE;
        ack.frameId = frameId;
        ack.totalFramesDecoded = ++decoded;
        due.emplace_back(std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs), ack);
        wake.notify_one();
    }
    void stop()
    {
        if (thread.joinable()) {
            thread.request_stop();
            wake.notify_all();
            thread.join();
        }
        std::lock_guard lock(mutex);
        gfx = nullptr;
        active = false;
    }
};
DelayedAcks g_delayedAcks;
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
        ++probe->gfxFrames;
        probe->lastGfxFrameAt = QDateTime::currentMSecsSinceEpoch();
        if (count == 1) {
            logf("first frame on surface %u (codec %u, %ux%u)", cmd->surfaceId, cmd->codecId, cmd->width, cmd->height);
        }
        if (probe->gfxCountOnly) {
            static constexpr char Magic[] = "KRDPSEQ:";
            constexpr size_t MagicSize = sizeof(Magic) - 1;
            if (cmd->data && cmd->length >= MagicSize + 8 && std::memcmp(cmd->data + cmd->length - 8 - MagicSize, Magic, MagicSize) == 0) {
                quint64 seq = 0;
                for (int i = 7; i >= 0; --i) {
                    seq = (seq << 8) | cmd->data[cmd->length - 8 + size_t(i)];
                }
                logf("frame seq %llu surface %u", static_cast<unsigned long long>(seq), cmd->surfaceId);
            }
            return CHANNEL_RC_OK;
        }
    }
    return g_gdiSurfaceCommand ? g_gdiSurfaceCommand(gfx, cmd) : CHANNEL_RC_OK;
}

UINT probeOnOpen(RdpgfxClientContext *gfx, BOOL *doCapsAdvertise, BOOL *sendFrameAcks)
{
    UINT error = g_previousOnOpen ? g_previousOnOpen(gfx, doCapsAdvertise, sendFrameAcks) : CHANNEL_RC_OK;
    if (g_gfxProbe && g_gfxProbe->gfxAckDelayMs > 0 && sendFrameAcks) {
        *sendFrameAcks = FALSE; // probeEndFrame() acknowledges, late
        g_delayedAcks.start(gfx);
        logf("acknowledging frames %d ms after their end", g_gfxProbe->gfxAckDelayMs);
    }
    return error;
}

UINT probeEndFrame(RdpgfxClientContext *gfx, const RDPGFX_END_FRAME_PDU *pdu)
{
    const UINT error = g_gdiEndFrame ? g_gdiEndFrame(gfx, pdu) : CHANNEL_RC_OK;
    if (g_gfxProbe && g_gfxProbe->gfxAckDelayMs > 0) {
        g_delayedAcks.add(pdu->frameId, g_gfxProbe->gfxAckDelayMs);
    }
    return error;
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
    if (g_gfxProbe && g_gfxProbe->gfxAckDelayMs > 0) {
        g_gdiEndFrame = gfx->EndFrame;
        g_previousOnOpen = gfx->OnOpen;
        gfx->EndFrame = probeEndFrame;
        gfx->OnOpen = probeOnOpen;
    }
}

// ---- --clipboard: a stock client's clipboard (wlfreerdp3-like) ----

Probe *g_clipboardProbe = nullptr;

/// One format list with text, the way a client announces a copy.
UINT probeClipboardAnnounce(CliprdrClientContext *cliprdr, const char *what)
{
    std::vector<CLIPRDR_FORMAT> formats;
    for (const UINT32 id : {UINT32(CF_TEXT), UINT32(CF_OEMTEXT), UINT32(CF_UNICODETEXT)}) {
        CLIPRDR_FORMAT format{};
        format.formatId = id;
        formats.push_back(format);
    }
    CLIPRDR_FORMAT_LIST list{};
    list.common.msgType = CB_FORMAT_LIST;
    list.numFormats = UINT32(formats.size());
    list.formats = formats.data();
    const UINT rc = cliprdr->ClientFormatList(cliprdr, &list);
    logf("clipboard: %s", what);
    return rc;
}

UINT probeClipboardMonitorReady(CliprdrClientContext *cliprdr, const CLIPRDR_MONITOR_READY *)
{
    CLIPRDR_GENERAL_CAPABILITY_SET general{};
    general.capabilitySetType = CB_CAPSTYPE_GENERAL;
    general.capabilitySetLength = 12;
    general.version = CB_CAPS_VERSION_2;
    general.generalFlags = CB_USE_LONG_FORMAT_NAMES;
    CLIPRDR_CAPABILITIES caps{};
    caps.cCapabilitiesSets = 1;
    caps.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET *>(&general);
    UINT rc = cliprdr->ClientCapabilities(cliprdr, &caps);
    // wlfreerdp3 sends its whole list again for every MIME type the compositor offers.
    std::vector<CLIPRDR_FORMAT> formats;
    for (const UINT32 id : {UINT32(CF_TEXT), UINT32(CF_OEMTEXT), UINT32(CF_UNICODETEXT), UINT32(CF_DIB), UINT32(CF_TIFF)}) {
        CLIPRDR_FORMAT format{};
        format.formatId = id;
        formats.push_back(format);
        if (formats.size() < 3) {
            continue;
        }
        CLIPRDR_FORMAT_LIST list{};
        list.common.msgType = CB_FORMAT_LIST;
        list.numFormats = UINT32(formats.size());
        list.formats = formats.data();
        rc |= cliprdr->ClientFormatList(cliprdr, &list);
        logf("clipboard: announced %u formats", list.numFormats);
    }
    return rc;
}

UINT probeClipboardServerCapabilities(CliprdrClientContext *, const CLIPRDR_CAPABILITIES *)
{
    return CHANNEL_RC_OK;
}

UINT probeClipboardServerFormatList(CliprdrClientContext *cliprdr, const CLIPRDR_FORMAT_LIST *list)
{
    logf("clipboard: server announced %u formats", list ? list->numFormats : 0);
    CLIPRDR_FORMAT_LIST_RESPONSE response{};
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    UINT rc = cliprdr->ClientFormatListResponse(cliprdr, &response);
    // A clipboard manager on the client reads the new selection at once.
    bool text = false;
    for (UINT32 i = 0; list && i < list->numFormats; ++i) {
        text |= list->formats[i].formatId == CF_UNICODETEXT;
    }
    if (text) {
        CLIPRDR_FORMAT_DATA_REQUEST request{};
        request.common.msgType = CB_FORMAT_DATA_REQUEST;
        request.common.dataLen = 4;
        request.requestedFormatId = CF_UNICODETEXT;
        rc |= cliprdr->ClientFormatDataRequest(cliprdr, &request);
    }
    return rc;
}

UINT probeClipboardServerFormatListResponse(CliprdrClientContext *, const CLIPRDR_FORMAT_LIST_RESPONSE *)
{
    return CHANNEL_RC_OK;
}

UINT probeClipboardServerFormatDataResponse(CliprdrClientContext *cliprdr, const CLIPRDR_FORMAT_DATA_RESPONSE *response)
{
    if (!response || !(response->common.msgFlags & CB_RESPONSE_OK) || !response->requestedFormatData || response->common.dataLen < 2) {
        logf("clipboard: server refused its text");
        return CHANNEL_RC_OK;
    }
    const QString text = QString::fromUtf16(reinterpret_cast<const char16_t *>(response->requestedFormatData), response->common.dataLen / 2 - 1);
    logf("clipboard: server text \"%s\"", qPrintable(text));
    // ... and the clipboard manager re-owns it: the client announces it back.
    return probeClipboardAnnounce(cliprdr, "echoed");
}

UINT probeClipboardServerFormatDataRequest(CliprdrClientContext *cliprdr, const CLIPRDR_FORMAT_DATA_REQUEST *request)
{
    auto *probe = g_clipboardProbe;
    logf("clipboard data request for format %u", request ? request->requestedFormatId : 0);
    if (!probe) {
        return CHANNEL_RC_OK;
    }
    const auto waitUntil = [probe](QDeadlineTimer deadline) {
        while (!deadline.hasExpired() && !g_interrupted && !probe->stopping) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };
    if (probe->clipboardMode == "never") {
        return CHANNEL_RC_OK;
    }
    if (probe->clipboardMode == "refuse-then-stall") {
        if (probe->clipboardRequests.fetch_add(1) == 0) {
            CLIPRDR_FORMAT_DATA_RESPONSE refusal{};
            refusal.common.msgType = CB_FORMAT_DATA_RESPONSE;
            refusal.common.msgFlags = CB_RESPONSE_FAIL;
            logf("clipboard: refused");
            probe->clipboardCopyAt = QDateTime::currentMSecsSinceEpoch() + 1000;
            return cliprdr->ClientFormatDataResponse(cliprdr, &refusal);
        }
        logf("clipboard: stalling the whole client, as a blocked local clipboard read does");
        probe->clipboardStalled = true;
        waitUntil(QDeadlineTimer(QDeadlineTimer::Forever));
        return CHANNEL_RC_OK;
    }
    if (probe->clipboardMode == "stall") {
        logf("clipboard: stalling the whole client, as a blocked local clipboard read does");
        probe->clipboardStalled = true;
        waitUntil(QDeadlineTimer(QDeadlineTimer::Forever));
        return CHANNEL_RC_OK;
    }
    if (probe->clipboardMode == "slow") {
        waitUntil(QDeadlineTimer(3000));
    }
    static const char16_t text[] = u"probe clipboard";
    CLIPRDR_FORMAT_DATA_RESPONSE response{};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    response.common.dataLen = sizeof(text);
    response.requestedFormatData = reinterpret_cast<const BYTE *>(text);
    logf("clipboard: answering the data request");
    return cliprdr->ClientFormatDataResponse(cliprdr, &response);
}

void onCliprdrConnected(void *context, const ChannelConnectedEventArgs *e)
{
    if (std::strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) != 0) {
        return;
    }
    auto *cliprdr = static_cast<CliprdrClientContext *>(e->pInterface);
    g_clipboardProbe = probeOf(static_cast<rdpContext *>(context));
    if (g_clipboardProbe) {
        g_clipboardProbe->cliprdr = cliprdr;
    }
    cliprdr->MonitorReady = probeClipboardMonitorReady;
    cliprdr->ServerCapabilities = probeClipboardServerCapabilities;
    cliprdr->ServerFormatList = probeClipboardServerFormatList;
    cliprdr->ServerFormatListResponse = probeClipboardServerFormatListResponse;
    cliprdr->ServerFormatDataRequest = probeClipboardServerFormatDataRequest;
    cliprdr->ServerFormatDataResponse = probeClipboardServerFormatDataResponse;
    logf("clipboard channel connected (mode %s)", g_clipboardProbe ? g_clipboardProbe->clipboardMode.constData() : "?");
}

// ---- --disp: a stock client's window resize over MS-RDPEDISP ----

Probe *g_dispProbe = nullptr;

UINT probeDispCaps(DispClientContext *disp, UINT32 maxMonitors, UINT32 factorA, UINT32 factorB)
{
    logf("DISP caps: MaxNumMonitors %u MaxMonitorAreaFactor %ux%u", maxMonitors, factorA, factorB);
    auto *probe = g_dispProbe;
    if (!probe || !probe->dispWidth || !disp->SendMonitorLayout) {
        return CHANNEL_RC_OK;
    }
    DISPLAY_CONTROL_MONITOR_LAYOUT layout{};
    layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
    layout.Width = probe->dispWidth;
    layout.Height = probe->dispHeight;
    layout.PhysicalWidth = 0;
    layout.PhysicalHeight = 0;
    layout.Orientation = ORIENTATION_LANDSCAPE;
    layout.DesktopScaleFactor = 100;
    layout.DeviceScaleFactor = 100;
    const UINT sent = disp->SendMonitorLayout(disp, 1, &layout);
    logf("DISP layout %ux%u sent (%u)", probe->dispWidth, probe->dispHeight, sent);
    return CHANNEL_RC_OK;
}

void onDispConnected(void *context, const ChannelConnectedEventArgs *e)
{
    if (std::strcmp(e->name, DISP_DVC_CHANNEL_NAME) != 0) {
        return;
    }
    auto *disp = static_cast<DispClientContext *>(e->pInterface);
    g_dispProbe = probeOf(static_cast<rdpContext *>(context));
    disp->DisplayControlCaps = probeDispCaps;
    logf("DISP channel connected");
}

// ---- --media's built-in AUDIN device (`--microphone probe`) ----

/*
 * A microphone that needs no audio server: after Open it delivers 20 ms of
 * silence every 20 ms, like a real capture device, until Close. (ALSA's
 * `null` device never blocks, so it would flood the channel, and a capture
 * stream on a graph without a session manager never gets data, which blocks
 * the AUDIN thread and with it drdynvc's close.)
 */
struct ProbeAudin {
    IAudinDevice iface{};
    AUDIO_FORMAT format{};
    std::thread thread;
    std::atomic<bool> stop = false;
};

UINT probeAudinClose(IAudinDevice *device)
{
    auto *audin = reinterpret_cast<ProbeAudin *>(device);
    audin->stop = true;
    if (audin->thread.joinable()) {
        audin->thread.join();
    }
    return CHANNEL_RC_OK;
}

UINT probeAudinOpen(IAudinDevice *device, AudinReceive receive, void *userData)
{
    auto *audin = reinterpret_cast<ProbeAudin *>(device);
    (void)probeAudinClose(device);
    audin->stop = false;
    audin->thread = std::thread([audin, receive, userData]() {
        const size_t align = qMax<size_t>(1, audin->format.nBlockAlign);
        const size_t bytes = qMax<size_t>(align, (audin->format.nAvgBytesPerSec / 50) / align * align);
        const std::vector<BYTE> silence(bytes, 0);
        auto next = std::chrono::steady_clock::now();
        while (!audin->stop) {
            if (receive(&audin->format, silence.data(), silence.size(), userData) != CHANNEL_RC_OK) {
                break;
            }
            next += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next);
        }
    });
    return CHANNEL_RC_OK;
}

BOOL probeAudinFormatSupported(IAudinDevice *, const AUDIO_FORMAT *format)
{
    return format && format->wFormatTag == WAVE_FORMAT_PCM ? TRUE : FALSE;
}

UINT probeAudinSetFormat(IAudinDevice *device, const AUDIO_FORMAT *format, UINT32)
{
    reinterpret_cast<ProbeAudin *>(device)->format = *format;
    return CHANNEL_RC_OK;
}

UINT probeAudinFree(IAudinDevice *device)
{
    (void)probeAudinClose(device);
    delete reinterpret_cast<ProbeAudin *>(device);
    return CHANNEL_RC_OK;
}

UINT VCAPITYPE probeAudinEntry(PFREERDP_AUDIN_DEVICE_ENTRY_POINTS entryPoints)
{
    auto *audin = new ProbeAudin;
    audin->iface.Open = probeAudinOpen;
    audin->iface.FormatSupported = probeAudinFormatSupported;
    audin->iface.SetFormat = probeAudinSetFormat;
    audin->iface.Close = probeAudinClose;
    audin->iface.Free = probeAudinFree;
    const UINT status = entryPoints->pRegisterAudinDevice(entryPoints->plugin, &audin->iface);
    if (status != CHANNEL_RC_OK) {
        delete audin;
    }
    return status;
}

FREERDP_LOAD_CHANNEL_ADDIN_ENTRY_FN g_addinFallback = nullptr;

PVIRTUALCHANNELENTRY probeAddinProvider(LPCSTR name, LPCSTR subsystem, LPCSTR type, DWORD flags)
{
    if (name && subsystem && std::strcmp(name, "audin") == 0 && std::strcmp(subsystem, "probe") == 0) {
        return reinterpret_cast<PVIRTUALCHANNELENTRY>(reinterpret_cast<void *>(probeAudinEntry));
    }
    return g_addinFallback ? g_addinFallback(name, subsystem, type, flags) : nullptr;
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
    if (probe->dispWidth) {
        PubSub_SubscribeChannelConnected(instance->context->pubSub, onDispConnected);
    }
    if (!probe->clipboardMode.isEmpty()) {
        PubSub_SubscribeChannelConnected(instance->context->pubSub, onCliprdrConnected);
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
    auto *settings = instance->context->settings;
    if (probe->media) {
        // What `xfreerdp /sound:sys:fake /microphone:sys:alsa,dev:null` asks for.
        const char *const sound[] = {"rdpsnd", "sys:fake"};
        const QByteArray device = "dev:" + probe->microphoneDevice;
        const char *const alsa[] = {"audin", "sys:alsa", device.constData()};
        const char *const builtin[] = {"audin", "sys:probe"};
        const bool paced = probe->microphoneDevice == "probe";
        if (!freerdp_client_add_static_channel(settings, ARRAYSIZE(sound), sound)
            || !(paced ? freerdp_client_add_dynamic_channel(settings, ARRAYSIZE(builtin), builtin)
                       : freerdp_client_add_dynamic_channel(settings, ARRAYSIZE(alsa), alsa))) {
            logf("could not add the media channels");
            return FALSE;
        }
    }
    if (probe->dispWidth) {
        // What `xfreerdp /dynamic-resolution` loads.
        const char *const disp[] = {"disp"};
        if (!freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, TRUE)
            || !freerdp_client_add_dynamic_channel(settings, ARRAYSIZE(disp), disp)) {
            logf("could not add the Display Control channel");
            return FALSE;
        }
    }
    if (probe->gfx || probe->media || probe->dispWidth || !probe->clipboardMode.isEmpty()) {
        // Without --gfx, keep rdpgfx out of the add-ins: KRDP only checks the
        // GCC flag, and this libfreerdp may not decode H.264.
        const BOOL pipeline = freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline);
        if (!probe->gfx) {
            (void)freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, FALSE);
        }
        const BOOL loaded = freerdp_client_load_addins(instance->context->channels, settings);
        (void)freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, pipeline);
        if (!loaded) {
            logf("freerdp_client_load_addins failed");
            return FALSE;
        }
    }
    if (!probe->krdpctl) {
        logf("not joining KRDPCTL (--no-krdpctl)");
        return TRUE;
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

BOOL logPointerSystem(rdpContext *context, const POINTER_SYSTEM_UPDATE *update)
{
    logf("pointer: system %s", update->type == SYSPTR_NULL ? "null" : update->type == SYSPTR_DEFAULT ? "default" : "other");
    const auto original = probeOf(context)->pointerSystem;
    return original ? original(context, update) : TRUE;
}

BOOL logPointerColor(rdpContext *context, const POINTER_COLOR_UPDATE *update)
{
    logf("pointer: color %ux%u hot %u,%u bpp 24 cache %u", update->width, update->height, update->hotSpotX, update->hotSpotY, update->cacheIndex);
    const auto original = probeOf(context)->pointerColor;
    return original ? original(context, update) : TRUE;
}

BOOL logPointerNew(rdpContext *context, const POINTER_NEW_UPDATE *update)
{
    const auto &color = update->colorPtrAttr;
    logf("pointer: new %ux%u hot %u,%u bpp %u cache %u xor %u", color.width, color.height, color.hotSpotX, color.hotSpotY, update->xorBpp, color.cacheIndex,
         color.lengthXorMask);
    const auto original = probeOf(context)->pointerNew;
    return original ? original(context, update) : TRUE;
}

BOOL logPointerCached(rdpContext *context, const POINTER_CACHED_UPDATE *update)
{
    logf("pointer: cached %u", update->cacheIndex);
    const auto original = probeOf(context)->pointerCached;
    return original ? original(context, update) : TRUE;
}

BOOL logPointerLarge(rdpContext *context, const POINTER_LARGE_UPDATE *update)
{
    logf("pointer: large %ux%u hot %u,%u bpp %u cache %u xor %u", update->width, update->height, update->hotSpotX, update->hotSpotY, update->xorBpp,
         update->cacheIndex, update->lengthXorMask);
    const auto original = probeOf(context)->pointerLarge;
    return original ? original(context, update) : TRUE;
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
    if (probe->logPointer) {
        rdpPointerUpdate *pointer = instance->context->update->pointer;
        probe->pointerSystem = std::exchange(pointer->PointerSystem, logPointerSystem);
        probe->pointerColor = std::exchange(pointer->PointerColor, logPointerColor);
        probe->pointerNew = std::exchange(pointer->PointerNew, logPointerNew);
        probe->pointerCached = std::exchange(pointer->PointerCached, logPointerCached);
        probe->pointerLarge = std::exchange(pointer->PointerLarge, logPointerLarge);
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

bool applySettings(rdpSettings *s, const QString &host, int port, const QString &user, const QString &password, bool dynamicChannels, bool clipboard)
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
    ok &= freerdp_settings_set_bool(s, FreeRDP_RedirectClipboard, clipboard ? TRUE : FALSE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_SupportDynamicChannels, dynamicChannels ? TRUE : FALSE);
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
                 "usage: krdpctl-probe HOST PORT USER PASSWORD (--query | --apply FILE.json | --apply-seq A.json B.json ... | --silent) "
                 "[--gfx [--gfx-count] [--gfx-ack-delay MS] [--refresh-rect-idle MS [--refresh-rect-count N]]] [--media [--microphone DEV]] [--disp WxH] [--clipboard answer|slow|never|stall|refuse-then-stall] [--no-krdpctl] [--log-pointer] [--timeout SECONDS] [--no-pong] [--raw FILE.json]... [--raw-gap MS]\n");
    return 2;
}

/** Reads \a path as a JSON object; logs and returns false if it cannot be opened or is not one. */
bool readJsonFile(const char *path, QJsonObject &out)
{
    QFile file(QString::fromLocal8Bit(path));
    if (!file.open(QIODevice::ReadOnly)) {
        logf("cannot read %s", path);
        return false;
    }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (!doc.isObject()) {
        logf("%s is not a JSON object: %s", path, qPrintable(error.errorString()));
        return false;
    }
    out = doc.object();
    return true;
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
    QString password = QString::fromLocal8Bit(argv[4]);
    if (password == QLatin1String("-")) {
        // Kept off the command line (ps, /proc/PID/cmdline): the first line of stdin.
        char line[1024] = {};
        if (!std::fgets(line, sizeof(line), stdin)) {
            logf("no password on stdin");
            return 2;
        }
        line[std::strcspn(line, "\r\n")] = '\0';
        password = QString::fromLocal8Bit(line);
        std::memset(line, 0, sizeof(line));
    }
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
                QJsonObject body;
                if (!readJsonFile(argv[++i], body)) {
                    return 2;
                }
                probe.applyBodies.push_back(body);
            } while (sequence && i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0);
            probe.mode = Mode::Apply;
            modeGiven = true;
        } else if (arg == QLatin1String("--raw") && i + 1 < argc) {
            // Repeatable; does not select a mode (usually paired with --silent).
            QJsonObject body;
            if (!readJsonFile(argv[++i], body)) {
                return 2;
            }
            probe.rawBodies.push_back(body);
        } else if (arg == QLatin1String("--gfx")) {
            probe.gfx = true;
        } else if (arg == QLatin1String("--log-pointer")) {
            probe.logPointer = true;
        } else if (arg == QLatin1String("--gfx-count")) {
            probe.gfxCountOnly = true;
        } else if (arg == QLatin1String("--refresh-rect-idle") && i + 1 < argc) {
            probe.refreshRectIdleMs = QString::fromLocal8Bit(argv[++i]).toInt();
        } else if (arg == QLatin1String("--refresh-rect-count") && i + 1 < argc) {
            probe.refreshRectCount = std::max(1, QString::fromLocal8Bit(argv[++i]).toInt());
        } else if (arg == QLatin1String("--gfx-ack-delay") && i + 1 < argc) {
            probe.gfxAckDelayMs = QString::fromLocal8Bit(argv[++i]).toInt();
            if (probe.gfxAckDelayMs <= 0 || probe.gfxAckDelayMs > 10000) {
                return usage();
            }
        } else if (arg == QLatin1String("--media")) {
            probe.media = true;
        } else if (arg == QLatin1String("--disp") && i + 1 < argc) {
            const QStringList size = QString::fromLocal8Bit(argv[++i]).split(QLatin1Char('x'));
            probe.dispWidth = size.size() == 2 ? size[0].toUInt() : 0;
            probe.dispHeight = size.size() == 2 ? size[1].toUInt() : 0;
            if (!probe.dispWidth || !probe.dispHeight) {
                return usage();
            }
        } else if (arg == QLatin1String("--microphone") && i + 1 < argc) {
            probe.microphoneDevice = QByteArray(argv[++i]);
        } else if (arg == QLatin1String("--raw-gap") && i + 1 < argc) {
            probe.rawGapMs = qMax(0, QString::fromLocal8Bit(argv[++i]).toInt());
        } else if (arg == QLatin1String("--clipboard") && i + 1 < argc) {
            probe.clipboardMode = QByteArray(argv[++i]);
            if (probe.clipboardMode != "answer" && probe.clipboardMode != "slow" && probe.clipboardMode != "never" && probe.clipboardMode != "stall"
                && probe.clipboardMode != "refuse-then-stall") {
                return usage();
            }
        } else if (arg == QLatin1String("--no-krdpctl")) {
            probe.krdpctl = false;
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
    // After the context: creating it registers libfreerdp's own provider,
    // which ours falls back to for everything but `audin:sys:probe`.
    g_addinFallback = freerdp_get_current_addin_provider();
    (void)freerdp_register_addin_provider(probeAddinProvider, 0);
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

    if (!applySettings(context->settings, host, port, user, password, probe.gfx || probe.media || probe.dispWidth, !probe.clipboardMode.isEmpty())) {
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
    // --raw pacing: fixed RawSequenceGapMs apart, not reply-paced (a raw
    // record, e.g. `chroma`, may draw no reply at all when accepted).
    bool rawStarted = false;
    QDeadlineTimer nextRawAt(QDeadlineTimer::Forever);
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
        // Raw sends start once the handshake's own request has gone out (so
        // they follow any --apply, per the file comment), then space out.
        if (probe.nextRaw.load() < probe.rawBodies.size() && probe.sentRequest.load() && (!rawStarted || nextRawAt.hasExpired())) {
            rawStarted = true;
            sendNextRaw(&probe);
            nextRawAt = probe.nextRaw.load() < probe.rawBodies.size() ? QDeadlineTimer(probe.rawGapMs) : QDeadlineTimer(QDeadlineTimer::Forever);
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
        if (const qint64 copyAt = probe.clipboardCopyAt.load(); copyAt > 0 && QDateTime::currentMSecsSinceEpoch() >= copyAt && probe.cliprdr) {
            // --clipboard refuse-then-stall: a real copy on the client.
            probe.clipboardCopyAt = 0;
            probeClipboardAnnounce(probe.cliprdr, "copied");
        }
        if (const qint64 last = probe.lastGfxFrameAt.load(); probe.refreshRectIdleMs > 0 && probe.gfxFrames.load() > 0
            && QDateTime::currentMSecsSinceEpoch() - last >= probe.refreshRectIdleMs) {
            // --refresh-rect-idle: the desktop went quiet; ask the server to repaint it (once).
            probe.refreshRectIdleMs = 0;
            const RECTANGLE_16 area{0, 0, UINT16(freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth) - 1),
                                    UINT16(freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight) - 1)};
            int sent = 0;
            for (int i = 0; i < probe.refreshRectCount; ++i) {
                if (context->update && context->update->RefreshRect && context->update->RefreshRect(context, 1, &area)) ++sent;
            }
            logf("refresh rect sent (%d) after %lld ms without a frame, %llu frames so far", sent,
                 static_cast<long long>(QDateTime::currentMSecsSinceEpoch() - last), static_cast<unsigned long long>(probe.gfxFrames.load()));
        }
        if (probe.clipboardStalled) {
            // --clipboard stall: the client reads nothing more from the server.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
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

    probe.stopping = true;
    g_delayedAcks.stop(); // before the channel and its context go away
    freerdp_disconnect(instance);
    freerdp_client_context_free(context);
    return exitCode;
}
