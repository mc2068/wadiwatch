# WadiWatch

A low-cost, low-power flash-flood early-warning system for the wadis of Nabeul / Cap Bon, built for the TSYP14 NEXUS technical challenge.

Solar-powered wadi nodes measure water level and rainfall, decide the alert level on the node, and trigger neighbourhood sirens directly over LoRa. A gateway sends SMS and feeds a dashboard for the municipality and Civil Protection.

## Repository layout

| Path | Content |
|---|---|
| `CONTEXT.md` | Project vocabulary and fixed constraints |
| `docs/adr/` | Design decisions and their reasons |
| `docs/gateway-contract.md` | The messages the gateway sends to the backend |
| `firmware/` | Wadi node, siren node and gateway firmware, with tests that run on a PC |
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

The logic of each device is a portable core that reaches the outside world only through ports (level sensor, clock, radio, siren output, GSM modem, persistent storage). On the ESP32 the ports are hardware adapters; in tests they are fakes joined by an in-memory radio, so whole flood scenarios run on a PC in milliseconds with no hardware.

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

Build the wadi node, siren node and gateway firmware for the ESP32:

```bash
cd firmware
pio run
```

If `pio` is not on the PATH, use `python -m platformio` in its place. The board is set in `firmware/platformio.ini`; node identifiers and thresholds are set only in the site configuration inside the `wadiwatch` library.

Every radio packet is signed with a key shared by the nodes of one site, and a node ignores packets that are not signed with it, come from a node outside the site, or repeat an earlier packet. The key in the repository is a published test key, for tests and the bench only. A real site key never goes into the repository: put it in `firmware/lib/wadiwatch/src/wadiwatch/site_key_local.h`, which git ignores, as described at the top of `site_config.cpp`.

The hardware adapters (sensor, LoRa radio, siren driver, GSM modem, flash storage) are not written yet, so the ESP32 builds currently run the cores against placeholders.

The gateway sends SMS to the registered recipients, for alerts it accepts by the same rules as a siren node. It does not send anything to the backend yet. The recipient list committed in the `wadiwatch` library (`prototype_recipients.cpp`) holds fake numbers only, and a test fails if any other number is committed: real numbers are personal data, belong in local configuration on the gateway and never in the repository. The Arabic and French SMS texts are all in `sms_templates.cpp`.

## Backend and dashboard

The backend (Python, FastAPI, SQLite) takes the messages the gateway sends, stores them, and serves an API and the dashboard: a map of the nodes coloured by alert level, the water level and rate-of-rise curves of each wadi node, node health, the age of the gateway's latest data, and the alert log. The message format is in `docs/gateway-contract.md`. It needs no hardware: tests and the replay tool feed messages in.

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

### Alert log and officer sign-in

The alert log at the bottom of the dashboard lists every alert, test and all clear, newest first, and is open to anyone who can open the dashboard. For each alert it shows which siren nodes acknowledged it and whether an officer has. One alert entry is one change of alert level at one wadi node; the refresh a node sends every minute does not add entries.

Officers sign in to acknowledge an alert. While an Evacuate alert is in force and no officer has acknowledged it, the dashboard keeps a request for acknowledgement at the top of the page. The log shows everyone that an alert was acknowledged and when; the officer's account name is shown only to signed-in officers.

Sign-in uses test accounts. Create them with the seed step, once, and again whenever the database file has been deleted:

```bash
cd backend
python -m wadiwatch_backend.seed
```

The seed step creates two accounts, `officer1` and `officer2`, with a generated password each, and writes the passwords to `backend/data/officers.local.json`. It prints the account names and the path, never the passwords: open the file to read them. Git ignores that file (all of `backend/data/`, and any `*.local.json`); keep it on the machine that runs the backend, and never put a password in the repository, an issue or a message.

To use other account names or passwords, edit the file (one JSON object, account name to password, passwords of at least 12 characters) and run the seed step again. Account names are what the alert log shows as "who acknowledged", so use role names, not people's names. The seed step can run while the backend is running, and `--db` and `--credentials` point it at another database or file.

How sign-in is protected:

- Passwords are stored only as salted scrypt hashes.
- A session is kept on the server and lasts 12 hours; the browser holds a random session cookie that scripts cannot read (`HttpOnly`) and that other sites cannot make the browser send (`SameSite=Strict`).
- Requests that change something (acknowledge, sign out) must also carry the session's CSRF token, and are refused if the browser says they come from another site.
- Over HTTPS, set `WADIWATCH_SECURE_COOKIES=1` so the cookie is never sent over plain HTTP. A reverse proxy in front of the backend must pass the `Host` header through unchanged.

There is no limit on sign-in attempts and no way to remove an account except by deleting the database: enough for test accounts on a prototype, not for a deployment.

Settings are environment variables, listed in `backend/wadiwatch_backend/__main__.py`. Two matter most:

- `WADIWATCH_MQTT_HOST`: the MQTT broker to read gateway messages from (topic `wadiwatch/up`). Without it the backend is fed only through `POST /api/ingest`, which is what the replay tool uses.
- `WADIWATCH_SITE`: the site file with the nodes' identifiers, names and map positions, and the intervals below. The default, `backend/site/prototype.json`, uses placeholder positions in the Nabeul area, not surveyed locations.

What the dashboard says about health and staleness follows from three intervals in the site file, which must agree with the firmware site configuration:

| Setting | Meaning | Used for |
|---|---|---|
| `heartbeat_interval_s` | How often every node sends a heartbeat | A node is shown as faulty when its heartbeat has been missing for more than two intervals |
| `dry_mode_interval_s` | How often a wadi node measures in dry mode | The data is shown as stale when the gateway has delivered nothing for more than two intervals |
| `alert_refresh_interval_s` | How often a wadi node at Warning or Evacuate repeats its alert | While any wadi node is at Warning or Evacuate, two of these intervals of silence already make the data stale |

Low-battery and tamper flags come straight from each node's latest heartbeat. The API behind these panels is `GET /api/health` and `GET /api/nodes/<id>/readings?from=<unix seconds>&to=<unix seconds>` (the last day if no period is given).

The ingest endpoint is not authenticated. The backend listens on the local machine only unless `WADIWATCH_HOST` says otherwise; do not expose it to a network as it is.
