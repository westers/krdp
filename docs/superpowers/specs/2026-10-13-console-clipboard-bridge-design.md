# Clipboard bridge for the Console (:3391) and Virtual (:3395) routes (design, 2026-10-13)

Status: DESIGN FOR STEVE'S APPROVAL. No code has been written. Wire version 15 is what ships today; this design makes it 16.
Evidence base: `~/dev/rdp/evidence/2026-10-13-clipboard-sol-to-hal/NOTES.md` plus the code read for this document. Every
statement about current behaviour carries a `file:line` (tree: `master` at `e66e5c92`; client `~/dev/krdp-client` 0.8.0).

## 1. Verdict and goals

### 1.1 Verdict (read this first)

1. **Neither production route has a clipboard bridge.** Console is empty by design (stub), and **Virtual is empty too**:
   the NOTES hypothesis that Virtual "would work in principle" is wrong. `krdp-virtual-host` builds a
   `ConsoleWorkerSession` per client (`server/VirtualSessionTransport.h:5,166`), the very class whose
   `setClipboardData` is an empty stub (`server/ConsoleWorkerSession.cpp:86-91`), and it wires frames and input to it
   (`server/VirtualSessionTransport.cpp:181-185`) but no clipboard (grep of `server/VirtualSession*`, `virtualhost.cpp`:
   zero hits for `clip`). The Virtual desktop's worker is the same `krdp-console-worker` binary run with `--virtual-session
   ... --desktop-media` (`scripts/virtual-session-desktop.sh:89`), so the same missing records affect both routes. One
   design and one wire change fixes both.
2. **There is no hidden working path.** The only clipboard code that ever worked lives in the single-process
   `krdpserver` route (:3389, retired): `SessionController.cpp:497` connects `clipboardDataChanged` to
   `Clipboard::setServerData` and `:519-520` connects `Clipboard::clientDataChanged` to `setClipboardData`. Console and
   Virtual use `ConsoleHostController::addClient` / `VirtualSessionHostController::addClient` instead
   (`server/ConsoleHostController.cpp:900-1010`, `server/VirtualSessionHostController.cpp:620-641`), neither of which
   touches `clipboard()`.
3. **There is no "Clipboard" server setting today.** `server/krdpserversettings.kcfg` has no clipboard entry (entries
   listed `:13-118`, none named clipboard), and the broker host keys (`server/BrokerHostSettings.cpp:14-30`) have none.
   The only switch is the client's per-connection `clipboard` flag (default true, `krdp-client/src/core/Connection.h:103`)
   which maps to `FreeRDP_RedirectClipboard` (`krdp-client/src/rdp/SessionSettings.cpp:106`): off means the cliprdr channel
   is never joined and the server never initialises `Clipboard` (`src/RdpConnection.cpp:1693-1696`).
4. **Decision proposed:** a text-only bridge over the standard CLIPRDR channel (no new client protocol), with delayed
   rendering, a pure-function policy planner that decides per state which direction is allowed, a new broker/worker wire
   (version 16) of six records, one new host setting `ClipboardDirection` per route (default `both`, state policy narrows it
   automatically), and clipboard sharing only for the connection that controls the console.

### 1.2 Goals

- G1. Plain text copy/paste both ways between the client and the host desktop on Console (user session) and on Virtual.
- G2. Safe by state: nothing is exposed that the client could not already see or do (section 2).
- G3. Paired server-only change; our own client and stock clients (mstsc, FreeRDP, Remmina) keep working with no client release.
- G4. Never costs a stock client its picture (reuse the AUD-FIX5/FIX6 rules; `autotests/ClipboardStallLoopbackTest.cpp:4-23`).
- G5. Contents are never logged; sizes and rates are bounded; a mismatched pair fails closed.

### 1.3 Non-goals (explicit later phases)

- Images (CF_DIB/PNG), HTML, RTF, files (CLIPRDR file streams), drag and drop: phase 2+ (section 11). Today
  `streamFileClipEnabled = FALSE`, `canLockClipData = FALSE`, `hasHugeFileSupport = FALSE` (`src/Clipboard.cpp:226-231`)
  and the code only ever offers CF_UNICODETEXT (`:296-322`).
- X11/primary selection (`QClipboard::Selection`); the existing bridge ignores it too
  (`src/PlasmaScreencastV1Session.cpp:430`).
- Clipboard history sync, a "clipboard manager" view, persistent queues.
- Sharing with viewers (non-controllers), see 2.3.

## 2. Threat model and policy

### 2.1 Facts the policy is built on

- Any PAM-authenticated RDP user may see the SDDM greeter and **any locked desktop**; only an unlocked desktop is the
  owner's alone (`server/ConsoleAdmission.h:12-39`: Greeter `true`; PhysicalUser `uid == seat.uid || locked`; evaluated per
  client in `ConsoleHostController::admissible`, `:688-693`, and re-evaluated on every seat change, `:695-707`).
  Therefore on a locked desktop of user X, an authenticated user Y may be connected. Y must get nothing of X's clipboard and
  must not be able to plant text into it.
- The worker runs as the seat's user: the greeter worker as `sddm` (adapter `Greeter`), the session worker as the logged-in
  user; only a physical user gets `--desktop-media` (`server/ConsoleWorkerLauncher.cpp:368-370`), which becomes
  `m_authenticatedDesktop` in the worker (`server/consoleworker.cpp:135`). Greeter and session workers are separate
  processes with separate logind session ids and uids; the endpoint authenticates `sessionId`, `uid` and a token
  (`server/ConsoleWorkerEndpoint.cpp:372-380`), so a record can never be confused across workers at the socket level.
- The broker is a system service. It must not read the greeter or user clipboard itself; this is the intent recorded at
  `server/ConsoleWorkerSession.cpp:89-90`. The bridge keeps that: only the worker touches `KSystemClipboard`.
- Control: exactly one admitted client owns control, others view (`server/ConsoleControl.h:32`;
  `m_control.ownsControl(id)` already gates input, `server/ConsoleHostController.cpp:920-924`).
- The worker already learns lock edges (`ConsoleLockWatch`, `server/ConsoleLockWatch.h:9-27`) but the broker's view
  (`session->locked`, `ConsoleHostController.cpp:692`, from logind) is authoritative and is what admission uses.

### 2.2 Policy per state (pure planner, section 7.2)

`ClipboardPolicy::plan(Input) -> {hostToClient, clientToHost, reason}`; inputs: route, seat adapter (Greeter / PhysicalUser /
VirtualUser), session locked, client uid, session uid, `ownsControl`, admin `ClipboardDirection`, client advertised the
channel. `reason` is a fixed enum string for the log (never data).

| State | Host to client | Client to host | Why |
|---|---|---|---|
| SDDM greeter | deny | allow (write only) | Greeter clipboard is not the user's data and could hold a previous visitor's paste; the only legitimate use is pasting a username or password into the greeter. Any authenticated user may use the greeter (`ConsoleAdmission.h:25`), so reading back is the risk, writing is not. |
| Locked session, client uid == session uid | allow | allow (marked secret, see 2.5) | Same account that PAM just authenticated; equivalent to unlocking for the data it can already reach. Pasting a password into the lock screen is a primary use. |
| Locked session, client uid != session uid | deny | deny | Y can see X's lock screen (`ConsoleAdmission.h:30`) but must neither read X's clipboard nor plant content X later pastes. |
| Unlocked session, client uid == session uid | allow | allow | The normal case (admission already excludes other uids, `ConsoleAdmission.h:30`). |
| No controller (viewer connection) | deny | deny | Viewers must not read or write what the controller's session holds; when control moves, section 3.6 resets state. |
| Virtual desktop (:3395) | allow | allow | Owner is always the authenticated PAM user (`VirtualSessionTransport` is created with `authenticatedUserUid`, `VirtualSessionTransport.cpp:138,193`); no greeter, no foreign viewer. Lock state is not consulted (same user). |
| Seat adapter `None` | deny | deny | Nothing on the seat (`ConsoleAdmission.h:34-37`). |

The planner result is re-evaluated on: seat change, lock edge, control change, client uid known (post-auth), admin setting
change at startup only (settings are read at service start, like the other broker keys, section 7). Each re-evaluation that
changes the result sends a new `ClipboardPolicy` record (3.2) and, for a direction going from allow to deny, drops the
state described in 3.6.

### 2.3 Multiple viewers

Only the control owner's connection gets a bridged `Clipboard`. For every other admitted client the planner returns deny
and the broker neither announces nor requests (see 5.3 for how `Clipboard` is told). When control changes hands, both
the old and the new controller get a state reset, and the new controller sees nothing of the host's current clipboard until
the next host copy (recommended default; open question Q4).

### 2.4 Greeter worker to user worker handover

At login the seat target changes from the greeter to the user session (`ConsoleHandoff::targetFor`; the worker is replaced,
`ConsoleHostController.cpp:545-626`). Rules so nothing crosses:

1. All clipboard state lives per worker endpoint (offer sequence, outstanding request ids, last-set id). It is cleared in
   `ConsoleWorkerEndpoint::workerDisconnected` (`ConsoleWorkerEndpoint.cpp:515-520`) and on `workerAuthenticated`.
2. The RDP connection survives the handover, so the connection's `Clipboard` also survives and still holds the *previous*
   worker's announcement (`serverData`, `src/Clipboard.cpp:268-274`). The handover therefore calls a new
   `Clipboard::resetHostSide()` (3.6) that frees the snapshot, withdraws the announcement and fails any pending request.
3. A request id is bound to `(endpoint instance, request id)`; a late `ClipboardData` from a dead worker cannot be delivered
   because the endpoint object forgets it on disconnect. Worker authentication already fixes `(sessionId, uid)`.
4. Text the client pasted into the greeter dies with the greeter compositor; it is never copied by the broker.
   What the client already holds locally (our client eager-fetches every offer, `SessionModelClipboard.cpp:104-116`)
   is the same authenticated user's own copy and is out of scope.

### 2.5 Content, size, rate

- **Never logged.** Info lines carry counts, lengths (characters or bytes), state names, policy reasons and request ids.
  Contents appear in no log level, no stats panel, no crash text, no KRDPCTL record, no D-Bus signal. The existing debug
  lines already print only lengths and format ids (`src/Clipboard.cpp:345-395`, `:472`, `:536`); keep that rule and
  add a test that greps captured log output for a canary token (section 8).
- **Size limit.** Text payload at most **1 MiB UTF-8 on the worker wire and 2 MiB UTF-16 on the cliprdr wire** (the same
  1 MiB figure our client enforces on both directions, `SessionModelClipboard.cpp:24,152,169`). The broker rejects an
  oversized client response before it is turned into a `QString` (today there is no cap: `Clipboard.cpp:152-160` copies
  whatever `dataLen` says). Oversize: refuse (host to client: FAIL response; client to host: dropped), one Info line.
- **Rate limit.** Host to client: the worker coalesces host changes (100 ms settle, only the newest offer is sent) and the
  broker sends at most one format list per 250 ms (reuses the spirit of `FormatListSettleMs`, `Clipboard.h:60`).
  Client to host: at most one data request outstanding (existing, `Clipboard.cpp:416-423`), at most 20 accepted sets per
  10 s and 8 MiB per minute per connection; beyond that the set is dropped with one Info line per episode.
- **Klipper history.** Anything the bridge writes into a user session lands in Klipper's history in clear. For writes made
  while the session is locked (or the greeter), the worker sets `x-kde-passwordManagerHint = secret` on the MIME data so
  Klipper skips history (to be verified on Sol in spike S3). Unlocked writes are ordinary.
- **Direction setting** and per-connection flag: see section 7.

## 3. Architecture

### 3.1 Roles

```
client (CLIPRDR) <-- RDP --> BROKER (system service)                WORKER (greeter: sddm / session: user)
                              RdpConnection::clipboard()  <-wire->   HostClipboard (KSystemClipboard)
                              Clipboard (CLIPRDR PDUs)               echo guard, size cap, secret hint
                              ConsoleWorkerSession (adapter)
                              ClipboardPolicy planner
