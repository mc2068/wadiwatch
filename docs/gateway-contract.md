# Gateway and backend message contract

Version 1. Two directions over one connection:

- **Up**: what the gateway sends the backend for every radio packet it accepts or transmits. The gateway firmware produces it; the backend ingest validates it. Most of this document.
- **Down**: the one command the backend sends the gateway, the all clear for a wadi, with an authentication code that only the backend can make. See "Commands" below.

Where each side is written down:

- Up, executable form: `backend/wadiwatch_backend/contract.py`.
- Up, what the gateway produces: `docs/gateway-contract/gateway-output.jsonl`, and `firmware/lib/wadiwatch/src/wadiwatch/uplink_message.cpp`, which writes it.
- Up, further examples the backend must accept, written by hand: `docs/gateway-contract/valid.jsonl`.
- Up, examples the backend must reject, with the reason: `docs/gateway-contract/invalid.jsonl`.
- Down, what the backend produces: `backend/wadiwatch_backend/downlink.py` and `docs/gateway-contract/backend-commands.jsonl`.
- Down, what the gateway reads: `firmware/lib/wadiwatch/src/wadiwatch/downlink_command.cpp`.
- The site both sides speak of, as the firmware site configuration has it: `docs/gateway-contract/firmware-site.json`.

`gateway-output.jsonl` and `backend-commands.jsonl` are the shared contract check. The second is signed with the published test command key, so it also holds the two sides to the same test key and the same way of computing the authentication code. The first holds the messages the gateway core hands to its uplink for a fixed scenario covering every message kind and every node the gateway hears: one flood at both wadi nodes of the prototype site, with the refresh of an alert, the Evacuate the downstream wadi node raises on hearing the upstream one, and the all clear, which the gateway relays on the command of the second file. The firmware tests (`firmware/test/test_gateway_contract`) feed the gateway core that command, produce the messages again and fail if the first file differs by a byte or the core refuses a line of the second. The backend tests ingest the first file and fail if the backend refuses a line, and fail if the backend does not produce the lines of the second byte for byte. So the gateway and the backend cannot drift apart in either direction without a test failing on one side, although the two suites never run together.

`firmware-site.json` is the same kind of check for the site itself. The firmware site configuration (`site_config.cpp`) is the one source of the node identifiers, of which wadi each wadi node is on and which node is upstream of it, of the Watch thresholds and of the intervals; the backend site file holds a copy, because the backend needs them to accept messages (rule 7) and to judge staleness and the all clear. The firmware tests write the file from the site configuration and fail if the committed one differs; the backend tests fail if `backend/site/prototype.json` disagrees with it.

Change this document, `contract.py`, `downlink.py`, the gateway's translation and reader, and the example files together. To rewrite `gateway-output.jsonl` and `firmware-site.json` after an intended change, run from `firmware/`:

```bash
WADIWATCH_UPDATE_CONTRACT=1 pio test -e native -f test_gateway_contract
```

Then run the backend tests.

## Transport

| | |
|---|---|
| Protocol | MQTT 3.1.1, QoS 1 |
| Topic, up | `wadiwatch/up` |
| Payload, up | One message per publish: a single JSON object, UTF-8, at most 512 bytes |
| Topic, down | `wadiwatch/down` |
| Payload, down | One command per publish: a single JSON object, UTF-8, at most 256 bytes. Never retained. |

One topic carries every message kind, so the broker keeps them in order.

The backend can also take the same payload over HTTP, `POST /api/ingest`, with one message as the body, for the replay tool and manual checks. That endpoint is not authenticated, so the backend serves it only when it is started with `WADIWATCH_HTTP_INGEST=1`, and only while it listens on the local machine. Both transports call the same ingest function, so they behave identically.

## Rules

