# WadiWatch: project context

WadiWatch is a low-cost, low-power flash-flood early-warning system for the wadis of Nabeul / Cap Bon, built for the TSYP14 NEXUS challenge. This file fixes the vocabulary used in the report, the code and the pitch. Use these terms exactly; do not introduce synonyms.

## Glossary

| Term | Meaning |
|---|---|
| **Wadi** | A normally dry riverbed that carries storm water. The thing we monitor. |
| **Wadi node** | Solar-powered sensing unit mounted over a wadi (bridge, culvert). Measures water level and rainfall, decides the alert level locally, transmits over LoRa. |
| **Siren node** | Solar-powered unit in a neighbourhood that sounds a siren and flashes a strobe when it receives an authenticated alert. Listens continuously. |
| **Gateway** | Unit at a municipal building that receives LoRa traffic, sends SMS through a GSM modem and forwards data to the dashboard when internet is available. |
| **Dashboard** | Web view for the municipality and Civil Protection: map, levels, node health, alert log, acknowledgement, all clear. |
| **Dry mode** | Default node state: one measurement every 15 minutes. |
| **Storm mode** | Node state entered on rainfall or rising level: one measurement per minute. |
| **Rate of rise** | Change in water level per unit time (cm/min). The main early signal of a flash flood. |
| **Alert level** | One of three graded states: **Watch**, **Warning**, **Evacuate**. |
| **Watch** | Heavy rain or level above the first threshold. Dashboard and SMS to officials only. |
| **Warning** | Level or rate of rise above the second threshold. SMS to registered households; siren pattern 1. |
| **Evacuate** | Level above the third threshold, or Warning confirmed by an upstream node. Continuous siren, SMS to everyone. |
| **Life-safety path** | Wadi node -> siren node, directly over LoRa. Must work with no gateway, no internet, no grid power and no GSM. |
| **Information path** | Wadi node -> gateway -> SMS and dashboard. Useful but allowed to degrade. |
| **Event** | One flood at one wadi, from the first alert above no alert at any of its wadi nodes until the all clear. Its alert level is the highest any of those nodes has announced. SMS, the closing SMS and the all clear all go by event. |
| **All clear** | The end of an event, decided by a signed-in officer on the dashboard and never by a device: accepted only once the level has stayed below the Watch threshold for 30 minutes at every wadi node of the wadi. The gateway relays it as a signed packet for each of those wadi nodes. The only way an alert level comes down. |
| **Closing SMS** | The SMS sent at the all clear to everyone who received an alert SMS for the event. |
| **Alert refresh** | The alert a wadi node at Warning or Evacuate sends again every minute, repeating the alert level it already announced. Keeps siren nodes sounding and tells the downstream node. An alert that raises the alert level is not a refresh. |
| **Upstream node** | The wadi node next up the same wadi from a given wadi node. Its Warning turns a Warning downstream into Evacuate. |
| **Receive window** | The short time after each storm-mode measurement in which a wadi node with an upstream node keeps its receiver on to hear it. |
| **Answer window** | The short time after each packet it transmits in which a wadi node that holds an alert level keeps its receiver on. The gateway answers in it with the all clear. |
| **Heartbeat** | Periodic status packet (battery, signal, last reading) that proves a node is alive. |
| **Registered recipient** | A resident, farmer or official who has consented to receive SMS alerts. |
| **Wadi committee** | Local group (residents, municipality, Civil Protection) that owns thresholds, siren locations and the recipient list. |
| **Pilot** | First real deployment on one wadi in Nabeul: 4 wadi nodes, 3 siren nodes, 1 gateway. |

## Fixed constraints

- Phase 1 deliverables are anonymous: no personal names, no school, no Student Branch name, in any file that is submitted.
- Pure software submissions are rejected: the physical wadi node and the LoRa link are the core of the project.
- Every number in the report is either cited or labelled as an estimate.

## Decisions

See `docs/adr/`.