```

- The **worker owns the host clipboard** through one new `HostClipboard` object (a thin extraction of the code now inside
  `PlasmaScreencastV1Session`, 5.1) and never talks CLIPRDR.
- The **broker owns CLIPRDR toward the client** with the existing `Clipboard` class, extended with a delayed-rendering
  source (3.4) instead of the in-process `QMimeData` snapshot. The broker never holds a host clipboard copy longer than one
  in-flight transfer.
- `AbstractSession::clipboardDataChanged(const QMimeData *)` and `setClipboardData(unique_ptr<QMimeData>)`
  (`src/AbstractSession.h:154,270`) stay as the in-process contract. `ConsoleWorkerSession` implements `setClipboardData`
  by emitting a new `clientClipboardText(QString)` signal that the endpoint turns into `ClipboardSet`; the host-to-client
  side is not routed through `clipboardDataChanged` (that signal carries data eagerly, which delayed rendering avoids).

### 3.2 Wire v16 records (`server/ConsoleWorkerWire.h`)

Append after `HostScreensChanged` (`ConsoleWorkerWire.h:119-129`) and move `LastKind` (`:130`). `ProtocolVersion` 15 -> 16
(`:64`; the test pins it at `autotests/ConsoleWorkerWireTest.cpp:557`). Binary `QDataStream`, big-endian, like `Audio`;
every decoder bounds its payload (as `MaxCameraJpegBytes` does for camera) and rejects trailing bytes.

| Kind | Direction | Fields | Notes |
|---|---|---|---|
| `ClipboardPolicy` | broker -> worker | `revision u64` (strictly increasing, endpoint-local), `controlGeneration u64`, `hostToClient bool`, `clientToHost bool`, `locked bool`, `greeter bool` | Authoritative decision from the planner. `locked`/`greeter` are only used for the secret hint and for the worker's own defence in depth. Sent right after Hello is accepted and on every change. Stale revision ignored (same rule as `DisplayPolicy.revision`, `ConsoleWorkerWire.h:1099-1106`). |
| `ClipboardOffer` | worker -> broker | `seq u64` (per worker lifetime, from 1), `flags u8` (bit0 text available) | A host change. No contents, no length. Sent only while `hostToClient` is allowed. Coalesced to the newest. Only valid after `Ready`; before it the broker fails the worker as for any unexpected record (`ConsoleWorkerEndpoint.cpp:418-421`). |
| `ClipboardRequest` | broker -> worker | `requestId u64` (nonzero, increasing), `maxBytes u32` | The client pasted (CLIPRDR `FormatDataRequest` for CF_UNICODETEXT). |
| `ClipboardData` | worker -> broker | `requestId u64`, `seq u64`, `status u8` (Ok, Empty, Gone, TooLarge, Denied), `text` UTF-8, LF line endings, at most `maxBytes` | `seq` is the offer it was read for (may be newer than the one announced). Not-Ok statuses carry no text. |
| `ClipboardSet` | broker -> worker | `setId u64` (nonzero, increasing), `controlGeneration u64`, `text` UTF-8 (LF) | Client copied. Worker writes it only if `mayApplyClipboardSet(set, control, policy)` holds. |
| `ClipboardSetResult` | worker -> broker | `setId u64`, `status u8` (Written, Denied, Failed) | Lets the broker count and log; there is no retry. |

Gates, written in the style of `mayApplyQuality` (`ConsoleWorkerWire.h:1163`) and unit-tested as pure functions:

- `mayApplyClipboardSet`: `control.active`, `set.controlGeneration == control.generation`, `policy.clientToHost`,
  `text.size() <= MaxClipboardBytes`, `setId > lastSetId`. A worker that is `!m_authenticatedDesktop` (greeter) additionally
  requires `policy.greeter` to be true, so a broker bug cannot make a desktop write look like a greeter write or vice versa.
- `mayAnswerClipboardRequest`: `policy.hostToClient`, `control.active`, and the request's `requestId` newer than the last.
- `mayOffer`: `policy.hostToClient`.

Caps: `MaxClipboardBytes = 1 MiB` (`ClipboardData` and `ClipboardSet` payload decode limit), well under `MaxRecordBytes`
64 MiB (`ConsoleWorkerWire.h:65`).

**Backpressure.** Offers are tiny and latest-wins, so they need no queue. For `ClipboardSet` and `ClipboardRequest` the
endpoint copies the audio/camera pattern: refuse when the worker socket backlog exceeds `2 * MaxClipboardBytes`
(`ConsoleWorkerEndpoint.cpp:288,310` are the models) and answer the client with a failed response. Worker side: at most
one `ClipboardData` in flight; a newer `ClipboardRequest` supersedes (the older request id is answered `Gone`).

**Cancellation.** No explicit cancel record. A request is cancelled by (a) its timeout (3.4), (b) a policy change to deny
(worker answers any pending request `Denied` and forgets), (c) worker disconnect (endpoint drops all ids), (d) a newer
request (supersedes). A late answer for a forgotten id is discarded at the endpoint.

**Versioning and mismatch.** `frame()` stamps `ProtocolVersion` into every record (`ConsoleWorkerWire.h:1422`) and the
deframer reports a peer with another version (`:1880-1886`). A v15 worker meeting a v16 broker (or the reverse) fails the
way every earlier bump did: the endpoint emits `versionMismatch` and fails the worker (`ConsoleWorkerEndpoint.cpp:505-508`),
the host logs "install the paired broker and worker together" (`ConsoleHostController.cpp:548-550`) and applies the
relaunch backoff (`:620-630`) instead of retrying in a loop. Nothing falls back to "clipboard off but video on": the pair
is replaced together (rule in the project CLAUDE.md, Packaging). The same package carries both binaries for Console and
Virtual, so a normal upgrade cannot produce the mismatch.

### 3.3 Flow: host to client (delayed rendering)

```
host app copies -> KSystemClipboard::changed(Clipboard)                       [worker]
  HostClipboard: skip if our own write (flag + lastSetText), skip if !policy.hostToClient,
                 skip if no text format, settle 100 ms, seq++, send ClipboardOffer{seq,text}
