"""Replay a recorded scenario into a running backend, for demonstration.

    python -m wadiwatch_backend.replay                  one scenario minute per second
    python -m wadiwatch_backend.replay --speed 120      twice as fast
    python -m wadiwatch_backend.replay --instant        everything at once, as an event that just ended

A scenario is a file of gateway messages, one per line, exactly as the
contract defines them. Only their timestamps are rewritten: the gaps between
messages are kept (divided by the speed), and the times are moved to now so
the dashboard treats the event as current.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass

from .backend import DUPLICATE, REJECTED, STORED
from .site import BACKEND_DIR

DEFAULT_SCENARIO = BACKEND_DIR / "scenarios" / "flood_demo.jsonl"
DEFAULT_URL = "http://127.0.0.1:8000"
# Scenario seconds per real second.
DEFAULT_SPEED = 60.0


@dataclass
class ReplaySummary:
    stored: int = 0
    duplicate: int = 0
    rejected: int = 0


def load_scenario(path):
    with open(path, encoding="utf-8") as lines:
        return [json.loads(line) for line in lines if line.strip()]


def replay(
    scenario,
    send,
    *,
    speed=DEFAULT_SPEED,
    instant=False,
    clock=time.time,
    sleep=time.sleep,
    report=lambda offset, message, status: None,
):
    """Send every message through `send(payload) -> status` and count the outcomes."""
    summary = ReplaySummary()
    if not scenario:
        return summary
    first_ts = scenario[0]["ts"]
    duration = scenario[-1]["ts"] - first_ts
    started = clock()
    for message in scenario:
        offset = message["ts"] - first_ts
        if instant:
            ts = int(started) - duration + offset
        else:
            wait = started + offset / speed - clock()
            if wait > 0:
                sleep(wait)
            ts = int(clock())
        status = send(json.dumps(dict(message, ts=ts), separators=(",", ":")))
        if status == STORED:
            summary.stored += 1
        elif status == DUPLICATE:
            summary.duplicate += 1
        else:
            summary.rejected += 1
        report(offset, message, status)
    return summary


def http_sender(base_url):
    """A `send` that posts to the backend's HTTP ingest endpoint."""
    url = base_url.rstrip("/") + "/api/ingest"

    def send(payload):
        request = urllib.request.Request(
            url,
            data=payload.encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return json.load(response)["status"]
        except urllib.error.HTTPError as error:
            if error.code == 400:
                return REJECTED
            raise

    return send


def _print_progress(offset, message, status):
    detail = " ".join(
        f"{name}={message[name]}"
        for name in ("level_cm", "rise_cm_min", "alert_level", "wadi_node")
        if name in message
    )
    minutes, seconds = divmod(offset, 60)
    print(
        f"+{minutes:3d}:{seconds:02d}  {message['kind']:<15} node {message['node']:<5} "
        f"{detail:<40} {status}",
        flush=True,
    )


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Replay a recorded scenario into a running WadiWatch backend."
    )
    parser.add_argument(
        "scenario", nargs="?", default=DEFAULT_SCENARIO, help="scenario file (default: the flood demo)"
    )
    parser.add_argument("--url", default=DEFAULT_URL, help=f"backend address (default {DEFAULT_URL})")
    parser.add_argument(
        "--speed",
        type=float,
        default=DEFAULT_SPEED,
        help=f"scenario seconds per real second (default {DEFAULT_SPEED:g})",
    )
    parser.add_argument(
        "--instant",
        action="store_true",
        help="send everything at once, timestamped as an event that has just ended",
    )
    arguments = parser.parse_args(argv)
    if arguments.speed <= 0:
        parser.error("--speed must be greater than 0")

    scenario = load_scenario(arguments.scenario)
    try:
        summary = replay(
            scenario,
            http_sender(arguments.url),
            speed=arguments.speed,
            instant=arguments.instant,
            report=_print_progress,
        )
    except urllib.error.URLError as error:
        print(f"Could not reach the backend at {arguments.url}: {error.reason}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Stopped.", file=sys.stderr)
        return 130
    print(
        f"{summary.stored} stored, {summary.duplicate} duplicate, {summary.rejected} rejected",
        flush=True,
    )
    return 1 if summary.rejected else 0


if __name__ == "__main__":
    sys.exit(main())
