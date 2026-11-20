# Phase 1 – Probe plan (multimeter + scope + ESP32, no logic analyzer)

Page refs are printed page (PDF page) in the service manual. "Inferred" = not shown in the manual.

## Strategy

- **Tap point:** the solder side of **CB502** on the OPERATION (1) PCB. It's a through-hole connector at the board's left edge, and the silkscreen labels every pin (p. 64 (68)).
- **No probing on a live, open chassis.** Solder fly-leads with the unit unplugged, bring them out through a rear vent to a header, and close the lid. All powered measurements happen on that header.
- **The scope answers the electrical questions:** levels, clock rate, framing, edge.
- **An ESP32 running a small SPI-slave sniffer stands in for the logic analyzer.** It prints each CS frame as hex, which is also the first version of the Phase 3 firmware.

## Board identification and access (p. 18–20 (22–24) disassembly, Fig. 1–5)

| Board | Recognise it by | Relevance |
|---|---|---|
| OPERATION (1) | Behind the front panel, part of the Front Panel Unit: VFD V501, IC501 M66003 behind it, IR receiver U501 behind the window | **CB502 = tap point** (left end, through-hole) |
| FUNCTION (4) | µC IC601 (100-pin QFP) + 16 MHz crystal XL601 (schematic block "601-700 FUNCTION(4)", p. 80 (84)) | **CB605** = other end of the front-panel flex (Fig. 1, right-front) |
| DSP | On a metal "Support/DSP" bracket, CB602/CB603 (Fig. 2) | — |
| FUNCTION (1)–(7) + tuner | Boards on the rear-panel jacks (Fig. 4) | Volume ICs IC302/IC303 (contingency only) |
| MAIN (1)/(3) | Power-amp heatsink, big filter caps, regulators IC402–IC406 | Keep clear |
| POWER (1)/(2) | Mains inlet, transformers, RY701, F701/F702, T701 | **Mains side: hands off** |

- **Access:** CB502's solder side faces the inside of the Front Panel Unit. To solder there, remove the Front Panel Unit: 5 screws ③, loosen the harness fixture, then unplug CB305, CB306, CB605, CB612 (p. 18 (22)).
- **Alternative:** tap the flex's other end at CB605 on FUNCTION (4), if its pins are reachable from the top. In that case, confirm the signal order there with A1 first.
- **Identify the flex:** follow the wide flat cable from behind the display. The OPERATION end is CB502, the main-board end is CB605.

## CB502 physical order (top → bottom on the component side, p. 64 (68))

HPMT · VRB · VRA · /HP · ISB · ISA · **REM1** · PSW · /BLK · **CKFD** · **DTFD** · **CEFD** · ADKEY1 · +5M · ADKEY0 · **VP (−28 V)** · **MG**

⚠ **VP sits right next to MG and ADKEY0.** A solder bridge there shorts the VFD supply to logic ground. Check every joint under magnification and buzz VP to its neighbours before first power-up.

## Fly-leads

| Lead | Tap (CB502 pad) | Series R at the tap | Purpose |
|---|---|---|---|
| MG ×2 | MG | — | Ground: one return per signal pair, and the scope ground |
| CKFD | CKFD | 220 Ω | Display clock |
| DTFD | DTFD | 220 Ω | Display data |
| CEFD | CEFD | 220 Ω | Display /CS |
| REM1 | REM1 | 220 Ω | IR line (sense now, inject in Phase 2) |
| +5M | +5M | 1 kΩ | Measurement only: never draw power from it |
| /BLK (optional) | /BLK | 220 Ω | Seeing driver reset at power-up |

### Cable: UTP (4 pairs) + DB-9

Receiver side: female (sockets) DB-9. ESP side: male. Shell to MG at the receiver end, or unconnected. Label it "Yamaha display tap – NOT RS-232".

| DB-9 pin | Signal | UTP pair | If mis-plugged into an RS-232 DTE port |
|---|---|---|---|
| 1 | REM1 | 4a | DCD input: harmless |
| 2 | DTFD | 3 | RXD input: harmless |
| 3 | MG | 3 (return) | TXD output → ground (driver is short-circuit limited) |
| 4 | MG | 1 (return) | DTR output → ground |
| 5 | spare / +5M via 1 kΩ (separate wire) | — | GND ↔ 1 kΩ: harmless |
| 6 | CEFD | 2 | DSR input: harmless |
| 7 | MG | 2 (return) | RTS output → ground |
| 8 | **CKFD** | 1 | CTS input: harmless |
| 9 | /BLK | 4b | RI input: harmless |