broker endpoint: validate (authenticated, Ready, seq increasing) -> ConsoleWorkerSession::hostClipboardOffered(seq)
  Clipboard::announceHostText(seq):          [main thread, existing queued path]
      if !policy or !graphicsDelivered(AUD-FIX5) -> remember "pending announce" (existing flag, Clipboard.cpp:303-307)
      else ServerFormatList{CF_UNICODETEXT}; serverClipboardActivity.start()  (echo window starts, :320-321)
client pastes (or our client eager-fetches) -> CB_FORMAT_DATA_REQUEST(CF_UNICODETEXT)
  Clipboard::onClientFormatDataRequest (main thread via postToMainThread, :115-132, :450-)
      not CF_UNICODETEXT or none announced -> FAIL (existing behaviour, :454-462)
      else emit hostTextRequested(requestId); start 3 s ServerDataTimeoutMs
  endpoint -> ClipboardRequest{requestId,maxBytes}
worker: HostClipboard reads mimeData->text() NOW (current clipboard), normalises, caps -> ClipboardData
broker: Clipboard::provideHostText(requestId, text):
      toWire (LF->CRLF, ClipboardText.h:35-45), UTF-16LE + NUL, ServerFormatDataResponse OK;
      lastServerText = toHost(text); serverClipboardActivity.start()  (echo rules, :475-476)
      non-Ok / timeout -> ServerFormatDataResponse FAIL