1. **One radio packet, one message.** Forward each packet the gateway accepts (valid authentication code, known sender, fresh counter), and the all clear packets the gateway itself transmits. Never forward a rejected packet. A node may transmit the same packet several times (a wadi node sends each alert three times): the gateway accepts the first copy it hears and forwards that one message.
2. **Flat.** Every value is a number, a string or `true`/`false`. No nested objects, no arrays, no `null`.
3. **Strict.** A message with a missing field, a field of the wrong type, a value out of range, or a field this document does not list is rejected whole. So is a field given twice.
4. **Gateway time.** `ts` is the time the gateway received (or, for an all clear, transmitted) the packet, taken from the gateway's own clock. It is never the time of forwarding: a message held during an uplink outage keeps its original `ts`. It is not the time of the measurement either: a wadi node holds back the reading whose measurement raised an alert until the alert has had all its transmissions, so that reading reaches the gateway some seconds late. What happens while the gateway does not know the time is under "Held messages" below.
5. **In order.** Send messages in the order the packets were received, including after an outage or a restart of the gateway. The backend treats the last message to arrive as the latest state.
6. **Safe to repeat.** A message whose `node`, `counter` and `ts` match one already stored is ignored and reported as a duplicate, not as an error. When unsure whether a message arrived, send it again unchanged.
7. **Known nodes only.** `node` and `wadi_node` must be identifiers from the backend site file (`backend/site/prototype.json`), which must list the same identifiers as the firmware site configuration (`firmware-site.json` is the check).

Field order and white space do not matter to the backend. The examples use the order of the tables below with no white space; producing exactly that makes byte-for-byte comparison in firmware tests possible.

## Fields in every message

| Field | Type | Meaning |
|---|---|---|
| `v` | integer | Contract version. Always `1`. |
| `kind` | string | `reading`, `alert`, `acknowledgement`, `heartbeat`, `all_clear` or `test`. |
| `node` | integer, 1 to 65535 | Node identifier of the packet's sender. For example `0x0101` is sent as `257`. |
| `counter` | integer, 0 to 4294967295 | Message counter from the packet. |
| `ts` | integer, 1 to 4294967295 | Unix time in whole seconds, UTC, set by the gateway. |

## Fields per kind

Fields are required unless marked optional.

### `reading`: a measurement from a wadi node

| Field | Type | Meaning |
|---|---|---|
| `level_cm` | number, 0 to 10000 | Water level above the wadi bed, in centimetres. |
| `rise_cm_min` | number, -1000 to 1000 | Rate of rise in centimetres per minute. Negative while the level falls. |
| `rain_mm` | number, 0 to 10000, optional | Rain over the node's rainfall window (30 minutes on the prototype site), in millimetres. |
| `storm_mode` | boolean, optional | The node is in storm mode (`true`) or dry mode (`false`). A node sends a reading with every measurement in storm mode, and in dry mode while it holds an alert level. |
| `alert_level` | string, optional | The alert level the node held when it measured: `none`, `watch`, `warning` or `evacuate`. For display beside the reading only: it never changes the alert level the backend shows. |
| `level_repeated` | boolean, optional | `true` when the node's latest measurement gave no usable level, so that `level_cm` repeats the last level it did measure (0 if it has measured none) and says nothing about the wadi now. `false` for a level just measured. See "Repeated levels" below. |

The gateway sends the four optional fields with every reading. They are optional so that messages written before they existed stay valid.

```json
{"v":1,"kind":"reading","node":257,"counter":1041,"ts":1791590400,"level_cm":84.5,"rise_cm_min":3.2,"rain_mm":12.4,"storm_mode":true,"alert_level":"watch","level_repeated":false}
```

### `alert`: a wadi node states its alert level

Sent by a wadi node. Each transmission the gateway accepts is forwarded, including the refresh a node at Warning or above sends every minute.

| Field | Type | Meaning |
|---|---|---|
| `alert_level` | string | `none`, `watch`, `warning` or `evacuate`, in lower case. |
| `level_cm` | number, 0 to 10000, optional | The water level behind the alert, in centimetres. |
| `rise_cm_min` | number, -1000 to 1000, optional | The rate of rise behind the alert, in centimetres per minute. |
| `refresh` | boolean, optional | `false` for the alert that raises the node's alert level, `true` for an alert refresh: the alert a node at Warning or above sends again every minute to repeat the level it already announced. The node says which it is; the gateway needs the difference after an all clear (see "Commands"). |