- CKFD (pin 8) neighbours are 3, 4, 7 (MG) and 9 (/BLK).
- **Pairing REM1 with /BLK is acceptable.** /BLK is static except at power transitions and filtered by C512 0.1 µF / R531 220 k at IC501 (p. 83 (87)), so REM1 edges can't pulse it. /BLK only moves at power on/off, when a stray IR edge doesn't matter.
- **Not protected:** a DCE device would drive pins 1, 2, 6, 8, 9 with ±12 V.

- **Series resistors at the tap end:** if anything outside the case gets shorted, the µC and IR module are protected. They also damp ringing on ~50 cm of wire.
- **Twist pairs:** CKFD with one MG, the other MG with DTFD/CEFD. Use thin wire and route clear of the heatsink and the transformer.
- 220 Ω in series doesn't matter at these rates (inferred: the clock should be ≤ a few MHz).

## Step A – unpowered (multimeter)

Start with safety, in this order:
1. Unplugged for over 5 minutes.
2. Discharge and measure the big caps on the main supply (MAIN board C417–C422, 6800–10000 µF, ±33 V class rails, p. 85 (89)) and C701 (14 V standby, p. 87 (91)).
3. Don't touch the POWER board primary side: mains inlet, voltage selector, F701/F702, RY701 contacts, TE701 (p. 87 (91)).

Then measure:

| # | Measurement | Expect | Why |
|---|---|---|---|
| A1 | Continuity, each CB502 pad ↔ the matching CB605 pad on FUNCTION | Same signal at both ends | Confirms the flex mapping, since pin numbers were only inferred |
| A2 | MG ↔ each mains plug pin (power switch in both positions) | Open (> 20 MΩ) | Logic side is isolated from mains, so earthing the scope/ESP to MG is safe |
| A3 | MG ↔ chassis/rear-panel screw | ~0 Ω (confirmation) | The manual says boards ground to the chassis via the rear panel and must be jumpered to chassis if removed (p. 19–20 (23–24)). So MG = chassis. Note this for the ground-loop question once the ESP connects to an earthed PC |
| A4 | VP pad ↔ MG / ADKEY0 / +5M after soldering | Not a short | Bridge check |

### Step A results (2026-11-16)

- **Access:** CB502's foil side is reachable from the top with the front panel unit in place (photo). The "Lead Type Device" layouts in the manual are foil-side views.
- **A1:** CB502 ↔ CB605 is straight-through, HPMT end ↔ HPMT end. This matches p. 64 (68) and the FUNCTION (4) foil-side layout.
  - Still to do: A1b, identifying each tapped pad against a landmark part (below).
- **A2:** MG ↔ mains pins open. ✔
- **A3:** MG ↔ chassis 0 Ω. ✔
- The heavy blue/brown wires next to CB502 are cable guides, not connected to anything (checked by Martijn). Not mains.
- **A1b done (2026-11-19):** all tapped pads buzzed against IC601. VP ↔ MG ≈ 10 MΩ: no bridge.

**A1b – pad identity check (unplugged)**

| Pad | Buzz to | Expect |
|---|---|---|
| MG | Chassis | 0 Ω |
| VP | IC501 pin 64 | 0 Ω; and open to MG |
| CEFD | IC501 pin 2 | Low (series 470 Ω R533/R534); high to pins 3/4 |
| CKFD | IC501 pin 3 | Low (series 470 Ω R535); high to pins 2/4 |
| DTFD | IC501 pin 4 | Low; high to pins 2/3 |
| REM1 | U501 OUT (the schematic symbol is VCC-G-OUT; physical pin order per the Sharp GP1UM27x datasheet) | 0 Ω |
| /BLK (if tapped) | IC501 pin 1 | ~470 Ω (R530) |

- **B1 done (2026-11-20):** DC levels on the DB-9 match the expected idle levels. Lesson learned: scope probes don't reach into female DB-9 sockets reliably, so measure on a mating connector.

