# 0002: Three-tier alert path (siren, SMS, dashboard)

Date: 2026-10-10. Status: accepted.

## Context

The brief names four recipients (household, municipality, Civil Protection, farmer) and requires designs that survive intermittent power and connectivity. Options considered: SMS + dashboard only; mobile app + dashboard; siren + SMS + dashboard.

## Decision

Three tiers:

1. **Siren node** in the neighbourhood, triggered directly over LoRa (the life-safety path).
2. **SMS** to registered recipients, sent by the gateway's GSM modem.
3. **Dashboard** for the municipality and Civil Protection.

## Why

- A siren reaches people with no phone, no credit, no literacy and no electricity, including at night.
- SMS works on any handset and carries the detail a siren cannot (which wadi, which level, what to do).
- The dashboard gives responders the overview needed to coordinate.
- An app was rejected: it needs a smartphone and mobile data, which excludes the most exposed residents.

## Consequences

The siren node must listen continuously, so it needs a larger panel and battery than a wadi node. Alerts must be authenticated to prevent a spoofed siren.
