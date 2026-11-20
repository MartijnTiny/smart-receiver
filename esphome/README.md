# Smart Receiver – ESPHome component (Phase 3)

ESP32 inside a Yamaha RX-V540RDS:
- **State:** sniffs the IC601 → IC501 (M66003) display bus.
- **Control:** injects Yamaha NEC codes on REM1.

Every command is verified against the display and retried. That replaces the hardware idle-line check.

## Install (ESPHome Builder add-on in HA)

1. Copy the files into HA's config share, via the Samba or File editor add-on:
   ```
   /config/esphome/yamaha-rxv540.yaml
   /config/esphome/components/yamaha_vfd/__init__.py
   /config/esphome/components/yamaha_vfd/yamaha_vfd.h
   /config/esphome/components/yamaha_vfd/yamaha_vfd.cpp
   ```
2. Add these keys to `/config/esphome/secrets.yaml`:
   - `wifi_ssid`, `wifi_password`
   - `yamaha_api_key` (32-byte base64; e.g. `openssl rand -base64 32`)
   - `yamaha_ota_password`, `yamaha_ap_password`
3. Do the first flash over USB. The sniffer firmware has no OTA, so use *Install → Manual download*, then flash the factory image with web.esphome.io or `esptool` on COM7. Every flash after that goes over OTA.
4. HA discovers the device; adopt it with the API key.

## Entities

| Entity | Type | Notes |
|---|---|---|
| Power | switch | Unknown until the first bus traffic or the boot probe. Verified via `F1`/`F0`, 3 tries. |
| Mute | switch | Toggle code. The component only sends it if the state differs, and re-checks before each retry. |
| Volume | number (dB) | −99…0 dB, 1 dB per IR press (observed). Closed-loop stepping, stops at the closest reachable value. |
| Input | select | Verified via the input text/icon, 3 tries. PHONO is shown but has no IR code. |
| Sound program / Sub-program / Input mode / Tuner | text_sensor | Read-only. Input mode = the INPUT MODE key setting (AUTO/…). |
| Display | text_sensor | Raw 14-char VFD text. |
| Bus frames OK/bad/dropped | diagnostic | `bad` > 0 means a missed first bit (ISR latency). Should stay 0. |
| Re-sync state | button | Sends a VOL− "wake" press: learns power + volume. |

## Status (2026-11-20)

- Built with the ESPHome CLI on Windows (`build.ps1`, ESPHome-managed ESP-IDF 5.5.6 in `C:\esphome-idf`, build in `C:\esphome-build`; no spaces in either path).
- Running on the ESP32 (own supply, full TX power), adopted in HA, all entities follow the receiver. Verified: VOL− probe, input restored after restart, push updates.
- Brownout at WiFi start happened only on the monitor's USB port; fixed by the dedicated supply (`output_power` removed).
- Template switches use `restore_mode: DISABLED` (a boot must never send commands). MUTE never toggles while its state is unknown.
- Volume range observed on the display: Mute, −99 … 0 dB, 1 dB steps.
- Entities (current): Power, Mute, Volume, Input, Sound program, Sub-program, Input mode (= INPUT MODE key, AUTO/…), Tuner, Display, Bus trace, diagnostics.
- Scope decision (2026-11-20): no further receiver features. Not planned: 6CH/SLEEP/number-key IR codes, front-panel-only keys (speakers A/B, tone, night, tuner/RDS keys via the ADKEY ladders).
- Open: long-run test (Bus frames bad should stay ~0); Phase 5 install (external antenna: −67 dBm on the bench already).
- Lesson: a loose DB-9 shows as Bus frames OK = 0 and Display "??????????????" (no frames at all).

## Behaviour notes

- **Boot:** 8 s after boot, if power is still unknown, the component sends one VOL− (7A-1B). Normally this only shows the volume; if the volume screen is already up it lowers by 1 dB (never raises). Any display traffic within 1 s = on.
- **Input after reboot:** the last input seen is stored in flash and restored at boot (`restored last input: …`); it stays until the display shows another input. No input command is ever sent at boot.
- **Updates:** push model. The component's `on_change` trigger updates the entities when the decoded state or display text changes; polling is only a 60 s fallback.
- **Volume:** a set request first wakes the volume screen with VOL− (same caveat), then steps toward the target. Refused while muted (IR VOL± does nothing then). Stops when the knob moves against it, or at the closest reachable value.
  - "Limit reached" = two steps without a change.
  - Fails after 4 missed responses.
- **Logs:** every command logs `TX 7A-xx` (DEBUG) and the result (`... done` / `FAILED`) at INFO.

## First-test checklist

1. **Log check:** `dump_config` shows `interrupt allocated (level 3)`.
2. **Front-panel check:** turn the volume knob and switch inputs on the front panel. The Volume/Input entities should follow within ~1 s, and frames bad/dropped should stay 0.
3. **Control from HA:** Power off/on, Input DVD → CD, Mute on/off, Volume −40 → −30.
4. **Cable test:** unplug the DB-9 while running. The device must stay responsive (the ISR gives up after 20 ms if CS is stuck low).
