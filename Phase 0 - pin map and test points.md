# Phase 0 – Pin map and test points (RX-V540RDS)

Source: Yamaha service manual RX-V540/540RDS/440 family (manualslib export). Page refs are **printed page (PDF page)**. "Inferred" = not shown explicitly in the manual.

## Headline findings

1. **The display driver is a character controller, not a segment driver.** IC501 = **M66003-0101FP** (Mitsubishi/Renesas) on the OPERATION PCB: 5×7-dot VFD controller with CGROM (35 bit × 166), CGRAM (35 bit × 16), display-code RAM (8 bit × 60), 8-bit serial input on /CS, SCK, SDATA (block diagram, p. 83 (87)). The µC sends **character codes**, so for the 14 text positions you decode bytes to characters directly. No segment map is needed there. That makes the Phase 1 gate much easier than planned.
2. **Every signal we need is on one 17-pin FFC** between FUNCTION (CB605) and OPERATION (CB502): the display bus, IR remote, standby key, key ADC lines, +5M and MG. That's the tap point. There's no need to touch fine-pitch IC legs.
3. **The remote code table is in the manual** (p. 138–139 (142–143)), and it includes **discrete codes**: separate STANDBY / SYSTEM POWER (on) codes and a direct code for each input.
4. **The standby rail can't feed an ESP32.** The 5 V standby rail comes from IC602 **NJM78L05A** (a 100 mA-class part), which already powers the µC and the IR module.

## IC / part summary

| Function | Ref | Part | Board | Manual |
|---|---|---|---|---|
| System µC | IC601 | M30626 (schematic) / M3062VMGP (IC data), M16C/62, 100-pin, 16 MHz (XL601) | FUNCTION | p. 46–47 (50–51), schematic p. 80 (84) |
| VFD controller | IC501 | M66003-0101FP | OPERATION | p. 83 (87) |
| VFD tube | V501 | HNA-16MM43, 16 grids, 38 anodes | OPERATION | p. 44–45 (48–49) |
| IR receiver | U501 | Sharp GP1UM271XK | OPERATION | p. 83 (87) |
| 7ch volume | IC302 | BD3815KS (parts/IC list). The schematic label reads "BD3813KS 7ch VOLUME", so check the chip marking | FUNCTION | p. 79 (83) |
| 2ch volume (LFE) | IC303 | BD3812F-E2 | FUNCTION | p. 79 (83) |
| Input selector | IC301 | BD3841FS | FUNCTION | p. 79 (83) |
| Standby 5 V reg | IC602 | NJM78L05A | FUNCTION | p. 80 (84) |

## Tap point: CB605 (FUNCTION, Molex 52045) ↔ CB502 (OPERATION, 52492-1720), 17-pin FFC

The signal order is from the schematics (p. 80 (84) bottom-left, p. 83 (87) bottom). **Pin numbers are inferred.** The pin-1 marker (◎) sits at the MG end on CB605 and at the HPMT end on CB502, so check with a continuity test before relying on the numbers.

| # (inferred, CB605) | Signal | µC pin | Idle level (manual) | Notes |
|---|---|---|---|---|
| 1 | **MG** | — | 0 V | Logic ground. ESP ground goes here |
| 2 | VP | — | −28 V at IC501 pin 64 | VFD negative supply. **Never connect to the ESP or the LA** |
| 3 | ADKEY0 | 94 (AN2) via R845 100 Ω | — | Front-key resistor ladder, ADC |
| 4 | **+5M** | — | 4.9 V | 78L05 → D607. Powers IR module and IC501 logic |
| 5 | ADKEY1 | 95 (AN1) via R844 100 Ω | 4.8 V | Second key ladder |
| 6 | **CEFD** | 100 (P97, GPIO) | 4.9 V | M66003 /CS (active low, despite the name) |
| 7 | **DTFD** | 1 (SOUT4) | 0 V | Serial data (UART4, clock-synchronous mode) |
| 8 | **CKFD** | 2 (CLK4) | 4.9 V | Serial clock, idles high |
| 9 | /BLK | 3 (P94) | 4.0 V at IC501 | Goes to IC501 /RESET via R530 470 Ω; R531 220 k / C512 to GND. Low = driver reset/blank |
| 10 | **PSW** | 19 (INT1) | — | Standby/On key |
| 11 | **REM1** | 20 (INT0) | 4.8 V | IR module OUT, direct, no series R on either board |
| 12 | ISA | 4 | — | Input-selector encoder A |
| 13 | ISB | 21 | — | Input-selector encoder B |
| 14 | /HP | 23 via R843 100 Ω | — | Headphone detect |
| 15 | VRA | 24 | — | Volume encoder A |
| 16 | VRB | 25 | — | Volume encoder B |
| 17 | HPMT | 50 | 0.1 V | Headphone mute |