```

Why delayed: (1) policy is evaluated at paste time, not only at copy time; (2) the broker never stores a host copy
(unlike the eager snapshot in `PlasmaScreencastV1Session.cpp:453-462`, which copies the bytes of **every** MIME type,
images included); (3) the worker returns the clipboard as it is when pasted. Note that our own client fetches immediately
on every offer ("Eager fetch", `SessionModelClipboard.cpp:104-116`), so for it the transfer still happens right after the
copy; the saving is for stock clients and for denied states.

### 3.4 Flow: client to host

```
client copies -> CB_FORMAT_LIST (CF_UNICODETEXT [+CF_TEXT])                     [cliprdr thread -> main thread]
  Clipboard::onClientFormatList (existing: ack with FormatListResponse first, pick one format, :325-397)
      new gate: !policy.clientToHost -> ack, remember nothing, no request (replaces the isSignalConnected hack, :424)
      echo window / request-once / graphicsDelivered rules unchanged (:365-397)
  settle 250 ms -> FormatDataRequest(fmt) -> client answers -> onClientFormatDataResponse (:480-)
      new: reject dataLen > 2 MiB before copying (:152-160), decode, drop if text.size() > 1 MiB
      existing: empty text -> clear; equals lastServerText -> not written back (:521-531)
  emit clientDataChanged -> SessionHost glue -> ConsoleWorkerSession::setClipboardData(text)
  endpoint: backpressure check, rate bucket (2.5) -> ClipboardSet{setId, generation, text}
worker: mayApplyClipboardSet -> HostClipboard::write(text): flag on, setMimeData(text/plain;charset=utf-8,
        + x-kde-passwordManagerHint=secret when locked/greeter), flag off, lastSetText = text
        -> ClipboardSetResult{Written}
```

The existing glue point to replace is the stub (`ConsoleWorkerSession.cpp:86-91`). Both host controllers connect
`connection->clipboard()` to their session exactly as `SessionController.cpp:519-520` does, but gated by the planner and by
`ownsControl` (Console: in `addClient`, `ConsoleHostController.cpp:900-1010`; Virtual: in `VirtualSessionTransport`'s
constructor next to `:181-185`).

### 3.5 Echo and loop suppression

Three layers, all reusing what exists:

1. **Worker, own write:** a `writingSet` flag across `setMimeData` plus `lastSetText`, as in
   `PlasmaScreencastV1Session.cpp:384,439-451`. KGuiAddons emits `changed` synchronously inside `setMimeData`
   (comment `:376-383`), so the flag catches the immediate echo and `lastSetText` the later one.
2. **Broker, announce side:** a client format list within `EchoWindowMs` (2 s) of an announce or serve is taken for the
   host text coming back and is not requested (`Clipboard.h:67-71`, `Clipboard.cpp:93-104,376-385`), and a response equal to
   the last host text is not written back (`:529`). These rules are unchanged; they key on `serverClipboardActivity` and
   `lastServerText`, which delayed rendering still sets at announce and at serve.
3. **Client:** `ClipboardEcho` guards (`SessionModelClipboard.cpp:119-127,177-188`). No change.

Delayed rendering changes one detail: `lastServerText` is known only after the first serve. For an announce with no serve
yet, an echo of the *announcement itself* is possible only if the client re-offered immediately; the 2 s window already
covers that without needing the text.

### 3.6 State reset on handover, control change, policy change

New `Clipboard::resetHostSide()` (main thread): free the announcement, clear `lastServerText`, answer any pending client
data request with FAIL, drop `pendingServerAnnounce`, and if an announcement was on the client send an **empty format list**
(withdraws the offer; MS-RDPECLIP permits it and FreeRDP's `ServerFormatList` takes `numFormats = 0`; to be confirmed against
mstsc and wlfreerdp in spike S2). `clientRequestsDisabled` is deliberately **not** reset: a stock client that failed once
stays unasked (`Clipboard.cpp:403-413`). Called on: worker disconnect/replace, control change away from this connection,
and policy `hostToClient` true -> false. `resetClientSide()` drops a half-received client copy (`clientData`) and cancels
`outstandingFormat`; called on worker replace and policy `clientToHost` true -> false.

### 3.7 Concurrency

- CLIPRDR PDU callbacks run on FreeRDP's cliprdr thread and only copy the PDU and queue to the main thread
  (`Clipboard.cpp:106-168`, from `ce4b7a82`/AUD-P5, which replaced a `BlockingQueuedConnection` deadlock). The bridge adds
  **no new thread and no new cross-thread call**: worker records arrive on the endpoint's `QLocalSocket` on the main thread,
  and `provideHostText`/`announceHostText` run on the main thread and write PDUs under `sendMutex` exactly like
  `sendServerData` (`:296-322`).
- `Clipboard::close()` sets `closing` so handlers queued before it are dropped (`:75-77,115-132`); the new handlers check
  `closing` and the endpoint pointer (QPointer) before touching either side.
- The worker's `KSystemClipboard` runs in the worker's GUI thread; `HostClipboard` is single-threaded and never blocks:
  `mimeData()` reads on Wayland data-control are asynchronous inside KGuiAddons (see 5.2 for the one place a read can stall).

## 4. Formats

### 4.1 Offered and accepted

| Direction | Wire format | Rule |
|---|---|---|
| host to client | CF_UNICODETEXT only | The only format ever offered (`Clipboard.cpp:296-322`) and the only one served; any other request gets FAIL (`:454-462`). Kept: CF_TEXT is not offered (every current client takes Unicode). |
| client to host | CF_UNICODETEXT preferred, CF_TEXT, CF_OEMTEXT | One request per list, preference order as today (`:332-355`). CF_TEXT/CF_OEMTEXT decoded as Latin-1 (`:509-516`), a known limitation (code pages are not honoured), kept. |

Line endings: wire CRLF <-> host LF through `ClipboardText::toWire/toHost` (`src/ClipboardText.h:22-45`), unit-tested
(`autotests/ClipboardTextTest.cpp`). The worker wire carries LF UTF-8 only; the conversion stays in the broker so it has one
home. A lone CR is untouched in both directions (`ClipboardText.h:11-13`).

### 4.2 What "text" means on the host

The worker decides "has text" with `QMimeData::hasText()` and reads with `text()`. Today's code does the same
(`PlasmaScreencastV1Session.cpp:443-449`, `Clipboard.cpp:465`), so behaviour is not changing, but the cases need stating:

- `text/plain;charset=utf-8` and `text/plain`: `hasText()`/`text()` handle both (Qt maps `text/plain` and charset variants).
  This is what Kate, Konsole, KWrite, Chrome's page copy and Firefox offer.
- **Chrome omnibox copy of a URL:** Chrome offers `text/plain` and `text/plain;charset=utf-8` (so it works) and often also
  `text/html`/`text/uri-list`. Not examined in code today (NOTES "unexamined"); spike S4 captures the real MIME list on Sol
  with `wl-paste -l`-equivalent (Sol lacks `wl-clipboard`, NOTES harness line; use the worker's own debug log of formats,
  `PlasmaScreencastV1Session.cpp:463`). **[Jev]** classification of each captured MIME list: text usable / URL only / none.
- `text/uri-list` **only** (a file manager copy, no `text/plain`): `hasText()` is false, so nothing is offered, as today.
  Proposed: if `hasUrls()` and no text, offer the URLs joined by LF as text (`file:///...` for local files). Off by default
  in phase 1 (Q5); simple to add because it is inside `HostClipboard::currentText()`.
