# WadiWatch

A low-cost, low-power flash-flood early-warning system for the wadis of Nabeul / Cap Bon, built for the TSYP14 NEXUS technical challenge.

Solar-powered wadi nodes measure water level and rainfall, decide the alert level on the node, and trigger neighbourhood sirens directly over LoRa. A gateway sends SMS and feeds a dashboard for the municipality and Civil Protection.

## Repository layout

| Path | Content |
|---|---|
| `CONTEXT.md` | Project vocabulary and fixed constraints |
| `docs/adr/` | Design decisions and their reasons |
| `firmware/` | Wadi node and siren node firmware, with tests that run on a PC |
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

Every radio packet is signed with a key shared by the nodes of one site, and a node ignores packets that are not signed with it, come from a node outside the site, or repeat an earlier packet. The key in the repository is a published test key, for tests and the bench only. A real site key never goes into the repository: put it in `firmware/lib/wadiwatch/src/wadiwatch/site_key_local.h`, which git ignores, as described at the top of `site_config.cpp`.

The hardware adapters (sensor, LoRa radio, siren driver, flash storage) are not written yet, so the ESP32 builds currently run the cores against placeholders. The gateway and dashboard will be added during the prototype phase.
