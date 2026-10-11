# WadiWatch

A low-cost, low-power flash-flood early-warning system for the wadis of Nabeul / Cap Bon, built for the TSYP14 NEXUS technical challenge.

Solar-powered wadi nodes measure water level and rainfall, decide the alert level on the node, and trigger neighbourhood sirens directly over LoRa. A gateway sends SMS and feeds a dashboard for the municipality and Civil Protection.

## Repository layout

| Path | Content |
|---|---|
| `CONTEXT.md` | Project vocabulary and fixed constraints |
| `docs/adr/` | Design decisions and their reasons |
| `docs/gateway-contract.md` | The messages the gateway sends to the backend, and the all clear command the backend sends the gateway |
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

If `pio` is not on the PATH, use `python -m platformio` in its place. The board is set in `firmware/platformio.ini`; node identifiers, thresholds and timings are set in the site configuration inside the `wadiwatch` library (`site_config.cpp`), which is their one source. The backend needs a copy of some of them in its site file; the paragraph on `test_gateway_contract` below says how that copy is held to the source.

The prototype site has two wadi nodes on one wadi, and the site configuration says which is upstream of which. `pio run` builds the firmware of every device: the `wadi_node` environment is the first wadi node, and `wadi_node_downstream` is the same firmware with the second node's configuration.

Every radio packet is signed with a key shared by the nodes of one site, and a node ignores packets that are not signed with it, come from a node outside the site, or repeat an earlier packet. The key in the repository is a published test key, for tests and the bench only. A real site key never goes into the repository: put it in `firmware/lib/wadiwatch/src/wadiwatch/site_key_local.h`, which git ignores, as described at the top of `site_config.cpp`.

The hardware adapters (sensor, LoRa radio, siren driver, GSM modem, uplink, network time, flash storage) are not written yet, so the ESP32 builds currently run the cores against placeholders. The gateway's placeholder clock never knows the time of day, so that build holds its messages and forwards none, and its placeholder uplink never delivers a command, so it relays no all clear.

One firmware test, `test_gateway_contract`, compares what the gateway core sends with `docs/gateway-contract/gateway-output.jsonl`, the file the backend tests ingest, and feeds the gateway core the command in `docs/gateway-contract/backend-commands.jsonl`, the file the backend tests must reproduce. The same test writes down what the site configuration says about the values the backend site file copies (node identifiers, the wadi and the upstream node of each wadi node, Watch thresholds, intervals) and fails unless `docs/gateway-contract/firmware-site.json` holds exactly that; the backend tests fail unless `backend/site/prototype.json` agrees with that file. So a value changed on one side only fails a test on one side or the other. It finds the files from `firmware/` or from the repository root; started from anywhere else, set `WADIWATCH_CONTRACT_DIR` to the directory. The contract document says how to rewrite the files after an intended change.

The gateway sends SMS to the registered recipients, for alerts it accepts by the same rules as a siren node. It also forwards every packet it accepts to the backend, as the messages of `docs/gateway-contract.md`, each with the time the packet was received. While the uplink is down the messages wait in a store that survives a restart and are forwarded in order when it returns; what gives way when that store is full is in the contract document under "Held messages". SMS do not wait for the uplink. The recipient list committed in the `wadiwatch` library (`prototype_recipients.cpp`) holds fake numbers only, and a test fails if any other number is committed: real numbers are personal data, belong in local configuration on the gateway and never in the repository. The Arabic and French SMS texts are all in `sms_templates.cpp`.

### The all clear on the devices

An alert level comes down only through the all clear, which an officer issues on the dashboard for a wadi. The backend's command comes down the gateway's uplink; the gateway then:

- transmits a signed all clear packet for each wadi node of that wadi, each several times like an alert. The siren node stops for a wadi node when it accepts that node's all clear. The wadi node drops its alert level, stops its alert refresh, forgets what it heard from its upstream node, says at once in a reading that it holds no alert level, and goes back to dry mode when the quiet period has passed. If its water still calls for an alert it raises one again straight away, as a new event;
- ends the event and sends one closing SMS to everyone who received an alert SMS for it (an alert SMS still waiting is dropped, and whoever was never told of the event is not told of its end);
- reports each all clear packet to the backend through the same store as every other message.

