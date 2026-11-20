# Smart Receiver – Yamaha RX-V540RDS × Home Assistant

An ESP32 inside a 2003 Yamaha RX-V540RDS AV receiver (no RS-232, no network) that gives Home Assistant **two-way control with real state**: power, volume in dB, mute, input, sound program and input mode. Everything is read from the receiver's own front-panel display and verified there.

No trace cuts and nothing on the mains side; the whole add-on unplugs via one DB-9 connector.

```
                 ┌──────────────────────── RX-V540RDS ─────────────────────────┐
                 │                                                             │
  IR remote ───► │ U501 IR module ──REM1──► IC601 system µC ──SPI──► IC501     │
                 │                  ▲        (M30626)        mode 3    M66003  │
                 │                  │                        │         VFD ctrl│
                 └──────────────────┼────────────────────────┼─────────────────┘
                     NEC frames     │                        │ sniffed (read-only)
                     (open collector)                        ▼
                 ┌──────────────────┴──── ESP32 (ESPHome) ───────────────────────┐
                 │  remote_transmitter ◄── commands      shadow display RAM ──►   │
                 │  closed-loop verify  ◄──────────────── decoded state           │
                 └───────────────────────────────┬──────────────────────────────┘
                                                 │ ESPHome API (WiFi)
                                                 ▼
                                Home Assistant: entities + media_player
```

## How it works

| Direction | Method |
|---|---|
| **State** | The ESP32 passively sniffs the bus between the system µC (IC601) and the VFD controller (IC501, M66003). It keeps a shadow copy of the display RAM and icon glyphs and decodes it into volume, input, program, mute and power. A level-3 GPIO interrupt on the chip-select line captures each burst. |
| **Control** | The ESP32 injects demodulated Yamaha NEC frames (custom code `7A`) on the IR receiver's output line REM1 via an open-collector transistor, so the real IR receiver keeps working. |
| **Verification** | Every command is checked against the display and retried if needed: power via display on/off, input via its name/icon, volume step by step against the shown dB value, mute via the `AUDIO MUTE ON/OFF` screen. |

The protocol is documented in [`Display bus protocol (observed).md`](Display%20bus%20protocol%20(observed).md).

## Repository layout

| Path | Contents |
|---|---|
| `esphome/` | ESPHome device config, the `yamaha_vfd` external component, `build.ps1` |
| `ha/` | Home Assistant package: `media_player` (Universal) + volume-level sensor |
| `firmware/m66003_sniffer/` | Stand-alone ESP-IDF sniffer + IR test tool used for Phases 1–2 |
| `Phase 0 …`, `Phase 1 …`, `Display bus protocol …` | Pin map, probe plan and decoded protocol |
| `Yamaha RX-V540RDS → Home Assistant integration plan.md` | Original plan and phases |

The service manual (RX-V540/RX-V540RDS/RX-V440/HTR-5650) is the reference for all hardware details, but it is not included: it's copyrighted.

## Hardware

| Signal | Receiver side (CB502 ↔ CB605 flex) | ESP32 | Interface |
|---|---|---|---|
| CEFD (chip select) | pin 6 | GPIO5 | 1 k / 2.2 k divider (5 V → ~3.15 V) |
| CKFD (clock) | pin 8 | GPIO18 | 1 k / 2.2 k divider |
| DTFD (data) | pin 7 | GPIO23 | 1 k / 2.2 k divider |
| REM1 (IR out) | pin 11 | GPIO25 | 4.7 k → BC547 base (100 k B-E), collector on REM1, emitter MG. **No divider on REM1** (weak pull-up) |
| MG (logic ground) | pin 1 | GND | |

Pin numbers on the flex are inferred; see Phase 0. The ESP32 is powered from its own 5 V supply (≥ 500 mA peak): the receiver's standby rail can't supply WiFi current.

**DB-9 between receiver and ESP** (receiver side female):

| Pin | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|
| Signal | REM1 | DTFD | MG | MG | spare | CEFD | MG | CKFD | /BLK |

## Build and flash (Windows, ESPHome CLI)

```powershell
cd esphome
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install esphome
copy secrets.example.yaml secrets.yaml      # then fill it in
powershell -ExecutionPolicy Bypass -File .\build.ps1                       # compile
powershell -ExecutionPolicy Bypass -File .\build.ps1 upload --device COM7  # first flash over USB
powershell -ExecutionPolicy Bypass -File .\build.ps1 run                   # later: compile + OTA
```

`build.ps1` keeps ESPHome's own ESP-IDF (5.5.x) in `C:\esphome-idf` and the build in `C:\esphome-build`. Both paths have no spaces, which ESP-IDF requires, and the setup stays separate from any other ESP-IDF install.

## Home Assistant

1. Adopt the device (ESPHome integration, API key from `secrets.yaml`).
2. Copy `ha/yamaha_rxv540.yaml` to `/config/packages/` (next to `configuration.yaml`), make sure `configuration.yaml` has `homeassistant: packages: !include_dir_named packages`, check the config and restart.

Result: **`media_player.yamaha_rx_v540`** (power, volume 0–100 % = −99…0 dB, mute, source, sound mode), plus these entities:

| Entity | Notes |
|---|---|
| Power, Mute | Switches; never sent at boot (`restore_mode: DISABLED`) |
| Volume | −99…0 dB, 1 dB per step; refused while muted; stops if the knob is turned against it |
| Input | 7 inputs; the last one seen is restored after an ESP reboot |
| Sound program / Sub-program | e.g. `MOVIE THTR 1` / `Spectacle` (grouping inferred) |
| Input mode | The INPUT MODE setting (`AUTO`, or `N/A` for CD/VCR/TUNER) |
| Display | Raw 14-character display text |
| Bus trace, frame counters | Diagnostics |

Updates are pushed on change, so an idle receiver causes no traffic.

## Status

| Phase | | |
|---|---|---|
| 0 | Pin map from the service manual | ✅ |
| 1 | Display bus capture and decode (go/no-go) | ✅ |
| 2 | IR injection (100/100 verified self-test) | ✅ |
| 3 | ESPHome component, state decoding, closed-loop control | ✅ (0 bad frames in 600+ under heavy traffic) |
| 4 | Home Assistant media_player | ✅ |
| 5 | Permanent install (enclosure, external antenna or ESP32-WROOM-32U) | ⏳ |

Out of scope: speakers A/B, tone, night, tuner presets, DSP program select (front-panel keys or unknown IR codes).

## Safety

The receiver has mains voltage inside. Only open it unplugged, with the filter capacitors discharged and measured. Never work on a live open chassis without an isolation transformer. All added wiring stays on the low-voltage side, away from the transformer and heatsinks.

## Credits

- [emsyscode/M66003](https://github.com/emsyscode/M66003) – Arduino driver for the M66003 VFD controller (command set reference)
- Yamaha RX-V540 service manual – schematics, IR code tables, DIAG mode