| `level_repeated` | boolean, optional | As in a `reading`: `true` when `level_cm` repeats an earlier level because the node could not measure. |

The gateway sends the four optional fields with every alert.

```json
{"v":1,"kind":"alert","node":257,"counter":1042,"ts":1791590460,"alert_level":"warning","level_cm":104.0,"rise_cm_min":5.1,"refresh":false,"level_repeated":false}
```

### `acknowledgement`: a siren node confirms an alert

Sent by a siren node.

| Field | Type | Meaning |
|---|---|---|
| `wadi_node` | integer | The wadi node whose alert is acknowledged. |
| `alert_level` | string | The alert level acknowledged: `watch`, `warning` or `evacuate`. |
| `alert_counter` | integer, optional | `counter` of the alert packet being acknowledged. The acknowledgement packet carries it, so the gateway always sends it. |

```json
{"v":1,"kind":"acknowledgement","node":513,"counter":17,"ts":1791590462,"wadi_node":257,"alert_level":"warning","alert_counter":1042}
```

### `heartbeat`: a node proves it is alive

Sent by a wadi node or a siren node.

| Field | Type | Meaning |
|---|---|---|
| `battery_mv` | integer, 0 to 65535 | Battery voltage in millivolts. |
| `rssi_dbm` | integer, -200 to 0 | Signal strength in dBm: the value carried in the heartbeat packet, or the strength at which the gateway received the packet if the packet carries none. |
| `low_battery` | boolean | The node's battery is below its low-battery threshold. The heartbeat packet carries the voltage only: the gateway compares it with the threshold its site configuration gives for that node (`GatewayConfig::batteries`), and reports `false` for a node with no threshold there. |
| `tamper` | boolean | The node's tamper switch is open. |
| `level_cm` | number, optional | Wadi node: the last level measured. Left out until the node has measured one, and for a siren node. |
| `level_repeated` | boolean, optional | Wadi node, as in a `reading`: `true` when the node's latest measurement gave no level and `level_cm` is from an earlier one. The gateway sends it whenever it sends `level_cm`. |
| `discards` | integer, optional | Wadi node: sensor pings discarded since its previous heartbeat, because they were physically impossible or gave no echo. Not a running total. It stops counting at 255. |
| `refresh_lost` | boolean, optional | Siren node: it fell silent because alert refreshes stopped arriving. |

The heartbeat packet has the same layout for every node, and the gateway does not sort nodes by role, so it sends `discards` and `refresh_lost` in every heartbeat: `0` and `false` from a node they do not apply to.

```json
{"v":1,"kind":"heartbeat","node":257,"counter":1043,"ts":1791590500,"battery_mv":3920,"rssi_dbm":-97,"low_battery":false,"tamper":false,"level_cm":84.5,"level_repeated":false,"discards":2,"refresh_lost":false}
```

### Repeated levels

A wadi node measures with several pings of its level sensor. When too few of them are usable (no echo, a fouled or failed sensor) the measurement gives no level, and the node keeps the last level it did measure. It goes on sending that level in every alert refresh and heartbeat, and in the reading it sends on hearing an all clear, because the packet layouts have no way to leave a level out. `level_repeated` is how it says so: the packet carries the flag, and the gateway forwards it.

A repeated level proves that the node is alive and nothing about the water. The backend still stores and shows it, but does not count it as evidence for the all clear: an all clear is refused while the latest level of a wadi node is a repeated one, and the time a node could not measure does not count towards the 30 minutes below the Watch threshold. A message without the field is taken as measured.

### `all_clear`: the gateway transmitted an all clear

Sent by the gateway about its own packet, so `node` and `counter` are the gateway's and `ts` is the time it first transmitted the packet. An all clear is issued for a wadi and the gateway transmits one packet for each wadi node on it, so there is one message per wadi node cleared. The gateway transmits a further packet for a wadi node that shows it has not heard (see "Commands"), and reports that one too: several `all_clear` messages can name the same wadi node and the same command.

