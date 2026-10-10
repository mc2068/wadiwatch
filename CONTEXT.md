# WadiWatch: project context

WadiWatch is a low-cost, low-power flash-flood early-warning system for the wadis of Nabeul / Cap Bon, built for the TSYP14 NEXUS challenge. This file fixes the vocabulary used in the report, the code and the pitch. Use these terms exactly; do not introduce synonyms.

## Glossary

| Term | Meaning |
|---|---|
| **Wadi** | A normally dry riverbed that carries storm water. The thing we monitor. |
| **Wadi node** | Solar-powered sensing unit mounted over a wadi (bridge, culvert). Measures water level and rainfall, decides the alert level locally, transmits over LoRa. |
| **Siren node** | Solar-powered unit in a neighbourhood that sounds a siren and flashes a strobe when it receives an authenticated alert. Listens continuously. |
| **Gateway** | Unit at a municipal building that receives LoRa traffic, sends SMS through a GSM modem and forwards data to the dashboard when internet is available. |
| **Dashboard** | Web view for the municipality and Civil Protection: map, levels, node health, alert log, acknowledgement. |
| **Dry mode** | Default node state: one measurement every 15 minutes. |
| **Storm mode** | Node state entered on rainfall or rising level: one measurement per minute. |
| **Rate of rise** | Change in water level per unit time (cm/min). The main early signal of a flash flood. |
| **Alert level** | One of three graded states: **Watch**, **Warning**, **Evacuate**. |
| **Watch** | Heavy rain or level above the first threshold. Dashboard and SMS to officials only. |
| **Warning** | Level or rate of rise above the second threshold. SMS to registered households; siren pattern 1. |
| **Evacuate** | Level above the third threshold, or Warning confirmed by an upstream node. Continuous siren, SMS to everyone. |
| **Life-safety path** | Wadi node -> siren node, directly over LoRa. Must work with no gateway, no internet, no grid power and no GSM. |
| **Information path** | Wadi node -> gateway -> SMS and dashboard. Useful but allowed to degrade. |
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
