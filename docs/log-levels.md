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