| Field | Type | Meaning |
|---|---|---|
| `wadi_node` | integer | The wadi node whose alert is ended. |
| `command` | integer, 1 to 4294967295, optional | The `command` number of the backend's command the gateway relayed. The all clear packet carries it, so the gateway always sends it; it is optional so that a message written by hand may leave it out. The backend uses it to find the officer who issued the all clear. |

```json
{"v":1,"kind":"all_clear","node":769,"counter":5,"ts":1791594000,"wadi_node":257,"command":41}
```

### `test`: a siren node ran a local test

Sent by a siren node when its test button starts a test. No further fields.

```json
{"v":1,"kind":"test","node":513,"counter":20,"ts":1791597600}
```

## From packet to message

The radio packets (`firmware/lib/wadiwatch/src/wadiwatch/packet.h`) count in other units than this contract. The gateway translates, and forwards every field a packet carries. Each number is the packet's own, written with one decimal, so nothing is rounded on the way.

| Packet field | Unit on the air | Message field | Unit here |
|---|---|---|---|
| `levelMm`, `lastLevelMm` | whole millimetres | `level_cm` | centimetres |
| `rateOfRiseMmPerMin` | millimetres per minute | `rise_cm_min` | centimetres per minute |
| `rainfallTenthsMm` | tenths of a millimetre | `rain_mm` | millimetres |
| `batteryMillivolts` | millivolts | `battery_mv`, and `low_battery` by the node's threshold | millivolts |
| `signalStrengthDbm` | dBm | `rssi_dbm` | dBm |
| `alertSender`, `alertCounter` | | `wadi_node`, `alert_counter` | |
| `refresh` (alert) | | `refresh` | |
| `levelRepeated`, `lastLevelRepeated` | | `level_repeated` | |
| `wadiNode`, `command` (all clear) | | `wadi_node`, `command` | |
| `discardedReadings`, `alertRefreshLost` | | `discards`, `refresh_lost` | |
| `hasLastLevel` | | whether a heartbeat has `level_cm` | |

The gateway forwards a packet whatever the role of its sender: whether a node may send a kind of message is for the backend to judge (rule 7 and the tables above).

## Commands

What the backend sends down to the gateway, on `wadiwatch/down`. There is one command: the all clear for a wadi, issued by a signed-in officer on the dashboard and accepted by the backend's rule (the level below the Watch threshold for 30 minutes at every wadi node of the wadi, on current data).

| Field | Type | Meaning |
|---|---|---|
| `v` | integer | Contract version. Always `1`. |
| `kind` | string | `all_clear`. |
| `command` | integer, 1 to 4294967295 | The backend's number for this command. It comes back in every `all_clear` message the gateway sends for it. |
| `ts` | integer, 1 to 4294967295 | Unix time in whole seconds, UTC, at which the backend issued the command. |
| `wadi` | integer, 1 to 255 | The wadi, by its identifier in the firmware site configuration (`WadiConfig::id`) and the backend site file (`wadis[].id`). The all clear covers every wadi node on it. |
| `auth` | string, 64 hexadecimal digits in lower case | The authentication code of the command under the command key. See "Authentication code" below. |

```json
{"v":1,"kind":"all_clear","command":41,"ts":1791593990,"wadi":1,"auth":"798ce3d2ad55640ccdeba5e6dead5ae444082d2e1eb6abb258767837b9782f9a"}
```

The backend writes the fields in this order with no white space. The gateway reads them in any order and is as strict as the backend is with messages: a command with a missing field, a field given twice, a field this table does not list, a value out of range or of the wrong type, another version or another kind is dropped whole.

### Authentication code

The MQTT broker is not trusted to decide who may silence a siren: whoever can publish to `wadiwatch/down` can put a well-formed command there. So the backend and the gateway share a secret, the **command key** (32 bytes), and every command carries the code that key gives it. The gateway works the code out again from the fields it read and drops the command unless the two are equal.