A wadi node in dry mode sends nothing but its hourly heartbeat. While it holds an alert level it also sends a reading with every measurement, in dry mode too (a node at Watch can be back in dry mode long before the all clear): the backend judges the all clear on levels measured recently, and the gateway can reach the node only after a packet of its own.

A wadi node sleeps with its receiver off. While it holds an alert level it keeps the receiver on for a short answer window (`answerWindowMs`, 2 s) after each packet it transmits, and that is when the gateway reaches it: the gateway holds a wadi node's all clear until it next hears that node (within a minute at Warning or above, where the alert is sent again every minute), and for `allClearHoldMs` (90 s) at most. So the sirens stop up to a minute and a half after the command, and they are not sounded again by a wadi node that had not heard yet. A node that goes on sending its alert refresh has missed the all clear and is answered again, a limited number of times. An alert that raises a node's alert level is never answered with an all clear: it starts a new event. Neither is an alert refresh at a higher alert level than the all clear ended, which is what the gateway hears when it missed the alert that raised it.

A wadi node that holds no alert level of its own can sleep through the all clear and still remember the Warning it heard from its upstream node. That memory lapses by itself: what was heard from upstream is in force only while the upstream node's alert refresh keeps arriving, and is dropped once none has been heard for `upstream.alertRefreshTimeoutMs` (5 minutes, five alert refresh intervals). A node at Warning listens without a break, so a genuine confirmation is never more than one alert refresh away.

What the gateway does not act on: a command that does not carry the authentication code of its command key (see "The command key" below), is malformed, names a wadi it does not know, is older than `commandMaxAgeS` (2 minutes) by its own clock, or arrives while it does not know the time. A command can wait at the MQTT broker through an uplink outage, and must not end a later flood. With GSM down the all clear still goes out on the radio and the closing SMS follow when the network returns; with the uplink down after the command arrived, the all clear goes out and is reported when the uplink returns. Every one of these values is in the site configuration (`GatewayConfig`, `WadiNodeConfig`), and the scenarios are in `firmware/test/test_all_clear`.

### The command key

Anyone who can publish to the MQTT broker can put an all clear command on the gateway's topic. The gateway therefore acts only on a command that carries an authentication code made with the command key, a 32-byte secret shared by the backend and the gateway and by nobody else (the contract document says how the code is computed). It is a different key from the site key: the site key is in every wadi node and siren node, the command key only in the gateway firmware.

It follows the rules of the site key. The key in the repository is a published test key, for tests and the bench only; the backend logs a warning while it signs with it. A real command key never goes into the repository:

- gateway: add `#define WADIWATCH_COMMAND_KEY { 0x.., ... }` (32 bytes) to `firmware/lib/wadiwatch/src/wadiwatch/site_key_local.h`, the same git-ignored file that holds the real site key, as described at the top of `command_key.cpp`;
- backend: set `WADIWATCH_COMMAND_KEY` to the same 32 bytes as 64 hexadecimal digits, in the environment of the machine that runs it. Never put it in a file that is committed, an issue or a message.

With a local key file in place the PC tests still pass: the shared contract examples are checked against the published test key, and every other scenario signs with whatever key the gateway under test holds.

## Backend and dashboard

The backend (Python, FastAPI, SQLite) takes the messages the gateway sends, stores them, and serves an API and the dashboard: a map of the nodes coloured by alert level, the water level and rate-of-rise curves of each wadi node, node health, the age of the gateway's latest data, and the alert log, where officers acknowledge alerts and issue the all clear. The message format is in `docs/gateway-contract.md`. It needs no hardware: tests feed messages in, and so does the replay tool, into a backend started for it.

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

Replay the flood scenario into the backend and watch the map change. The replay tool sends over HTTP to `POST /api/ingest`, an endpoint that is not authenticated and that the backend does not serve unless it is started for a replay, on the local machine:

```bash
cd backend
WADIWATCH_HTTP_INGEST=1 python -m wadiwatch_backend
```