The OPERATION side has 470 Ω series resistors R533–R535 on the CE/CK lines near IC501 (p. 83 (87)). Phase 1 should probe on the FFC or at the µC side.

## Display bus (M66003)

- IC501 pins: 1 /RESET, 2 /CEFL, 3 CKFL, 4 DTFL, 5 VSS, 6 XOUT, 7 XIN (R532 27 k / C513 100 pF RC osc), 8 VDD 4.9 V, 64 VP −28 V (p. 83 (87)).
- 5 V CMOS levels, driven only by the µC (one-way, no read-back). The ESP only listens.
- **Datasheet:** there's no public M66003 datasheet. The best reference is a working Arduino driver for an M66003 on a Yamaha RX-463 panel: [emsyscode/M66003](https://github.com/emsyscode/M66003), `M35500.ino`. **Do not use the M66004 command list.** The M66003 command bytes are different (0xF*), which matches the EEVblog thread's finding that extracted commands don't match the M66004 list.
- **Serial format (from that driver; confirm on the first capture):** 8 bits, MSB first. Data is sampled on the SCK rising edge, and SCK idles high. That's **SPI mode 3**, which fits CKFD idling at 4.9 V on the schematic. /CS (CEFD) goes low around a whole command burst: the command byte, then its arguments or characters.
- **Command bytes used by that driver** (value → apparent meaning, inferred from the driver code):

| Byte | Following bytes | Meaning |
|---|---|---|
| 0xF8 | 1 | Digit/segment configuration |
| 0xF9 | 1 | Dimmer (0x70 used) |
| 0xF5 | — | Write mode, auto-increment |
| 0xF6 | 1 | Number of active digits |
| 0xFB | 1 | Set RAM address |
| 0xFA | addr, codes… | **Write display RAM from addr, auto-increment.** This is the one we decode |
| 0xFC | addr 0x90–0x9F, 5 bytes | Write CGRAM glyph (5 columns × 7 bits = 35 dots) |
| 0xF0 / 0xF2 / 0xF1 | — | All off / all on / normal display |
| 0xF3 | — | Reverse |

  Display RAM addresses 0x00–0x11 are digit positions. 0x70–0x80 also exist (icon or extended RAM, purpose unconfirmed). Character codes are ASCII for text, and 0x90–0x9F select CGRAM glyphs. The RX-463 uses the same chip with possibly a different mask ROM (-0101 here), so expect the same command set but verify the font for codes above 0x7F.
- **Unknown until measured:** the SCK rate. UART4 is in clock-sync mode from a 16 MHz CPU, so it could be anywhere from a few hundred kHz to a few MHz. Sample at 24 MS/s or more.

### VFD layout (V501 HNA-16MM43, p. 44–45 (48–49))

| Grid | Content |
|---|---|
| 1G–14G | 14 × 5×7-dot characters (anodes P1–P35 = dots 1-1…5-7). 1G also has icon T1 on P36 |
| 15G | S2 (P1), MD/CD-R (P3), TUNER (P4), CD (P5), PHONO (P6), bar V1–V10 (P20–P29) |
| 16G | R1–R5 (P21–P25), S1 (P26), VCR (P28), VCR2/DVR (P29), VCR1 (P30), V-AUX (P31), D-TV/CBL (P32), DVD (P33) |

On grids 15G/16G the icons are probably sent as CGRAM patterns (0xFC writes) or via the 0x70–0x80 RAM range, not ROM codes (inferred). Expect a small bit-map table for these two positions only.

## IR receiver / injection point

- U501 GP1UM271XK: VCC 4.9 V (+5M), OUT 4.8 V idle, C505 10 µF decoupling. OUT goes straight to REM1 → µC INT0 (pin 20). The manual shows no pull-up on FUNCTION, so the pull-up is the module's own (verify the output stage in the Sharp datasheet).
- Injection: N-MOSFET (2N7002/BSS138) open-drain to MG on REM1. Gate from the ESP GPIO with ~1 k series and 100 k pull-down, so it stays off during ESP boot. Sense REM1 through the 3.3 V buffer for the idle-line check.

### Remote codes (AMP column; Yamaha notation custom–data, p. 138–139 (142–143))