- The code is HMAC-SHA-256 under the command key, the whole digest, written as 64 hexadecimal digits in lower case.
- It is computed over these 11 bytes, not over the JSON text, so field order and white space do not matter:

| Bytes | Content |
|---|---|
| 0 | `v`, the contract version |
| 1 | the kind of command: `1` for `all_clear` |
| 2 to 5 | `command`, big-endian |
| 6 to 9 | `ts`, big-endian |
| 10 | `wadi` |

- The command key is not the site key that signs radio packets. Every wadi node and siren node holds the site key, and a node taken from its mast must not give anyone the means to command the gateway. The command key is on the gateway and the backend only.
- The key in the repository is a published test key (the ASCII text `WADIWATCH-TEST-COMMAND-KEY-ONLY!`), the same in `firmware/lib/wadiwatch/src/wadiwatch/command_key.cpp` and `backend/wadiwatch_backend/downlink.py`, for tests and the bench. The example above and `backend-commands.jsonl` are signed with it. A real key is never committed: the README says where each side takes it from.
- The code covers `ts`, and the gateway acts only on a fresh command (below), so a command read off the broker cannot be kept and given a later date. Sent again unchanged within those two minutes it is the officer's own decision arriving twice.

What the gateway does with a command that carries the right code:

1. **Only a fresh one.** A command can wait at the broker while the gateway is offline and arrive when the wadi is in flood again. The gateway acts on a command only if `ts` is no more than `GatewayConfig::commandMaxAgeS` (120 s on the prototype site) before its own clock, and not that much after it. It does not act on any command while it does not know the time. The backend does its part: it never retains a command, and does not hand one to an MQTT client that has no connection.
2. **Once.** The same `command` and `ts` delivered twice (QoS 1) is acted on once. A command for a wadi the gateway does not know is dropped.
3. **The event ends.** Every recipient who received an alert SMS for the event gets one closing SMS, queued and retried like any SMS. An alert SMS still waiting is not sent any more. The next alert that raises an alert level starts a new event.
4. **A signed all clear packet for each wadi node of the wadi**, carrying `command`, each transmitted several times like an alert, and reported up as an `all_clear` message from the gateway's store like every other message. A wadi node listens only for a short answer window after a packet of its own, so the gateway holds the packet for a wadi node until it next hears that node (within a minute for a node at Warning or above), and for `GatewayConfig::allClearHoldMs` at most. So an `all_clear` message can follow its command by up to a minute and a half.
5. **Again for a wadi node that has not heard.** A wadi node that held an alert level and goes on sending an alert refresh has missed the all clear: the gateway answers it with a further all clear packet (and message) and starts no event for it, up to `GatewayConfig::allClearMaxAnswers` times within `GatewayConfig::allClearAnswerPeriodMs`. A wadi node that has heard says so at once with a `reading` whose `alert_level` is `none`. An alert that raises a node's alert level (`refresh` false) is never answered with an all clear: it is a new event. So is an alert refresh at a higher alert level than the all clear ended at that node: the gateway did not hear the alert that raised it.

The gateway sends nothing back for a command it did not act on. The backend learns that the all clear happened from the `all_clear` messages, and that it did not from their absence: until they arrive it shows the alert as still in force, and after `all_clear_command_valid_s` (site file, the same 120 s) it lets the officer issue the command again.

## Held messages

The gateway keeps every accepted packet in a store that survives a restart, and lets go of it only when the uplink has taken its message. While the uplink is down, messages wait there and then leave in the order received (rule 5), each with the `ts` of its packet (rule 4). After a failed publish the gateway waits (`GatewayConfig::uplinkRetryIntervalMs`) before it offers the message again. If the gateway cannot tell whether a message arrived, the backend may get it twice, which rule 6 makes harmless.