Then, from a second terminal:

```bash
cd backend
python -m wadiwatch_backend.replay              # one scenario minute per second, about two minutes
python -m wadiwatch_backend.replay --speed 120  # twice as fast
python -m wadiwatch_backend.replay --instant    # all at once, as an event that has just ended
python -m wadiwatch_backend.replay --leave-open # without the scenario's all clear: issue it yourself
```

The scenario (`backend/scenarios/flood_demo.jsonl`) is synthetic until there is hardware to record from: two wadi nodes rise through Watch and Warning, the downstream node reaches Evacuate by upstream confirmation, the siren node acknowledges, and an all clear ends the event.

### Alert log and officer sign-in

The alert log at the bottom of the dashboard lists every alert, test and all clear, newest first, and is open to anyone who can open the dashboard. For each alert it shows which siren nodes acknowledged it and whether an officer has. One alert entry is one change of alert level at one wadi node; the refresh a node sends every minute does not add entries.

Officers sign in to acknowledge an alert. While an Evacuate alert is in force and no officer has acknowledged it, the dashboard keeps a request for acknowledgement at the top of the window, in view wherever the page is scrolled to. The log shows everyone that an alert was acknowledged and when; the officer's account name is shown only to signed-in officers.

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
- Requests that change something (acknowledge, issue an all clear, sign out) must also carry the session's CSRF token, and are refused if the browser says they come from another site.
- Over HTTPS, set `WADIWATCH_SECURE_COOKIES=1` so the cookie is never sent over plain HTTP. A reverse proxy in front of the backend must pass the `Host` header through unchanged.

There is no limit on sign-in attempts and no way to remove an account except by deleting the database: enough for test accounts on a prototype, not for a deployment.

### The all clear

A signed-in officer ends an event with the all clear. The control stands in the alert log, in the Officer column beside the latest alert in force of each wadi, and asks for confirmation before it sends anything. An all clear is issued for a wadi and covers every wadi node on it.

The backend refuses the all clear unless, at every wadi node of the wadi, the level has stayed below that node's Watch threshold for 30 minutes. It needs evidence for that, and silence is not evidence, so it also refuses when:

- the gateway's data is stale (the rule of the table below): the last levels heard do not say how the wadi is now;
- a wadi node has never reported a level, or last reported one too long ago: more than two alert refresh intervals for a node at Warning or Evacuate, which reports every minute; more than two dry-mode intervals for a node at Watch, which reports every measurement while it holds an alert level, in dry mode too; and more than two heartbeat intervals for a node with no alert level, which in dry mode reports its level only in its heartbeat;
- a wadi node is not measuring: its level sensor gives no usable reading, so what it sends only repeats the last level it measured (`level_repeated` in the contract). A wadi node at Warning goes on sending its alert refresh every minute with that old level, and the refusal says for how long the node has not measured.

The 30 minutes count from the first level measured below the threshold after the last report that was at or above it or was only a repeated level, by the gateway's time of each message, so levels forwarded late after an uplink outage count for when they were measured, and the time a node could not measure does not count. While the all clear is refused the control is greyed out and says why, in the backend's words; `GET /api/wadis` gives the same verdict to anyone. A wadi node that the flood has destroyed therefore blocks the all clear for its wadi until it is replaced: there is no override.

When the all clear is accepted the backend sends the gateway a command (`POST /api/wadis/<id>/all-clear`, guarded like the acknowledgement: session cookie, CSRF token, own origin). The alert stays in force on the dashboard until the gateway reports that it has transmitted the all clear, which can take a minute and a half; meanwhile the log says that the all clear has been sent and by whom. Then the log shows one all clear entry for each wadi node, with the time and, to signed-in officers, the account that issued it. If the gateway reports nothing within two minutes (it is offline, or it refused a command that reached it too late), the control comes back.

The command needs a way to the gateway. With `WADIWATCH_MQTT_HOST` set it goes to the broker (topic `wadiwatch/down`); the backend sends nothing while it has no connection to the broker, and answers that the all clear was not sent. Without a broker no all clear can be issued, unless the gateway is simulated:

