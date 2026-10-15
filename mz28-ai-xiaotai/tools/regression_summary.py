"""Summarize C3 serial evidence per call; counters are not an audio/pass verdict."""
import argparse
import json
from pathlib import Path
import re


LINE = re.compile(r"\((\d+)\)\s+(voip|audio|memory): (.*)")
TOTAL = re.compile(
    r"TX packets=(\d+) bytes=(\d+) failed=(\d+) backlog_drop=(\d+) \| "
    r"RX packets=(\d+) bytes=(\d+) bad_format=(\d+)")
ALERT = re.compile(
    r"allocation failed|Guru Meditation|assert failed|Stack canary|CORRUPT HEAP|"
    r"TX FAILED|TX ASSET INVALID|RX FORMAT REJECTED|RTC error=|WHIP failed|"
    r"WHIP submit failed|reject request busy|REJECT failed|control queue full|"
    r"Task watchdog|Interrupt wdt", re.IGNORECASE)


def summarize(text):
    # Host markers can split an unfinished UART line. Remove only our markers,
    # including their added newline, and retain every original firmware byte.
    text = text.replace("\r\n", "\n")
    text = re.sub(r"\n\[HOST [^\n]*\] [^\n]*\n", "", text)
    calls = {}
    alerts = []
    current = None
    previous_ms = None
    segment = 0
    for line in text.splitlines():
        match = LINE.search(line)
        if ALERT.search(line):
            alerts.append({"segment": segment, "line": line})
        if not match:
            continue
        ms, tag, message = match.groups()
        ms = int(ms)
        # Logs from multiple boots must not merge equal call generations.
        if previous_ms is not None and ms + 1000 < previous_ms:
            segment += 1
            current = None
        previous_ms = ms
        generation = re.search(r"generation=(\d+)", message)
        if tag == "voip" and generation and int(generation[1]):
            key = (segment, int(generation[1]))
            current = calls.setdefault(key, {
                "segment": segment, "generation": key[1], "first_seen_ms": ms,
                "direction": "unknown", "last_state": "unknown", "signals": [],
                "media_start_ms": None, "active_ms": None, "resources_ready_ms": None,
                "active_memory_samples": [], "last_idle_memory": None,
                "audio_totals": None, "rx_max_gap_ms": None,
            })
        if current is None:
            continue
        current["last_seen_ms"] = ms
        if tag == "voip":
            if "DIALING:" in message:
                current["direction"] = "outgoing"
            elif "INCOMING:" in message:
                current["direction"] = "incoming"
            state = re.search(r"\bstate=([\w-]+)", message)
            flow = re.search(r"FLOW \| generation=\d+ [\w-]+->([\w-]+)", message)
            if state or flow:
                current["last_state"] = (flow or state)[1]
            if "SIGNAL |" in message:
                current["signals"].append({"ms": ms, "command":
                    re.search(r"cmd=(0x[0-9a-f]+)", message)[1]})
            if message.startswith("end:"):
                current["end"] = {"ms": ms, "detail": message}
            if "call resources ready:" in message:
                current["resources_ready_ms"] = ms
            memory = re.search(r"\bheap=(\d+) largest=(\d+) stack_hwm=(\d+)", message)
            if memory and state:
                sample = {"ms": ms, "free": int(memory[1]),
                          "largest": int(memory[2]), "stack_hwm": int(memory[3])}
                if state[1] == "active":
                    current["active_memory_samples"].append(sample)
                elif state[1] == "idle":
                    current["last_idle_memory"] = sample
        elif tag == "audio":
            if message.startswith("START |"):
                current["media_start_ms"] = ms
            stop = re.search(r"STOP \| active_ms=(\d+)", message)
            if stop:
                current["active_ms"] = int(stop[1])
            total = TOTAL.search(message)
            # Status before media start still contains the previous call totals.
            if total and current["media_start_ms"] is not None:
                current["audio_totals"] = dict(zip(
                    ("tx_packets", "tx_bytes", "tx_failed", "tx_drop", "rx_packets",
                     "rx_bytes", "rx_bad_format"), map(int, total.groups())))
            gap = re.search(r"max_gap=(\d+)ms", message)
            if gap and current["media_start_ms"] is not None:
                current["rx_max_gap_ms"] = max(current["rx_max_gap_ms"] or 0, int(gap[1]))
    result = list(calls.values())
    for call in result:
        samples = call["active_memory_samples"]
        call["active_memory_min"] = ({key: min(s[key] for s in samples)
                                      for key in ("free", "largest", "stack_hwm")}
                                     if samples else None)
    return {"notice": "Observed evidence only; not a hearing, weak-network or release verdict.",
            "calls": result, "alert_count": len(alerts), "alerts": alerts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = json.dumps(summarize(args.log.read_text(encoding="utf-8", errors="replace")),
                        ensure_ascii=False, indent=2)
    if args.output:
        with args.output.open("x", encoding="utf-8") as output:
            output.write(result + "\n")
    else:
        print(result)


if __name__ == "__main__":
    main()