| Key | Code | | Key | Code |
|---|---|---|---|---|
| STANDBY (off) | 7A-1E | | VOLUME + | 7A-1A |
| SYSTEM POWER (on) | 7A-1D | | VOLUME − | 7A-1B |
| CD | 7A-15 | | MUTE | 7A-1C |
| TUNER | 7A-16 | | SLEEP | 7A-57 |
| MD/CD-R | 7A-C9 | | 6CH INPUT | 7A-87 |
| DVD | 7A-C1 | | VCR | 7A-0F |
| D-TV/CBL | 7A-54 | | V-AUX | 7A-55 |
| (key K51, unlabelled; likely PHONO) | 7A-13 | | AMP-mode 1…8 / 9 / 0 (one table only) | 7A-88…8F / 7A-90 / 7A-95 |

The manual has two remote tables (RAV246/RAV247) and the extraction doesn't make clear which page belongs to which. The codes above match in both. Yamaha writes codes MSB-first and NEC sends LSB-first, so expect them bit-reversed on the wire. Confirm on the first capture.

## Electronic volume bus (optional)

- µC pin 59 **DTBD**, pin 58 **CKBD** (idle 0 V, 5 V CMOS) → IC302 DA pin 13 / CL pin 14, and IC303 DTBD pin 9 / CKBD pin 8 (p. 79 (83), p. 80 (84)). This is ROHM's 2-wire data+clock format with a latch at the end of the frame. Get the frame format from the BD3815KS (or BD3813KS) datasheet.
- These lines are not on the FFC. Tapping them means soldering at IC302/IC303 on FUNCTION. Keep this as the fallback only.

## Power and standby budget

| Rail | Source | Value | Live in standby? |
|---|---|---|---|
| Unregulated | T701 standby transformer (13.9 VAC) → D704/705/707/708 → C701 470 µ/25 | 14.0 V | Yes. Also feeds relay RY701 coil via Q701 |
| S12 | Q702/Q703 regulator on POWER board → CB701 → CB602 | ~13.4 V | Yes |
| +5 V (pre-diode) | IC602 NJM78L05A (GND lifted by D603) | 5.6 V | Yes |
| +5M | via D607 1SS355 | 4.9 V | Yes (IR module, VFD controller logic) |
| +5BU | via D606 1SS355 | 4.9 V | Yes (µC Vcc pin 16, backup) |
| +12/−12/+5/−5/+5D | IC402–406 on MAIN, from main transformer | — | No (switched by the power relay) |

Pages: POWER p. 87 (91), FUNCTION p. 80 (84), MAIN p. 85 (89).

- **The ESP can't run from +5M.** The 78L05 is a 100 mA-class part and already carries the µC, IR module and IC501 logic, while ESP32 WiFi TX peaks are about 300 mA or more.
- **S12** is the only candidate rail. The manual gives no rating for T701's secondary current. Decide after one measurement: S12 sag/ripple at C701 with the relay energised plus a 250 mA test load. Otherwise use an external 5 V supply with its ground tied at MG.
- VP (−28 V) and the filament supply come via CB602 from MAIN. Whether the VFD is fully powered in standby is not determined from the manual, so check it in the Phase 1 standby capture.

## Front-panel keys (fallback control path)

Two resistor ladders (R510 / R518 10 k pull-ups to +5M; steps R511 1 k, R512 1 k, R513 1.5 k, R514 2.2 k, R515 3.3 k …), with SKQNAA switches to MG, feed ADKEY0/ADKEY1 → µC AN2/AN1 (p. 83 (87)). Two ways to emulate a key:
- pull the ADKEY line to the key's ladder voltage through the matching resistor with an analog switch;
- for standby only, pull PSW low with open-drain.

## Self-diagnosis (DIAG): useful for Phase 1

- Entry: hold **6CH INPUT + STEREO** on the front panel and press **STANDBY/ON** (p. 23 (27)).
- Menu 7 DISPLAY CHECK: all segments off, all on at 100 %, all on at 50 % dimmer, lattice (p. 25, 34 (25, 38)). This is the fastest way to capture the dimmer command and the icon CGRAM patterns.
- **Before exiting:** set menu 9 PRESET to **PRESET INHIBITED**, otherwise the user memory gets wiped (p. 23 (27)).

## Still open after Phase 0

- [ ] FFC pin-1 orientation and numbering: check by continuity.
- [ ] Bus speed, bit order and /CS framing: first capture.
- [ ] Spare current on S12 (load test), or go with an external supply.
- [ ] MG to chassis bond: not traced in the schematics I reviewed. Measure MG to chassis resistance with the unit unplugged.
- [ ] Confirm the M66003 command set from the emsyscode driver (0xFA display write, 0xFC CGRAM) against the first capture.
- [ ] Which remote table (RAV246/247) matches your remote, and the K51 key label.