- **B2 done (2026-11-20):** REM1 idles at 4.77 V (DMM) and is active-low to 0 V.
  - Any NEC remote shows a clean 9 ms / 4.5 ms leader + 0.56 ms bits.
  - Fall ≈ 0.2 µs (active pull-down).
  - **Rise 10→90 % ≈ 10 µs**, RC-shaped, so a passive, weak pull-up. Inferred ~50–90 kΩ with ~50–80 pF of line + cable + probe.
  - Consequences:
    - **Never put a resistor divider on REM1.** 5.5 kΩ to GND would hold it at ~0.35 V, and the µC would see permanent IR.
    - Read it only through a 3.3 V-powered 74LVC1G17/1G14 (5 V-tolerant input, Ioff so it doesn't load the line when the ESP is unpowered).
    - Inject with an open-drain N-MOSFET (2N7002/BSS138) with a gate pull-down.
    - Keep the final REM1 wire short; it's a high-impedance node.
  - Scope note: vertical gain reads ~25 % low, so use the DMM for absolute levels.

- **B5 done (2026-11-20):** CKFD ≈ 500 kHz (2 µs period, ~50 % duty), idles high.
  - DTFD changes on the CKFD **falling** edge and is stable ~1 µs before each rising edge → **SPI mode 3 confirmed**.
  - 8 clocks per byte; byte gaps ~23–65 µs.
  - Tentative first bytes of a frame: `FA 00 20 20` (write display RAM from addr 0, spaces).

- **B3 done (2026-11-20):** CEFD goes low **once per command burst** (~0.8 ms window at 1 ms/div), with ~5 byte bursts of CKFD inside, and returns high ~0.2 ms after the last byte. One CS frame = one command, so each sniffer log line will be a complete command. Frames are short (≪ 64 bytes).

- **B4 (2026-11-20, partial):** CEFD frames are irregular and clustered while the volume knob turns: ~15–17 frames on screen (~8/s if 0.2 s/div). Shortest gap ≈ 0.14 div (≈ 27 ms at 0.2 s/div) → the ESP has milliseconds to requeue.
  - The varying spike depths are under-sampling of ~0.8 ms pulses, not real amplitude.
  - Open: is there a periodic refresh with a static display? The sniffer log's dt column answers it.

- **C – first sniffer capture (2026-11-20): gate effectively passed.**
  - Protocol: `FA <addr>` is its own CS frame; the next CS frame carries ASCII characters written from that address with auto-increment. The display has 14 character positions, 0x00–0x0D.
  - Volume up gave `FA 00` + "VOLUME …" (14 chars), then `FA 07` + "-49.0dB" (positions 7–13).
  - ~3.0 s later: `FA 00` + "  6ch Stereo  " (reverts to the DSP program name). Nothing else was sent in the following ~4 s, so the display is written on changes only (no periodic refresh seen).
  - Glitch: one data frame was 111 bits (first bit lost). Cause (inferred): SPI slave ISR re-arm delayed by UART print critical sections on the same core. Fix: ISR pinned to core 1 (`isr_cpu_id`).

**A1b alternative – buzz against IC601 (µC, FUNCTION (4), reachable without removing anything)**

IC601 is a 100-pin QFP: pins 1–30 on one long side, 31–50, 51–80, then 81–100, counter-clockwise viewed on the marking. Pins 1 and 100 meet at the pin-1 corner (p. 46 (50)).

| Pad | IC601 pin | Expect |
|---|---|---|
| DTFD | 1 (SOUT4) | ~0 Ω (no series R on FUNCTION side, inferred) |
| CKFD | 2 (CLK4) | ~0 Ω |
| /BLK | 3 (P94) | ~0 Ω |
| CEFD | 100 (P97) | ~0 Ω |
| PSW | 19 (INT1) | ~0 Ω |
| REM1 | 20 (INT0) | ~0 Ω |
| ADKEY0 / ADKEY1 | 94 / 95 | ~100 Ω (R845 / R844) |
| MG | 14 or 64 (Vss) | 0 Ω |
| VP | — | Open to every IC601 pin and to MG; 0 Ω to the VP pin of CB602 (FUNCTION (4) left edge, next to MG) |

+5M does **not** buzz to IC601 Vcc (pin 16 = +5BU, separated by D606/D607, p. 80 (84)).

## Step B – powered, lid closed (scope on the header)

Scope ground clip goes on **MG only**. Proceed only if A2 showed open.

| # | Setup | Measure | Decides |
|---|---|---|---|
| B1 | DC, both power states | +5M, and idle levels of CKFD / CEFD / DTFD / REM1. Is there display traffic in standby? | Level shifting, standby behaviour |
| B2 | CH1 REM1, 2 V/div, 2 ms/div, single, falling trigger; press VOL+ on the remote | 9 ms low / 4.5 ms high leader, bit widths (0.56 / 1.69 ms), low level; **10–90 % rise time** | Confirms NEC on the line; rise time gives the module's pull-up strength (for Phase 2) |
| B3 | CH1 CEFD (trigger, falling), CH2 CKFD, normal trigger | SCK frequency; clocks per CS-low (÷8 = bytes); CS→first-clock setup; gaps between bytes | SPI-slave feasibility and buffer sizes |
| B4 | Same as B3, zoomed out to 50–200 ms/div; hold VOL+ | Burst rate: periodic refresh or only on change? | Shadow-RAM strategy |
| B5 | CH1 CKFD, CH2 DTFD | Data changes on the falling edge and is stable at the rising edge | Confirms SPI mode 3 |
| B6 | If the scope has SPI decode: mode 3, MSB first, CS = CEFD | Read the bytes of one VOL+ burst directly | Shortcut to the gate |

B3–B5 answer everything the logic analyzer would have, except bulk byte capture. That's the ESP's job.

## Step C – ESP32 as sniffer

- **Levels:** CKFD/DTFD/CEFD are 5 V CMOS. Use a divider per line (2.2 kΩ top / 3.3 kΩ bottom ≈ 3.0 V at the ESP pin). The ESP32 is not 5 V-tolerant.
- **No divider on REM1:** its pull-up is the IR module's own (strength unknown until B2), and a few-kΩ divider could drag it low. Read REM1 later through a 74LVC buffer at 3.3 V (5 V-tolerant inputs) or not at all in Phase 1.
- **Power-up order:** ESP on USB first, then the receiver. This avoids back-powering the ESP through its clamp diodes.
- **Ground:** the ESP's USB ground ties MG to PC earth. That's fine on the bench if A2 passed. A laptop on battery removes the loop entirely.
- **Firmware:** SPI slave, mode 3, CS = CEFD, DMA, large queued buffers. One line per CS frame: `t_us len: hex…`, at 921600 baud. Check ESP-IDF's "SPI Slave – Restrictions and Known Issues" for your chip variant before trusting mode 3 + DMA.

## Step D – capture script (one change per capture)

1. Power on from standby (catch the init sequence).
2. VOL+ ×1, VOL− ×1, hold VOL+ for 2 s.
3. MUTE on/off.
4. Each input: CD, TUNER, MD/CD-R, DVD, D-TV/CBL, V-AUX, VCR, 6CH.
5. DSP programs: step through all.
6. Tuner preset and frequency change.
7. Standby.
8. DIAG → menu 7 DISPLAY CHECK: all off, all on, dimmer, lattice. Set menu 9 to **PRESET INHIBITED** before exiting (p. 23 (27)).
9. **Volume visibility test** (decides the volume-IC tap):
   - Change the volume, then keep logging for 10 s without touching anything. Does the dB value stay on the display or time out to the input/DSP name?
   - Standby → power on. Is the restored volume written to the display at power-on?

## Volume IC bus tap: contingency only

The M66003 display bus already carries every volume change the user sees, so the tap on DTBD/CKBD (µC pins 59/58 → IC302/IC303, p. 79–80 (83–84)) is dropped from the default plan. It would add:
- two wires soldered at fine-pitch ICs on FUNCTION (not on the flex);
- a ROHM serial decoder (part identity unclear: BD3815KS in the parts list, "BD3813KS" on the schematic).

Use the D9 result to decide:

| D9 result | Action |
|---|---|
| dB value appears on every change and at power-on | No tap. Track volume from the display writes |
| Appears on change but not at power-on | No tap. Firmware sends VOL+ then VOL− once after power-on to force a display update (±0.5 dB blip) |
| Volume changes without any display write | Only then consider the tap |

## Gate (go/no-go)

From a sniffer log alone, you can print the 14-character text and name the icon writes. Expected per the emsyscode M66003 driver: `0xFA <addr> <ASCII…>` for text, `0xFC <0x90–0x9F> <5 bytes>` for glyphs.
