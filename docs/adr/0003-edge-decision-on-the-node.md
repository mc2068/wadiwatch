# 0003: The alert decision is made on the wadi node

Date: 2026-10-10. Status: accepted.

## Context

The alert level could be computed in the cloud, at the gateway or on the wadi node. LoRaWAN was also considered as the radio protocol.

## Decision

- The wadi node computes the alert level itself (adaptive sampling, median filtering, level and rate-of-rise thresholds).
- Radio is LoRa peer-to-peer in the 868 MHz band, not LoRaWAN: the wadi node addresses siren nodes directly.
- Alert packets carry a message authentication code and a counter.
- Machine-learning forecasting is roadmap only and is never described as built.

## Why

- With LoRaWAN every packet passes through a gateway and network server. If either is down, no siren sounds. Peer-to-peer keeps the life-safety path independent of the gateway, the internet and the grid.
- Threshold logic is transparent: the wadi committee can read and set the thresholds, which an opaque model does not allow.
- It runs on an ESP32 within a small power budget.

## Consequences

Thresholds must be calibrated per site with the municipality and Civil Protection. Use of the 868 MHz band must be confirmed with the national frequency agency (ANF); 433 MHz is the fallback.
