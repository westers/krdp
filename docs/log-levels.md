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
