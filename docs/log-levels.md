# Log levels and gate runs

`farside.server` logs at Info and above by default (OPT-055). Repeating per-frame and per-tick lines
are debug-only: "Sending frame", "coalesced away", "Bandwidth measurement", "Adaptive quality ->",
"GFX channel reset" and similar.

Gate and acceptance runs whose scripts parse those lines (for example the F2 scripts `f2stats.py`
and `gentraces.py` under `~/dev/rdp/evidence/*/scripts/`) must enable debug on the test unit:

```
QT_LOGGING_RULES=farside.server.debug=true
```

Without it those scripts see no matching lines and report empty traces. Do not rely on debug lines
in production monitoring; use the Info lines (`Codec policy: ...`, `Reset graphics desktop ...`,
`Closing session`, `Client connection ended ...`).

## `session-end` reasons (KRDPCTL, unsolicited, sent just before the close)

- `encoder-failed`: the video encoder failed and could not be restarted (OPT-055 K4.2).
- `screencast-unavailable`: the compositor withheld `zkde_screencast_unstable_v1` (no matching
  `X-KDE-Wayland-Interfaces` grant for the executable, or a login greeter). One Warning line names the
  remedy; the session closes, the process and listener stay up. A missing `org_kde_kwin_fake_input`
  alone only disables remote input (one Warning line).

## Slow-link line (OPT-061)

`Video: the link is slow: slow link (TCP capacity at most N kbit/s in 65 % of 10 s (threshold T, S kbit/s sent); corroborated by ...)`
now ends with what corroborated it: "low for the last 3 intervals", "queueing delay N ms", "N retransmits in about M
segments" or "send queue growing in N of M intervals". A low capacity estimate without any of those (a nearly idle sender
measures about what it sends) declares nothing and logs nothing. Journal parsers that match the line's prefix are
unaffected.

## Quiet-by-default logging and how to switch the detail back on (2026-10-11)

Default journal output is state changes and actions only. Detail that was noisy during an AV1 connection is
off by default and opt-in. Set environment variables on the service (a drop-in `Environment=` line) and restart
it, or pass the Qt rules in `QT_LOGGING_RULES` (semicolon separated, several allowed).

| What you get back | How to enable |
|---|---|
| SVT-AV1 banner (about 20 `Svt[info]` lines per encoder open, plus 3 `Svt[warn]` lines) | `KPIPEWIRE_SVT_VERBOSE=1`, or set `SVT_LOG` yourself (1 errors, 2 + warnings, 3 + info; a preset `SVT_LOG` wins). Default is `SVT_LOG=1`: errors only. |
| Full effective encoder settings at open (rate control, qmin/qmax, every library option) | `QT_LOGGING_RULES=kpipewire_record_logging.debug=true` (the concise `Opened ...` Info line stays) |
| Settle-refresh burst summaries that did nothing | `kpipewire_record_logging.debug=true`. A burst that fired a refresh or was held back is still Info. |
| Settle-refresh per-frame trace | `KPIPEWIRE_SETTLE_TRACE=1` (Info, per frame; very verbose) |
| Experiment knobs for offline AV1 work | `KPIPEWIRE_SVTAV1_PRESET=<n>`, `KPIPEWIRE_SVTAV1_EXTRA_PARAMS=<svtav1-params>` (kept; used by `stalerepro`, no logging) |
| EGL config count, VAAPI display / DRM probe failures, "Hardware encoding is not supported" | `kpipewire_dmabuf_logging.debug=true`, `kpipewire_vaapi_logging.debug=true`, `kpipewire_record_logging.debug=true` |
| `Using PipeWire encoder for ...`, `Encoder chroma mode: ...` (start-up reports) | `farside.server.debug=true`. A real 4:4:4 to 4:2:0 fall-back stays Info; `Encoder backend: ...` is logged once per change. |
| Everything of the above | `QT_LOGGING_RULES="farside.server.debug=true;kpipewire*.debug=true"` plus `KPIPEWIRE_SVT_VERBOSE=1` |

Always kept at Info or above: codec switches, CPU guard and codec-policy decisions, `Opened ...` summary,
`Encoder running`, settle refreshes that act, filter-queue drops, renegotiation, encoder failures and all warnings
about real problems.

## Client quality lines (OPT-066)

One Info line per real state change, none per frame. Grep the journal of the console worker/host service (Sol: `journalctl -u krdp-console-host -u krdp-console-worker@*` or `-t krdp-console-worker`):

| Pattern | Meaning |
|---|---|
| `Console client N KRDPCTL: codec asked ... quality asked 65, pending 75` | the request and its first answer (`applied` for the first request of a connection, `pending` for a re-send, which settles 500 ms and is spaced 2 s) |
| `... quality host default (75)` | a codec re-send without `quality`: the host default applies (it is not "keep the last one") |
| `Client quality applied: requested 65, cap 65, live 65 (controller, applied)` | the settle timer applied the pending request: requested value, the cap the connection runs under, the live (adaptive) value, and the `qualityEffect` the client now sees (`applied` or `ignoredBitrateMode`). The client gets the same result as an unsolicited `codec` record. |
| `Quality decision: hevc_nvenc quality 65 qp 22 (running 18): <reason>` | the encoder's decision (KPipeWire, one per decision): `reopened the encoder`, `applied in place`, `deferred: encoder younger than 10000 ms`, `not applied: moved less than 2`, `ignored: running at a target bitrate` |

The KPipeWire `kpipewire_record_logging` category now defaults to Info (it was Warning, which hid its Info lines such as the
quality decisions, `Opened ...` and the queue-drain lines). Debug stays opt-in (`kpipewire_record_logging.debug=true`).
