#!/usr/bin/env python3
# OPT-078 spec 6.4 acceptance numbers from krdpctl-probe output (`reply: {...}` lines of stats-sample records).
# usage: uv run --no-project python analyze-stats.py RUN.log [BASELINE.log]
# Prints one table; with a baseline also the pass/fail of the video and audio criteria.
import json, sys, statistics


def samples(path):
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            i = line.find("{")
            if i < 0 or '"stats-sample"' not in line:
                continue
            try:
                rec = json.loads(line[i:])
            except ValueError:
                continue
            if rec.get("type") == "stats-sample":
                out.append(rec)
    return out


def summarize(recs):
    if not recs:
        return None
    seconds = sum(r.get("intervalMs", 1000) for r in recs) / 1000.0
    frames = sum(r["video"].get("framesSent", 0) for r in recs)
    coalesced = sum(r["video"].get("coalesced", 0) for r in recs)
    delay = [r["video"]["sendDelayMs"] for r in recs if "sendDelayMs" in r["video"]]
    queue = [r["link"]["sendQueueAtSendKiB"] for r in recs if "sendQueueAtSendKiB" in r["link"]]
    audio = [r["audio"] for r in recs if "audio" in r]
    return {
        "samples": len(recs),
        "seconds": round(seconds, 1),
        "fps": round(frames / seconds, 2) if seconds else 0,
        "coalescedPerS": round(coalesced / seconds, 2) if seconds else 0,
        # sample p99s are bucket upper bounds; the worst sample's p99 is the conservative run p99
        "frameDelayP50ms": statistics.median([d["p50"] for d in delay]) if delay else None,
        "frameDelayP99ms": max((d["p99"] for d in delay), default=None),
        "frameDelayMaxMs": max((d["max"] for d in delay), default=None),
        "sendQueueP99KiB": max((q["p99"] for q in queue), default=None),
        "sendQueueMaxKiB": max((q["max"] for q in queue), default=None),
        "audioPackets": sum(a["packets"] for a in audio) if audio else None,
        "audioLate": sum(a["late"] for a in audio) if audio else None,
        "audioOverflows": sum(a["overflows"] for a in audio) if audio else None,
        "audioProducerGaps": sum(a["producerGaps"] for a in audio) if audio else None,
        "audioMaxGapMs": max((a["maxGapMs"] for a in audio), default=None),
        "rttMs": statistics.median([r["link"]["tcpRttMs"] for r in recs if "tcpRttMs" in r["link"]] or [0]),
        "retransmits": sum(r["link"].get("retransmits", 0) for r in recs),
    }


def verdict(run, base):
    checks = []
    if base["fps"]:
        ok = run["fps"] >= base["fps"] * 0.9
        checks.append(("video framesSent within 10% of baseline", ok, f'{run["fps"]} vs {base["fps"]}'))
    if run["sendQueueP99KiB"] is not None:
        checks.append(("p99 send-queue at send <= 256 KiB", run["sendQueueP99KiB"] <= 256, f'{run["sendQueueP99KiB"]} KiB'))
    if run["frameDelayP99ms"] is not None and base["frameDelayP99ms"] is not None:
        added = run["frameDelayP99ms"] - base["frameDelayP99ms"]
        checks.append(("p99 added frame delay <= 40 ms over baseline", added <= 40, f'{added} ms ({run["frameDelayP99ms"]} vs {base["frameDelayP99ms"]})'))
    if run["audioLate"] is not None and base["audioLate"] is not None:
        bad = run["audioLate"] + run["audioOverflows"] + run["audioProducerGaps"]
        ref = base["audioLate"] + base["audioOverflows"] + base["audioProducerGaps"]
        checks.append(("audio late+overflow+gaps <= baseline + 2", bad <= ref + 2, f"{bad} vs {ref}"))
        checks.append(("audio gap <= 100 ms", (run["audioMaxGapMs"] or 0) <= 100, f'{run["audioMaxGapMs"]} ms'))
    return checks


def main():
    run = summarize(samples(sys.argv[1]))
    if run is None:
        print("no stats-sample records found")
        return 2
    for k, v in run.items():
        print(f"{k:22} {v}")
    if len(sys.argv) > 2:
        base = summarize(samples(sys.argv[2]))
        if base is None:
            print("baseline has no stats-sample records")
            return 2
        print()
        failed = 0
        for name, ok, detail in verdict(run, base):
            print(f'{"PASS" if ok else "FAIL"}  {name}: {detail}')
            failed += not ok
        return 1 if failed else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