**The store is bounded** (1024 messages at most, fewer if the gateway's storage is smaller). When it is full and another packet arrives, one message gives way: the least important one, and among equals the oldest. From least to most important:

1. `reading`
2. `heartbeat`
3. `acknowledgement` and `test`
4. an `alert` that tells the backend nothing new (the refresh a node at Warning or above sends every minute): it repeats the alert level of the alert or all clear held before it for the same wadi node or, when none is held, of the last one the uplink took
5. an `alert` that changes the alert level, and `all_clear`

The arriving packet takes part: if it is less important than everything stored, it is the one that is not kept. So after a long outage the backend may be missing readings, then heartbeats, then acknowledgements, then alert refreshes, always the oldest first, and what does arrive is still in order. A change of alert level is given up only by a store that holds nothing but changes of alert level and all clears, which a few hundred of them would have to fill; then the oldest goes, so that the backend still learns how things stand now. A wadi node whose change of alert level was given up is news again in its next refresh, which is then held as a change. A restarted gateway does not remember what the uplink took before, so the first alert it hears from each wadi node counts as a change unless one is held for that node. The gateway does not report how many messages it gave up.

**Before the gateway knows the time** (after power-on, until the mobile network or NTP has set its clock) it forwards nothing it cannot date. It holds each packet with the time since the gateway started, and when the clock is set works out `ts` from that. If the gateway restarts before the clock is ever set, how long before the restart those packets came is lost: they get the time of the restart as `ts`, which is the latest they can have been received, and their order is kept.

## What the backend does with a message

| Outcome | HTTP ingest answer | When |
|---|---|---|
| `stored` | 200 `{"status":"stored"}` | The message was valid and new. |
| `duplicate` | 200 `{"status":"duplicate"}` | Same `node`, `counter` and `ts` as a stored message. Nothing changes. |
| `rejected` | 400 `{"status":"rejected","error":"..."}` | Anything else. Nothing is stored. |

Over MQTT a rejected message is logged and dropped. The HTTP answers are those of a backend that serves the endpoint (see "Transport"); one that does not answers 404.

How stored messages become the alert level shown on the dashboard:

- A wadi node's alert level is the `alert_level` of its latest `alert`, or `none` once an `all_clear` for that wadi node has arrived after it.
- A siren node's alert level is the highest level among its latest `acknowledgement` per wadi node, leaving out wadi nodes that have had an `all_clear` since. What counts is when the alert it acknowledges arrived, if the acknowledgement names one the backend holds (`alert_counter`): a siren node's acknowledgement of the last alert refresh can arrive just behind the all clear that answered that refresh.
- In the alert log an `all_clear` is one entry per wadi node, with the officer and time of the command whose number it carries. A further `all_clear` for the same wadi node and command, with no alert from that node in between, belongs to the same entry.
- A `reading` never changes an alert level, not even through its own `alert_level` field. The decision belongs to the wadi node, and it states it in an `alert`.

## Not settled here

- The radio packet layouts are owned by the firmware. If an acknowledgement packet cannot supply `alert_level`, or a heartbeat packet cannot supply a field required here, change this contract rather than inventing values in the gateway.
- The gateway does not send a heartbeat of its own. If the dashboard needs to tell a silent gateway from a quiet wadi more quickly than the nodes' own traffic allows, add a gateway message kind.
- The gateway does not tell the backend that it refused or ignored a command. If the officer should see why an all clear did not happen (a stale command, a gateway without the time of day, a command key that is not the backend's), add a gateway message kind for it.
- Messages going up are not authenticated between the gateway and the backend: whoever can publish to `wadiwatch/up` can feed the backend false levels, on which an all clear could then be accepted. That needs the broker to restrict who may publish (accounts and access rules), or an authentication code on messages as on commands.
- An officer cannot issue an all clear for a wadi while one of its wadi nodes is silent, has never reported a level or is not measuring one ("Repeated levels"), so a wadi node destroyed by the flood, or one whose level sensor has failed, blocks the all clear for its wadi until it is replaced. Whether an officer may override that, and how, is not decided.
- The gateway does not tell the backend when it has given up messages from a full store. A gateway message kind could carry that count.
