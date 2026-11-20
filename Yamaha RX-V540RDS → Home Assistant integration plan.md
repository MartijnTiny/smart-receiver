# Yamaha RX-V540RDS → Home Assistant integration plan

Nov 15, 2026 · @Martijn van der Linden

## Goal and scope

Put an ESP32 inside the receiver so Home Assistant gets two-way control with real state. Commands go in on the IR receiver's signal line, and state is read back from the front-panel display bus. The receiver has no RS-232 port and no network, so this is the only route to real state.

Done means:

- HA shows a `media_player` entity whose power, volume, mute, input and DSP program match the front panel within about 1 s, including after the original remote is used.
- HA can set power, volume (step and target value), mute, input and DSP program.
- The original remote and front-panel buttons still work.
- The mod can be removed without a trace: no cut traces, nothing on the mains side.

Out of scope: audio streaming, and Zone 2 (check whether this model even has it).

## Architecture

&#91;embedded content: ESP32 inside the receiver · control in, state out\]

Commands enter on the IR receiver's output line, so the receiver's own micro can't tell them from the real remote. State comes from passively reading what the micro writes to the display driver. The volume IC tap is a fallback for exact volume only.

## Phase 0 – Desk research in the service manual

Before opening the case, pull every part number and test point you'll need from the [service manual](https://www.manualslib.com/manual/951850/Yamaha-Rx-V540.html). It covers the RX-V540/540RDS/440 family.