- Images and `text/html` alone: ignored in phase 1, no offer (this is a change from the current "announce CF_UNICODETEXT
  whatever the data is", `Clipboard.cpp:296-322`, which promised text it then served as an empty string).

Empty text: an offer is never sent for empty text; a client copy that is empty clears the host clipboard only when the host
text is not empty (existing behaviour `Clipboard.cpp:521-525` always clears; keep, it mirrors "client cleared its clipboard").

## 5. Wayland host behaviour

### 5.1 What exists and what is reused

`PlasmaScreencastV1Session` implements both halves: change detection via `KSystemClipboard::changed` with the
`monitorIndex() != 0`, mode and echo filters (`src/PlasmaScreencastV1Session.cpp:421-465`) and the write
(`:1253-1267`). `PortalSession` has a twin (`src/PortalSession.cpp:150-180,285-298`). `KSystemClipboard` (KGuiAddons) speaks the
compositor's data-control protocol directly, so there is **no Klipper D-Bus dependency, no window, no focus and no input
serial** requirement for either read or write (the NOTES' "Klipper D-Bus" was only the test harness, NOTES/HANDOFF F1 harness
line 1130). Klipper, when present in the session, observes the same selection and may re-own it after a paste; that is
the source of the "Klipper re-owning what the client just pasted" echo already handled (`Clipboard.cpp:90-97`).

It **cannot be reused unchanged in the worker**:

1. The worker has many `PlasmaScreencastV1Session` objects (the main one `consoleworker.cpp:3533`, per-monitor sessions
   `:3535`, creator/bootstrap sessions `:954,1246,1598,2565,2936`). Every one connects its own `KSystemClipboard::changed`
   handler in its constructor (`PlasmaScreencastV1Session.cpp:421`) and all default to `monitorIndex() == 0`
   (`src/AbstractSession.cpp:214`). Nothing is connected to `clipboardDataChanged` in the worker today, but each handler
   still runs `mimeData()` and **copies the bytes of every MIME type** (`:453-462`) on every host copy, per session
   object, and logs "Announcing ..." (`:463`), which misled the investigation (NOTES point 1). Wasteful on Console now
   (an image copy is copied once per session object), and a duplicate-announce hazard the moment anything connects.
2. It couples clipboard to the capture session lifetime (sessions come and go with Fit/Replace/creators), while a
   clipboard must follow the worker/greeter lifetime.
3. It is eager, which delayed rendering must not be.

**Proposal:** extract `HostClipboard` (`src/HostClipboard.{h,cpp}`, a plain `QObject` owning the `KSystemClipboard`
connection, echo flag, `lastSetText`, size cap, policy gates). The worker owns exactly one. `PlasmaScreencastV1Session` gets
`setClipboardBridgeEnabled(bool)` (default true to keep the retired route and the plasmastreamer harness unchanged); the worker
sets it false on every session it creates. Dedupe of the legacy path onto `HostClipboard` is optional later work.

### 5.2 Per-state facts to verify in spikes (not assumed)

- S1. Does the SDDM greeter's compositor expose a data-control protocol to the `sddm`-user worker? If not, greeter
  `clientToHost` is silently unavailable and the planner reports `greeter: no data-control` in the capability push (7.4).
  Console must still stream the greeter; clipboard is additive and never blocks video.
- A locked session's `plasmashell`/Klipper may be stopped or slow; a `KSystemClipboard::mimeData()` read of a selection
  owned by a hung client can stall. `HostClipboard` reads only on `ClipboardRequest` and answers `Gone` after 2 s (the
  broker's own 3 s timeout covers the wire). Chrome and Qt apps answer data-control reads promptly in practice; to be measured
  in the live matrix with a deliberately stuck source.
- Primary selection is ignored (`:430`).

### 5.2a Measured on a private KWin 6.6.6 on Sol, 2026-10-15 (OPT-072; evidence `~/dev/rdp/evidence/2026-10-15-image-wayland-verify/`)

- S1. KWin 6.6.6 advertises `ext_data_control_manager_v1` (v1) only, no `zwlr_data_control_manager_v1`, and does not restrict
  it (no desktop-file grant needed). KSystemClipboard therefore picks its `WaylandClipboard` (ext) class. The SDDM greeter's KWin
  could not be inspected (not running while a session is logged in); the worker now logs `Clipboard: host backend <class>` once.
- A plain `QMimeData::setData("image/png", bytes)` handed to `KSystemClipboard::setMimeData` is read as an EMPTY picture by
  every other process (KF6 6.24: the data-control source writes `imageData()` for image/png when `hasImage()`); only the
  writing process sees the bytes. `server/PngMimeData.h` offers image/png plus a lazily decoded application/x-qt-image and fixes
  it. Because the picture is then re-encoded by Qt on its way out, the echo check compares pixels (`ClipboardImage::samePixels`).
- Klipper (inside plasmashell; `IgnoreImages` defaults to true): a picture is not stored in history, the writer keeps the
  selection while it runs, and when the selection ends Klipper restores the last text. With `IgnoreImages=false` a picture
  is stored in `history3.sqlite` unless it carries `x-kde-passwordManagerHint` (then nothing is stored; same for text).
  Observed, cause not isolated: a Qt app (wl_data_device source) that copied an image/png-only selection while Klipper held
  history had it replaced by the last history text within the same millisecond; a data-control source (the worker) with
  the same content, with or without `application/x-qt-image`, was left alone.
- Chrome 155 (Wayland) "Copy image" offers `text/html` (an `<img src>` tag), `image/png`, `chromium/x-internal-source-rfh-token`,
  `chromium/x-source-url`. Firefox 157 offers `text/html`, `text/_moz_htmlinfo`, `text/_moz_htmlcontext`, `image/png` and
  about twenty more image formats (avif, bmp, x-bmp, x-MS-bmp, ico variants, jpeg, tiff, webp), `SAVE_TARGETS`.
- The worker reads a pasted picture with `SelectionReader` (own ext_data_control connection, 16 MiB cap for PNG, 64 MiB for
  other formats, abandons the source beyond that) because KSystemClipboard reads an offer to its end whatever its size.

## 6. Client side

### 6.1 Changes

None required for the first release. Our client already handles server format lists, requests, data and refusals
(`krdp-client/src/rdp/ClipboardChannel.cpp:214-262`, `SessionModelClipboard.cpp:104-197`), enforces a 1 MiB cap both ways
(`:24,152,169`), and has echo guards (`:119-127,177-188`).

### 6.2 What the client must tolerate (already true, listed as test targets)

- A format list that arrives late, after the client's own copy (echo rules above).
- A format list followed by an empty one (the withdrawal in 3.6): `onServerOffered` with no text formats is a no-op
  (`SessionModelClipboard.cpp:111-114`).
- A refused request (`CB_RESPONSE_FAIL`) and an empty response: `onServerData` returns on empty/non-text
  (`:165-176`). The 3 s `ServerDataTimeoutMs` is shorter than the client's own 5 s local timeout (`ClipboardChannel.cpp:25`).
- Offers that stop arriving in greeter/foreign-locked states: the client sees no clipboard traffic, no error.

### 6.3 Stock clients

- Request once per list, never while a request is outstanding, nothing before the first acknowledged frame (15 s fallback),
  after one refused or unanswered request the client is not asked again this session: all unchanged and kept
  (`Clipboard.cpp:76-100,186-191,403-413`). They exist because wlfreerdp3 3.22 reads its Wayland clipboard synchronously
  under a lock its event loop needs (`autotests/ClipboardStallLoopbackTest.cpp:4-23`; HANDOFF 2026-09-28 AUD-FIX5/FIX6).
- The `isSignalConnected` guard that today makes the console host never request (`Clipboard.cpp:424-429`, and the loopback
  test "no clipboard consumer: no request at all", `ClipboardStallLoopbackTest.cpp:273-287`) becomes an explicit
  `setClientToHostAllowed(bool)`. The test keeps its meaning with the planner returning deny.
- Policy-denied states must not wake a stock client's clipboard at all: in deny, `onClientFormatList` acks and returns.
- The host-to-client announce on a stock client stays behind `graphicsDelivered` (`:296-308`).

## 7. Settings, UI, capability, logging

### 7.1 Setting

`ClipboardDirection`, a **broker host key per route** (Console and Virtual each, like `StandardClientMedia`), values
`both` | `host-to-client` | `client-to-host` | `off`, default `both`.

Why a host key and not the per-user kcfg: broker host settings are per route and come from the unit
(`server/BrokerHostSettings.cpp:14-30` field table with env name and default; boolean/enum validation `:182,226`;
runtime read `server/BrokerHostRuntime.cpp:15,85`; service `Environment=`/`ExecStart` in
`server/krdp-console-host.service.in` and `krdp-virtual-host.service.in`; argument parsing `server/consolehost.cpp:65-123`,
`server/virtualhost.cpp:46-67`). `krdpserversettings.kcfg` is the legacy per-user schema with a pinned key set and digest
(spec 2026-10-09 section 9.1 notes `server/LegacySettingsMigration.cpp:113-127`); no kcfg change.

Default `both` rationale: the feature is expected, the planner already narrows it by state (2.2), and the client has its own
per-connection switch. Administrators who want a one-way policy (for example "client to host only" on a shared box) have
it. It never widens what 2.2 denies: the effective policy is `planner AND setting`.

Interaction with the client flag: the client's `clipboard=false` means no cliprdr channel, so no `Clipboard` is initialised
(`RdpConnection.cpp:1693-1696`); the bridge is simply idle. A *server* `off` means the broker still lets the channel join
(a stock client expects it) but acknowledges lists and answers every request with FAIL and announces nothing.

### 7.2 Code placement of the policy

Pure header `server/ConsoleClipboardPolicy.h` (planner, enum reason, no Qt GUI), shared by Console and Virtual hosts, with
`ConsoleClipboardPolicyTest`. Inputs come from existing state: `ConsoleHandoff::targetFor` / `ConsoleSeat::find`
(`ConsoleHostController.cpp:688-692`), `ownsControl`, `client.uid`.

### 7.3 KCM

Console and Virtual route pages, "Sharing" group (next to the camera/microphone rows), one row "Clipboard": Both ways /
Host to client only / Client to host only / Off, with a Use-default action and help text: "Copies plain text between your
computer and the host. Nothing is shared on the login screen except pasting into it, and nothing is shared with another
account's locked screen. Takes effect when the service restarts." Add through the data-driven `add()` the 2026-10-09 spec cites
(as spec 2026-10-09 9.2), follow `docs/settings-ui.md`, tests: `KcmSettingsTest`,
`BrokerHostsPageTest`, `BrokerHostSettingsModelTest` field definitions, and the helper's validation reuse
(`server/hostsettingshelper.cpp`). Standing requirement (OPT-043/053): UI, validation/help/defaults, tests, packaging
(unit files). The public snapshot shows the key automatically (spec 2026-10-09 9.1).

### 7.4 Capability

Not needed for correctness: the client speaks plain CLIPRDR. Additive and optional in phase 1b: the KRDPCTL `capabilities`
record (`ConsoleHostController.cpp:2075-2110`) gains `clipboard: {hostToClient, clientToHost, maxBytes}` reflecting the
effective setting plus current state, with a `clipboardState` push on change (same shape as the OPT-058 availability push,
`:869-874`) so our client can show "Clipboard is off on the login screen" instead of silence. Old clients ignore the group.
Recommendation: ship the server without it, add it with the next client release (Q6).

### 7.5 Logging and stats

- Info, once per edge, no data: `Clipboard: host to client enabled|disabled (reason=<enum>)`, `... client to host ...`,
  `Clipboard: refused (oversize|rate|policy|busy|timeout) direction=<d> bytes=<n>` (one per episode, not per event).
- One summary line at connection close: offers, requests served, requests refused by reason, sets written, sets refused,
  bytes each way. Counters live next to the connection's other stats; not added to the stats panel in phase 1.
- Debug lines keep today's discipline (lengths and format ids only). The worker uses the same categories; remove the
  misleading "Announcing system clipboard change to the client" text from the worker path (`PlasmaScreencastV1Session.cpp:463`
  stays only for sessions with the bridge enabled).
- The 15 s fallback line (`Clipboard.cpp:188`) and the refusal line (`:408-410`) are already Info and stay.

## 8. Test plan

### 8.1 Pure unit tests (no display)

- `ConsoleWorkerWireTest`: encode/decode round-trip of all six records, truncation, trailing bytes, oversize text,
  `status` out of range, version 16 pin and stale-version rejection (existing pattern `:745-746`), `LastKind`.
- `ConsoleClipboardPolicyTest`: the full table in 2.2 (greeter, locked own/foreign, unlocked, viewer, virtual, None) crossed
  with the four `ClipboardDirection` values; reasons; policy re-evaluation on each input change.
- `mayApplyClipboardSet` / `mayAnswerClipboardRequest` gates: stale generation, inactive control, greeter flag mismatch,
  repeated `setId`.
- `ClipboardTextTest` additions: oversize cap helper, URL join (if Q5 yes), NUL handling.
- Log hygiene: run the host and a fake worker with a canary string as clipboard text at Info and Debug, assert the canary is in
  no captured message (**[Jev]** only for classifying unexpected extra lines; the canary check itself is a plain grep).

### 8.2 Broker-level test with a fake worker

Extend `ConsoleWorkerEndpointTest` / `VirtualSessionTransportTest` style: a fake worker socket speaking v16 plus
`ClipboardStallLoopbackTest` (real `krdpctl-probe --clipboard` client) driving: offer then request then data; stale `seq`;
`Gone`/`TooLarge`/`Denied`; broker timeout (3 s) -> FAIL; worker disconnect mid-request -> FAIL and withdrawal; handover
(worker replace) clears the announcement; viewer connection never announces or requests; a v15 hello gives `versionMismatch`
and no clipboard; rate bucket; backpressure refusal; client oversized response rejected before allocation. All the existing
loopback cases keep passing (stock-client rules). `ClipboardHandoffTest` (`autotests/CMakeLists.txt:650-655`) covers the
cliprdr-thread hand-off and must still pass.

### 8.3 Live matrix (Sol server, Buzz client; never Hal)

Each cell: result, journal lines (Info), no secrets left in Klipper history (S3).

| # | Case | Expected |
|---|---|---|
| 1 | Sol user session unlocked, copy in Kate, paste on Buzz | works, our client and `wlfreerdp3` |
| 2 | Buzz copy, paste in Sol Kate | works both clients (stock: first request may be refused, then never asked; host to client still works) |
| 3 | Large text 900 KB / 2 MB | 900 KB works; 2 MB refused, one Info line, no stall |
| 4 | Binary garbage / lone surrogates / NUL / CR-only | no crash, text sanitised, no stall |
| 5 | Rapid changes (50 copies in 5 s) | coalesced, <= 1 format list per 250 ms, final value correct, no storm |
| 6 | Lock Sol mid-copy, same user reconnects | locked, own uid: both directions work, secret hint set (S3) |
| 7 | Locked, a second PAM user connects | both directions denied, Info line `reason=foreign-locked` |
| 8 | SDDM greeter: paste a username into the greeter field | works if S1 passes; host to client denied |
| 9 | Greeter to login handover with text announced before | client holds nothing of the greeter; new session clipboard is empty to the client |
| 10 | Disconnect/reconnect | no stale offer; first copy works |
| 11 | Two viewers (controller + viewer) | only the controller's channel carries data; control swap resets |
| 12 | Virtual :3395 session, copy both ways, attach/detach | works; detach clears the announcement |
| 13 | v15 worker with v16 broker (staged mismatch on Sol only) | `versionMismatch`, backoff, no loop, no video until the pair is fixed |
| 14 | Stuck host clipboard owner | `Gone` after 2 s, client gets FAIL, desktop unaffected |
| 15 | Chrome omnibox, Chrome page, Konsole, Firefox | S4 capture, **[Jev]** classify usable/not per MIME list |

**[Jev]** also: severity-triage of the filtered journal from the matrix runs (each line into expected / warning / bug).

### 8.4 What may not run on Hal

Nothing in 8.3. No daemon-starting test, no connection, no screenshot, no Klipper or clipboard write on Hal's session
(HARD RULE, `~/dev/rdp/CLAUDE.md`). Unit tests that start no daemon may run there. Hal gets the package only through the
gated rollout (Sol -> others -> Hal, zero established connections), and Steve does the hands-on clipboard check.

## 9. Rollout

- **Server-only paired change** (broker and worker, wire 16) for both routes; no client release needed, no KRDPCTL change in
  phase 1. Package `farside-server` carries both Console and Virtual units, so Console and Virtual cannot mix versions.
- **Migration:** none for settings (new key with a default). Existing sessions: a Virtual desktop's worker is restarted by
  its supervisor when the broker returns (`scripts/virtual-session-desktop.sh:81-95`); the desktop itself is not restarted.
  A running worker of the old version meeting the new broker is the v15/v16 mismatch case: it is replaced, which Console
  already does on package restart (the note "install the paired broker and worker together" exists for this).
- **Order:** candidate deb, Sol first, matrix 8.3, then cray/marvin/ace/Buzz, Hal last with zero connections (existing
  gated rollout). Keep the previous deb for rollback.
- **Rollback:** reinstall the previous `farside-server` deb (pair); no data or setting migration to undo; the new
  `ClipboardDirection` key is ignored by the old build. Kill switch without a rollback: set `ClipboardDirection=off` on the
  route and restart that service (when nobody is connected).

## 10. Open questions for Steve (with recommendations)

1. **Default direction.** `both`, narrowed automatically by state? Recommend yes. Alternative: default `client-to-host`
   only (smaller exposure) and let the admin opt in to the other way.
2. **Greeter host to client.** Deny (recommended). The greeter is the sddm user's compositor; there is nothing legitimate to
   read and any authenticated user can reach it.
3. **Locked screen, same user.** Allow both ways for the account that authenticated (recommended; needed to paste a
   password into the lock screen). Alternative: client to host only while locked.
4. **Controller change.** Reset and show the new controller nothing until the next copy (recommended), versus offering the
   current host clipboard to the new controller.
5. **File-manager copies (`text/uri-list` only).** Offer URLs as text in phase 1? Recommend no; do it with phase 2
   (files), where it belongs, to avoid paths leaking as text.
6. **Capability push to the client now or later.** Recommend later, with the next client release (section 7.4); phase 1 is
   server-only and silent in denied states.
7. **Secret hint on locked/greeter writes** (`x-kde-passwordManagerHint=secret`, keeps pasted passwords out of Klipper
   history). Recommend yes, if spike S3 shows Klipper honours it.
8. **Per-user restriction preference** in the user's own settings (a user may only tighten). Recommend not in v1; the
   host key plus the client's per-connection flag are enough, and the per-user key set is pinned (spec 2026-10-09 9.1).
9. **Size limit.** 1 MiB of UTF-8 (matches our client). A larger cap costs memory in the system broker. Recommend keep 1 MiB.
10. **Dedupe the legacy single-process clipboard onto `HostClipboard`.** Recommend later and only if the :3389 route code
    is not deleted first.

## 11. Phased implementation plan

Each task is sized for one Sonnet agent with a Definition of Done in the brief; `[Jev]` marks a bounded judgment step that is
preceded by an evidence-gathering step and offloaded.

**Phase 0 - spikes (read-only on Sol, Sol's owner Steve OK for connect-only; nothing on Hal)**
- S1. Greeter data-control availability for the `sddm` worker (journal/`ps`, a one-line probe in a disposable worker run on Sol).
- S2. Empty `CLIPRDR_FORMAT_LIST` withdrawal against `krdpctl-probe` and wlfreerdp3.
- S3. Klipper `x-kde-passwordManagerHint` honoured; history check on Sol.
- S4. Capture MIME lists for Chrome omnibox/page, Konsole, Kate, Firefox. **[Jev]** classify each list (usable text / URL only / none).

**Phase 1 - pure code, no display** (1a and 1b in parallel)
- 1a. Wire v16 records, gates, tests; bump `ProtocolVersion`, update `ConsoleWorkerWireTest` pins, `ConsoleWorkerOutbox` ordering rules
  and the version comment block (`ConsoleWorkerWire.h:1-63`).
- 1b. `ConsoleClipboardPolicy.h` + test; `Clipboard` extensions as pure state machine pieces (rate bucket, size caps,
  delayed-source API `announceHostText/hostTextRequested/provideHostText`, `resetHostSide/resetClientSide`,
  `setClientToHostAllowed/HostToClientAllowed`) with `ClipboardStallLoopbackTest` and `ClipboardHandoffTest` still green.

**Phase 2 - worker** : `HostClipboard`, bridge disable switch on `PlasmaScreencastV1Session`, worker record handling, secret hint,
`Gone` timeout. Unit-test with a fake `KSystemClipboard` seam; no live daemon.

**Phase 3 - broker** : `ConsoleWorkerEndpoint` dispatch, backpressure and per-endpoint state; `ConsoleWorkerSession`
implementation of `setClipboardData` and the offer signal; wiring in `ConsoleHostController::addClient` (planner + `ownsControl`)
and `VirtualSessionTransport`; reset hooks on worker disconnect/control change; Info logging and close summary.
Broker test with fake worker (8.2).

**Phase 4 - settings/KCM/packaging** : `ClipboardDirection` host key (table, env, args, units, helper validation, snapshot),
KCM row, tests, `docs/settings-ui.md` entry, packaging scripts check.

**Phase 5 - live (Sol/Buzz)** : matrix 8.3; **[Jev]** triage of filtered journal lines; evidence under
`~/dev/rdp/evidence/<date>-clipboard-bridge/`.

**Phase 6 - rollout** : candidate deb, gated rollout, HANDOFF Log and CLAUDE.md "Fixed" lines, `research.md` OPT entry.

**Later phases (separate designs):** 1b capability push + client notice; images (CF_DIB/PNG) with a larger cap and lazy fetch
on paste; HTML; files and drag and drop (needs `streamFileClipEnabled`, locking, and a path policy).

## Addendum OPT-077 (2026-10-17): formatted text (HTML), capability and failure review

Status: implemented on branch `html-clipboard`, not merged. Full design notes and the findings table are in
`~/dev/krdp/.work/html-clipboard/PLAN.md`; this is the part that belongs with the bridge design.

**Formats.** cliprdr "HTML Format" (registered name; our id 0xC002) <-> Wayland `text/html`. A rich copy is
`CF_UNICODETEXT` + "HTML Format" in one format list and ONE host offer (`text/plain` + `text/html`). The wrapper is the MS
"HTML Clipboard Format": `Version:0.9`, `StartHTML`, `EndHTML`, `StartFragment`, `EndFragment`, ten-digit offsets, CRLF,
UTF-8, NUL terminated, 105-byte header (same as FreeRDP's `clipboard_synthesize_ms_html_format`). The unwrapper takes the
StartHTML..EndHTML range and is more tolerant than FreeRDP's (see PLAN.md for the list) but refuses inconsistent offsets and
over-length input. Pure code: `src/ClipboardHtml.h`, tested by `HtmlClipboardTest`.

**Combination rule** (formats only, never contents): rich = HTML and (text or no picture). HTML + picture and no text (a
browser's "Copy image") is the picture. Without HTML the OPT-072 rule stands.

**Limits.** 8 MiB HTML (document), 8 MiB + 4 KiB on the cliprdr wire, text beside it 1 MiB, rate limit shared. **Wire** 19 ->
20: `ClipboardKind::Html`, Offer flag 4, Set = `[u32 text length][text][html]`. **Capability** `clipboard.html` + `maxHtmlBytes`.

**RTF is a deliberate non-goal.** LibreOffice offers `text/rtf` as well as `text/html`; the HTML carries bold/italic/colour/tables
and one more converter would be one more parser of untrusted input. Revisit only if a real application needs it.

**Stock clients.** mstsc asks for registered formats by the ids in our list and reads the 105-byte header; FreeRDP clients
(wlfreerdp3 3.22) keep the AUD-FIX6 rules (a refused request disables later requests; a refused HTML request after a
delivered text does not). Our server advertises only `CB_USE_LONG_FORMAT_NAMES` (files are not implemented), as does our client.
Not exercised against real mstsc or FreeRDP in this slice.

**Failure responses.** Every refusal of a host paste ends in `CB_RESPONSE_FAIL` at once (not announced, policy, busy, worker
refusal, oversize, wrapper refusal, deny while pending) or after the 3 s timer (no worker answer). Fixed in OPT-077: a worker
answer of the wrong kind now fails the paste immediately; a failure response no longer carries a NULL data pointer.
