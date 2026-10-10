# WadiWatch

A low-cost, low-power flash-flood early-warning system for the wadis of Nabeul / Cap Bon, built for the TSYP14 NEXUS technical challenge.

Solar-powered wadi nodes measure water level and rainfall, decide the alert level on the node, and trigger neighbourhood sirens directly over LoRa. A gateway sends SMS and feeds a dashboard for the municipality and Civil Protection.

## Repository layout

| Path | Content |
|---|---|
| `CONTEXT.md` | Project vocabulary and fixed constraints |
| `docs/adr/` | Design decisions and their reasons |
| `docs/gateway-contract.md` | The messages the gateway sends to the backend |
| `firmware/` | Wadi node and siren node firmware, with tests that run on a PC |
| `backend/` | Backend and dashboard: ingests gateway messages, serves the API and the map |
| `report/NEXUS_Phase1_Report.docx` | Phase 1 report (editable) |
| `report/NEXUS_Phase1_Report_preview.pdf` | Same content rendered to PDF |
| `report/video_script.md` | 2-minute video pitch script |
| `report/figures/` | Diagrams used in the report |
| `report/build/` | Scripts that generate the figures and the report |

## Rebuilding the report

The report text lives in `report/build/content.js`. After editing it:

```bash
cd report/build
npm install
python make_figures.py
node build.js
```

`node build.js` writes the `.docx` and `out/preview.html`. Print the HTML to PDF from Chrome (A4, no headers and footers) to refresh the preview PDF.

## Firmware

The logic of each device is a portable core that reaches the outside world only through ports (level sensor, clock, radio, siren output, persistent storage). On the ESP32 the ports are hardware adapters; in tests they are fakes joined by an in-memory radio, so whole flood scenarios run on a PC in milliseconds with no hardware.

Setup, once:

```bash
pip install platformio
```

The PC tests need a C++ compiler (`g++`) on the PATH. On Windows:

```bash
winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
```

Run the tests:

```bash
cd firmware
pio test -e native
```

Build the wadi node and siren node firmware for the ESP32:

```bash
cd firmware
pio run
```

If `pio` is not on the PATH, use `python -m platformio` in its place. The board is set in `firmware/platformio.ini`; node identifiers and thresholds are set only in the site configuration inside the `wadiwatch` library.

The prototype site has two wadi nodes on one wadi, and the site configuration says which is upstream of which. `pio run` builds the firmware of the first; `pio run -e wadi_node_downstream` builds the same firmware with the second node's configuration.

Every radio packet is signed with a key shared by the nodes of one site, and a node ignores packets that are not signed with it, come from a node outside the site, or repeat an earlier packet. The key in the repository is a published test key, for tests and the bench only. A real site key never goes into the repository: put it in `firmware/lib/wadiwatch/src/wadiwatch/site_key_local.h`, which git ignores, as described at the top of `site_config.cpp`.

The hardware adapters (sensor, LoRa radio, siren driver, flash storage) are not written yet, so the ESP32 builds currently run the cores against placeholders. The gateway firmware will be added during the prototype phase.

## Backend and dashboard

The backend (Python, FastAPI, SQLite) takes the messages the gateway sends, stores them, and serves an API and the dashboard: a map of the nodes coloured by alert level. The message format is in `docs/gateway-contract.md`. It needs no hardware: tests and the replay tool feed messages in.

Setup, once (Python 3.10 or later):

```bash
cd backend
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements-dev.txt    # Windows
.venv/bin/python -m pip install -r requirements-dev.txt        # Linux, macOS
```

The commands below are written as `python`. Use the interpreter of the virtual environment (`.venv/Scripts/python` or `.venv/bin/python`), or activate the environment first.

Run the backend:

```bash
cd backend
python -m wadiwatch_backend
```

Then open http://127.0.0.1:8000. Everything ingested is kept in `backend/data/wadiwatch.db` and is still there after a restart; delete that file to start empty.

Run the tests:

```bash
cd backend
python -m pytest
```

Replay the flood scenario into the running backend, from a second terminal, and watch the map change:

```bash
cd backend
python -m wadiwatch_backend.replay              # one scenario minute per second, about two minutes
python -m wadiwatch_backend.replay --speed 120  # twice as fast
python -m wadiwatch_backend.replay --instant    # all at once, as an event that has just ended
```

The scenario (`backend/scenarios/flood_demo.jsonl`) is synthetic until there is hardware to record from: two wadi nodes rise through Watch and Warning, the downstream node reaches Evacuate by upstream confirmation, the siren node acknowledges, and an all clear ends the event.

Settings are environment variables, listed in `backend/wadiwatch_backend/__main__.py`. Two matter most:

- `WADIWATCH_MQTT_HOST`: the MQTT broker to read gateway messages from (topic `wadiwatch/up`). Without it the backend is fed only through `POST /api/ingest`, which is what the replay tool uses.
- `WADIWATCH_SITE`: the site file with the nodes' identifiers, names and map positions. The default, `backend/site/prototype.json`, uses placeholder positions in the Nabeul area, not surveyed locations.

The ingest endpoint is not authenticated. The backend listens on the local machine only unless `WADIWATCH_HOST` says otherwise; do not expose it to a network as it is.