```bash
cd backend
WADIWATCH_GATEWAY=simulated WADIWATCH_HTTP_INGEST=1 python -m wadiwatch_backend
python -m wadiwatch_backend.replay --instant --leave-open    # second terminal
```

The simulated gateway is a stand-in for trying the dashboard without hardware: it answers the command with the `all_clear` messages a gateway would report, and nothing else happens (no radio, no siren, no SMS). After an instant replay the nodes of the scenario stop reporting, so the data turns stale two minutes later and the all clear is refused from then on, as it should be.

Settings are environment variables, listed in `backend/wadiwatch_backend/__main__.py`. Three matter most:

- `WADIWATCH_MQTT_HOST`: the MQTT broker to read gateway messages from (topic `wadiwatch/up`) and to send the gateway its commands through (topic `wadiwatch/down`). Without it the backend hears no gateway.
- `WADIWATCH_COMMAND_KEY`: the command key the gateway holds, as 64 hexadecimal digits (see "The command key" above). Without it the backend signs its all clear commands with the published test key, which a gateway with a real key ignores.
- `WADIWATCH_SITE`: the site file with the nodes' identifiers, names and map positions, the wadis, and the settings below. The default, `backend/site/prototype.json`, uses placeholder positions in the Nabeul area, not surveyed locations.

The site file names the wadis (`wadis`: an identifier and a name each) and gives every wadi node the wadi it is mounted over (`wadi`) and its Watch threshold in centimetres above the bed (`watch_level_cm`). The wadi identifiers and the thresholds must agree with the firmware site configuration: on the prototype site, one wadi with both wadi nodes, and 60 cm (30 % of a bank-full depth of 200 cm). A test holds them, the node identifiers and the intervals of the table below to what the firmware says (`docs/gateway-contract/firmware-site.json`, written by the firmware tests): change the firmware site configuration first, regenerate that file, then change the site file to match. A site file without them is refused when the backend starts. Nothing stored changes shape: an existing database file keeps working.

What the dashboard says about health, staleness and the all clear follows from these settings in the site file. All but `all_clear_quiet_s`, which is the backend's alone, are copies of firmware values and are checked against them:

| Setting | Meaning | Used for |
|---|---|---|
| `heartbeat_interval_s` | How often every node sends a heartbeat | A node is shown as faulty when its heartbeat has been missing for more than two intervals. On a day with no alert level anywhere the nodes send nothing else, so the data is shown as stale when the gateway has delivered nothing for more than two intervals |
| `dry_mode_interval_s` | How often a wadi node measures in dry mode | A wadi node that holds an alert level reports every measurement: while a wadi node is at Watch, two of these intervals of silence make the data stale, and make that node too silent for an all clear |
| `alert_refresh_interval_s` | How often a wadi node at Warning or Evacuate repeats its alert | While any wadi node is at Warning or Evacuate, two of these intervals of silence already make the data stale |
| `all_clear_quiet_s` | How long the level must have stayed below the Watch threshold | The all clear is refused until then (30 minutes) |
| `all_clear_command_valid_s` | How long the gateway still acts on an all clear command (its `commandMaxAgeS`) | A command the gateway has not reported back is treated as on its way for this long, then the officer may issue it again |

Low-battery and tamper flags come straight from each node's latest heartbeat. The API behind these panels is `GET /api/health` and `GET /api/nodes/<id>/readings?from=<unix seconds>&to=<unix seconds>` (the last day if no period is given).

`POST /api/ingest` takes gateway messages from anyone who can reach it, and the all clear is judged on gateway messages. So the backend serves it only when `WADIWATCH_HTTP_INGEST=1` is set, which is meant for replaying a scenario on the local machine, and refuses to start with that setting when `WADIWATCH_HOST` is anything but the local machine. A backend started without it answers 404 there, and the replay tool says how to start it. The backend listens on the local machine only unless `WADIWATCH_HOST` says otherwise. The MQTT topics are as open as the broker makes them: give the broker accounts and access rules before the backend is exposed to a network (the gateway's commands are authenticated, its messages are not).
