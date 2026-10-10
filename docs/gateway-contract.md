# Gateway-to-backend message contract

Version 1. This is what the gateway sends up to the backend for every radio packet it accepts or transmits. The gateway firmware produces it; the backend ingest validates it.

- Executable form: `backend/wadiwatch_backend/contract.py`.
- Examples the backend must accept: `docs/gateway-contract/valid.jsonl`.
- Examples the backend must reject, with the reason: `docs/gateway-contract/invalid.jsonl`.

The backend tests ingest both example files. They are the shared contract check: the gateway tests should prove that the gateway core produces messages of the same shape as `valid.jsonl`. Change this document, `contract.py` and the example files together.

## Transport

| | |
|---|---|
| Protocol | MQTT 3.1.1, QoS 1 |
| Topic | `wadiwatch/up` |
| Payload | One message per publish: a single JSON object, UTF-8, at most 512 bytes |
| Reserved | `wadiwatch/down` for backend-to-gateway commands (the all clear). Not defined yet. |

One topic carries every message kind, so the broker keeps them in order.

The backend also accepts the same payload over HTTP, `POST /api/ingest`, with one message as the body. The replay tool and manual checks use it. Both transports call the same ingest function, so they behave identically.

## Rules

1. **One radio packet, one message.** Forward each packet the gateway accepts (valid authentication code, known sender, fresh counter), and the all clear packets the gateway itself transmits. Never forward a rejected packet.
2. **Flat.** Every value is a number, a string or `true`/`false`. No nested objects, no arrays, no `null`.
3. **Strict.** A message with a missing field, a field of the wrong type, a value out of range, or a field this document does not list is rejected whole. So is a field given twice.
4. **Gateway time.** `ts` is the time the gateway received (or, for an all clear, transmitted) the packet, taken from the gateway's own clock. It is never the time of forwarding: a message held during an uplink outage keeps its original `ts`.
5. **In order.** Send messages in the order the packets were received, including after an outage. The backend treats the last message to arrive as the latest state.
6. **Safe to repeat.** A message whose `node`, `counter` and `ts` match one already stored is ignored and reported as a duplicate, not as an error. When unsure whether a message arrived, send it again unchanged.
7. **Known nodes only.** `node` and `wadi_node` must be identifiers from the backend site file (`backend/site/prototype.json`), which must list the same identifiers as the firmware site configuration.

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

```json
{"v":1,"kind":"reading","node":257,"counter":1041,"ts":1791590400,"level_cm":84.5,"rise_cm_min":3.2}
```

### `alert`: a wadi node states its alert level

Sent by a wadi node. Each transmission the gateway accepts is forwarded, including the refresh a node at Warning or above sends every minute.

| Field | Type | Meaning |
|---|---|---|
| `alert_level` | string | `none`, `watch`, `warning` or `evacuate`, in lower case. |

```json
{"v":1,"kind":"alert","node":257,"counter":1042,"ts":1791590460,"alert_level":"warning"}
```

### `acknowledgement`: a siren node confirms an alert

Sent by a siren node.

| Field | Type | Meaning |
|---|---|---|
| `wadi_node` | integer | The wadi node whose alert is acknowledged. |
| `alert_level` | string | The alert level acknowledged: `watch`, `warning` or `evacuate`. |
| `alert_counter` | integer, optional | `counter` of the alert packet being acknowledged, if the acknowledgement packet carries it. |

```json
{"v":1,"kind":"acknowledgement","node":513,"counter":17,"ts":1791590462,"wadi_node":257,"alert_level":"warning"}
```

### `heartbeat`: a node proves it is alive

Sent by a wadi node or a siren node.

| Field | Type | Meaning |
|---|---|---|
| `battery_mv` | integer, 0 to 65535 | Battery voltage in millivolts. |
| `rssi_dbm` | integer, -200 to 0 | Signal strength in dBm: the value carried in the heartbeat packet, or the strength at which the gateway received the packet if the packet carries none. |
| `low_battery` | boolean | The node's battery is below its low-battery threshold. |
| `tamper` | boolean | The node's tamper switch is open. |
| `level_cm` | number, optional | Wadi node: the last level measured. |
| `discards` | integer, optional | Wadi node: count of physically impossible readings discarded. |
| `refresh_lost` | boolean, optional | Siren node: it fell silent because alert refreshes stopped arriving. |

```json
{"v":1,"kind":"heartbeat","node":257,"counter":1043,"ts":1791590500,"battery_mv":3920,"rssi_dbm":-97,"low_battery":false,"tamper":false,"level_cm":84.5,"discards":2}
```

### `all_clear`: the gateway transmitted an all clear

Sent by the gateway about its own packet, so `node` and `counter` are the gateway's. One message per wadi node cleared.

| Field | Type | Meaning |
|---|---|---|
| `wadi_node` | integer | The wadi node whose alert is ended. |

```json
{"v":1,"kind":"all_clear","node":769,"counter":5,"ts":1791594000,"wadi_node":257}
```

### `test`: a siren node ran a local test

Sent by a siren node when its test button starts a test. No further fields.

```json
{"v":1,"kind":"test","node":513,"counter":20,"ts":1791597600}
```

## What the backend does with a message

| Outcome | HTTP ingest answer | When |
|---|---|---|
| `stored` | 200 `{"status":"stored"}` | The message was valid and new. |
| `duplicate` | 200 `{"status":"duplicate"}` | Same `node`, `counter` and `ts` as a stored message. Nothing changes. |
| `rejected` | 400 `{"status":"rejected","error":"..."}` | Anything else. Nothing is stored. |

Over MQTT a rejected message is logged and dropped.

How stored messages become the alert level shown on the dashboard:

- A wadi node's alert level is the `alert_level` of its latest `alert`, or `none` once an `all_clear` for that wadi node has arrived after it.
- A siren node's alert level is the highest level among its latest `acknowledgement` per wadi node, leaving out wadi nodes that have had an `all_clear` since.
- A `reading` never changes an alert level. The decision belongs to the wadi node.

## Not settled here

- The radio packet layouts are owned by the firmware. If an acknowledgement packet cannot supply `alert_level`, or a heartbeat packet cannot supply a field required here, change this contract rather than inventing values in the gateway.
- The gateway does not send a heartbeat of its own. If the dashboard needs to tell a silent gateway from a quiet wadi more quickly than the nodes' own traffic allows, add a gateway message kind.
- The gateway must have a real clock (from the mobile network or NTP) before it stamps messages. What it does before the clock is set is for the gateway ticket to decide.