- [ ] System microcontroller: part number, pinout, which pins drive the display driver, and the pin that takes the remote-sensor input
- [ ] Display driver chip (on the front PCB next to the VFD): part number and datasheet. Note its serial interface (clock/data/strobe), command set and memory map.
- [ ] IR receiver module: its output pin, pull-up value and supply voltage (likely 5 V)
- [ ] Electronic volume IC(s): part number and control bus. This is an optional second source for the exact volume.
- [ ] Standby supply: which rail is live in standby, its voltage, and how much current is spare after the micro, VFD filament off, relay coil
- [ ] Ground reference: is digital ground tied to chassis, and is anything on the logic side referenced to the primary side?
- [ ] Front-panel key matrix: lines and levels, in case IR injection doesn't work
- [ ] Board-to-board connectors between front PCB and main PCB, the cleanest place to tap without soldering to fine-pitch pins
- [ ] Self-test or service mode (manual's "self-diagnosis" section): it may help you trigger every display segment

Output of this phase: one page listing pins and test points, and the display driver datasheet saved locally.

## Phase 1 – Bench reconnaissance

This phase answers one question: how hard is the display bus to decode? Everything after it depends on the answer.

1. With the receiver unplugged and the caps discharged, solder thin wires (or use test clips) to clock, data, strobe and GND of the display driver, and to the IR receiver output.
2. Power it up and capture with a logic analyzer (sigrok/PulseView), at least 10× the bus clock.
3. Capture one known state change at a time: volume up 1 step, mute, each input, each DSP program, standby on/off, tuner preset.
4. Decode the frames against the datasheet: find the address/command bytes, the full refresh vs. partial writes, and the refresh rate.
5. Build a segment map: for each character position, which bits light which segments. Then map segment patterns to characters (font table).
6. Capture the IR line while pressing the remote: confirm it's NEC, note the logic level and idle state, and record the codes.

Exit criterion: given a raw capture, you can print the display text by hand. If the driver is a common 16-segment VFD controller, expect a day or two. If the micro drives the VFD grids directly with no driver chip, fall back to sniffing the volume IC bus plus the IR line (see Risks).

## Phase 2 – Control path (IR-line injection)

The ESP32 drives the IR receiver's output line directly, so the receiver's micro sees a perfect remote signal with no optics involved.

- **Electrical:** IR receiver modules have an open-collector-like output with a pull-up, idling high. Drive the line low through an N-MOSFET or NPN (open-drain) from an ESP32 GPIO. Don't push-pull 3.3 V onto a 5 V line. Add a series resistor (\~1 kΩ) at the module side if the module's output is push-pull.
- **Coexistence:** The real remote and the ESP32 share the line (wired-OR when low). Only transmit when the line has been idle for more than about 100 ms to avoid corrupting a real remote frame.
- **Signal:** Generate demodulated NEC (no 38 kHz carrier, since you're past the demodulator) with the RMT peripheral. Invert relative to the IR LED convention: active-low.
- **Codes:** Use the codes captured in Phase 1. Also look for Yamaha's *discrete* codes (separate power on / power off, direct input select). Community IR code lists for Yamaha RX-V receivers usually have them, but verify each on this unit.
- **Test:** From an ESPHome button, send power, volume up/down, mute, input select. Confirm with the display.

Exit criterion: every command you plan to expose works 100 times in a row with no misses.

## Phase 3 – State decoding firmware

The ESP32 listens passively on the display bus, rebuilds a shadow copy of the display RAM, and turns it into text and structured state.

1. **Capture:** Use SPI in slave mode, with strobe as chip-select if the framing fits. Otherwise use I2S/RMT or a tight GPIO interrupt on the clock edge. Level-shift 5 V → 3.3 V with a resistor divider or a 74LVC buffer. These are inputs only, never driven.
2. **Shadow RAM:** Apply each write to a local copy of the driver's display memory, honouring address auto-increment and partial writes.
3. **Render:** Run the segment map to characters to get a display string per refresh. Also decode indicator icons (MUTE, input, decoder, speaker LEDs) from their bits.
4. **Parse:** Use regex/state rules on the string, e.g. a volume pattern like `-35.5dB`, input names, DSP program names, `MUTE`. Note that the display shows transient messages for a few seconds after a key press, so keep the last *stable* value for each field.
5. **Standby:** Display blank or the driver's display-off command means standby. Cross-check with the power relay line or a standby LED if one is reachable.
6. **Debounce/publish:** Publish a field only after it has been stable for about 200 ms. Expose the raw display string as a diagnostic sensor; it's invaluable for debugging.

Useful extra: closed-loop volume. Set volume to X by reading the current dB and sending up/down steps until it matches, with a timeout.

## Phase 4 – ESPHome and Home Assistant

Package Phases 2–3 as an ESPHome external component so HA gets native entities over the API, with no MQTT glue.

| Entity | Type | Source | Notes |
| --- | --- | --- | --- |
| Receiver | `media_player` | display + IR | power, volume, mute, source list, sound mode |
| Volume | `sensor` (dB) | display | exact dB for automations |
| Input | `select` | display + IR | discrete codes if available, else step through inputs until the display matches |
| DSP program | `select` | display + IR | same approach as input |
| Display text | `text_sensor` | display | diagnostic |
| Tuner frequency/preset | `text_sensor` | display | only when input is TUNER |

- The `media_player` volume maps dB to 0–1. Pick the range you actually use (e.g. -60 to -10 dB) rather than the full scale.
- Commands are queued and paced (NEC frame plus a gap). Long-running actions like "set input" run as a small state machine that verifies against the display.
- OTA updates from day one, so the case only needs to be opened once.

## Phase 5 – Hardware integration

Turn the bench wiring into a small permanent board that can be removed in five minutes.

- **Board:** A small perfboard or a one-off PCB with an ESP32-C3/S3 module, an open-drain IR driver, a 5 V → 3.3 V input buffer, and a 3.3 V LDO or buck converter.
- **Power:** Take it from the standby rail only if Phase 0 shows at least 300 mA spare (WiFi TX peaks). Otherwise use a USB supply fed through a vent or the rear panel, with a common ground. A buck converter beats an LDO if the rail is higher than 5 V.
- **Taps:** Solder to board-to-board connector pins or test pads, not fine-pitch IC legs. Use a JST connector on the ESP board so it unplugs cleanly.
- **Placement:** Low-voltage side, away from heatsinks and the transformer. Keep the antenna clear of the metal lid: the steel case will kill WiFi, so plan an external antenna (u.FL) through a rear vent or a plastic front-panel area.
- **Reversibility:** No trace cuts. Photograph everything before you start.

## Bill of materials and tools

| Item | Purpose |
| --- | --- |
| ESP32-C3 or ESP32-S3 module, u.FL antenna variant | controller, WiFi outside the steel case |
| u.FL → RP-SMA pigtail + small antenna | WiFi through a rear vent |
| 2N7002 or BSS138 N-MOSFET | open-drain IR line driver |
| 74LVC245 / 74LVC1G17, or resistor dividers | 5 V → 3.3 V inputs from the display bus |
| Buck converter or LDO (rail dependent) | 3.3 V supply |
| JST-XH or PH connectors, thin silicone wire | removable taps |
| Logic analyzer (8 ch, sigrok-compatible) | Phase 1 captures |
| Multimeter, and a bench supply with current limit | rail checks, first power-up |
| IR receiver module + spare ESP32 (optional) | capture remote codes off-board |

## Risks, fallbacks and safety

| Risk | Fallback |
| --- | --- |
| No separate display driver (the micro multiplexes the VFD itself) | Sniff the electronic volume IC bus for volume; infer input/DSP from the IR commands seen on the line (the ESP sees the real remote too) |
| Display bus too fast or awkward for SPI slave | Capture with I2S in parallel/LCD mode or the RMT peripheral; or use an RP2040 PIO as a sniffer, sending decoded text to the ESP over UART |
| Front-panel-only actions not visible on the IR line | Accept it: the display sniff still sees the result |
| IR injection collides with the real remote | Idle-line check before sending, plus a retry if the display doesn't confirm |
| Standby rail can't supply the ESP | External USB supply with shared ground |
| WiFi weak inside the case | External antenna; or ESP32 with Ethernet (e.g. WT32-ETH01), since the receiver sits near the router anyway |
| Ground loop or hum from the shared ground | Power the ESP from the receiver's own rail, not an external supply, so there is no second ground path |

**Safety:** The power supply's primary side and the big filter capacitors are dangerous. Unplug the receiver, wait, and measure the rail caps before touching anything. Never work on the low-voltage side with the lid off and mains connected unless you're isolated (an isolation transformer). Keep all added wiring well away from the transformer, mains switch and fuse area.

## Milestones and open questions

Rough effort for one person in evenings. Phase 1 is the gate: if the display bus decodes cleanly, the rest is routine ESPHome work.

1. **M0 – Pin map done** (Phase 0): 1 evening
2. **M1 – Display text printed from a capture** (Phase 1): 2–4 evenings, the go/no-go point
3. **M2 – IR injection works from an ESPHome button** (Phase 2): 1–2 evenings
4. **M3 – Live display text in HA** (Phase 3): 2–3 evenings
5. **M4 – Full `media_player` with closed-loop volume and input** (Phase 4): 2–3 evenings
6. **M5 – Permanent install, lid closed, OTA only** (Phase 5): 1–2 evenings

Open questions:

- [ ] Which display driver is on the front PCB, and is its protocol documented?
- [ ] Does Yamaha have discrete power-on/off and input codes for this model?
- [ ] Is there enough spare current on the standby rail?
- [ ] Does WiFi reach from the receiver's spot, or is Ethernet simpler?
