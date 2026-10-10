# 0001: Flash floods in Nabeul / Cap Bon

Date: 2026-10-10. Status: accepted.

## Context

NEXUS asks for one application area, ideally tied to a real Tunisian region. Candidates were flash floods, wildfire, drought and heat/air quality.

## Decision

Flash-flood early warning on the wadis of Nabeul / Cap Bon (application area: Environmental & Climate Sensing).

## Why

- The 22 September 2018 Cap Bon floods are a documented precedent: about 200 mm of rain, at least five deaths, recovery needs of about US$100 million.
- A national weather warning was issued the day before, yet people were still caught. That is the challenge statement exactly: data that does not reach the household in time.
- Flash floods have the clearest Detect -> Communicate -> Coordinate -> Respond chain and can be prototyped with hardware the team already has (ESP32, LoRa, ultrasonic sensor).

## Consequences

Warning time is minutes, so the design is dominated by latency and by working when infrastructure is down. Slow hazards (drought) are out of scope.
