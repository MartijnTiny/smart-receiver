# RX-V540RDS display bus – M66003 protocol (observed)

Source: sniffer v2 captures, 2026-11-20 (volume, inputs, DSP programs, standby/power-on, DIAG menu 7 VFD check). Bus: IC601 → IC501 M66003 via CB502 (CEFD/CKFD/DTFD).
Status: **Phase 1 gate passed.** Display text and icons can be reconstructed from the log alone.

## Physical layer

| Item | Value |
|---|---|
| Mode | SPI mode 3, MSB first, ~500 kHz |
| CS (CEFD) | Active low. **Separate CS frame per command and per data block** |
| Gaps | `FA xx` → data ≈ 4–5 µs; between frames of one screen update 5–195 µs; between updates ≥ ms |
| Refresh | None: the µC writes only on change |

## Command set

| Frame | Meaning | Evidence |
|---|---|---|
| `FA <addr>` | Set write address; the next CS frame is written from `addr`, auto-increment | All text |
| (data) | Character codes: ASCII; `0x90`–`0x9F` = CGRAM glyph 0–15 | |
| `FC <n> <c1..c5>` | Define CGRAM glyph `n` (5 columns) | |
| `8p <bits>` | Extra anodes (P36–P38) for position `p` (seen: 0x80, 0x8D, 0x8E). 00 normally, FF in all-on, AA/55 in checker | VFD check |
| `F0` | Display off | Standby |
| `F1` | Display on (normal) | Init |
| `F5` | Auto-increment mode | Init |
| `F6 0F` | Digit count (16) | Init |
| `F7 00` + 14 bytes | Unknown RAM clear (followed by 14 spaces) | Init |
| `F8 1D` | Display configuration | Init |
| `F9 xx` | Dimmer: `A0` normal, `E0` in DIAG, `60` once in VFD check | |
| `FB 00` | Set RAM address 0 | Init |

## Display layout and CGRAM icon map

| Address | Grid | Content |
|---|---|---|
| 0x00–0x0D | 1G–14G | 14 text characters |
| 0x0E | 15G | Glyph 0x90 (CGRAM 0): S2, MD/CD-R, TUNER, CD, PHONO, V1–V10 bar |
| 0x0F | 16G | Glyph 0x91 (CGRAM 1): R1–R5, S1, input icons |

**Glyph bit ↔ anode:** byte `c` (1–5) = column, bit7 = row 1 … bit1 = row 7 (bit0 unused).
Anode **P = (row − 1)·5 + column** (anode table p. 45). Verified against volume and input changes.

| Icon (manual p. 45) | Anode | Glyph | Byte.mask | Verified |
|---|---|---|---|---|
| S2 | P1 | 0 | 1.0x80 | Always on so far |
| MD/CD-R | P3 | 0 | 3.0x80 | ✔ |
| TUNER | P4 | 0 | 4.0x80 | ✔ |
| CD | P5 | 0 | 5.0x80 | ✔ |
| PHONO | P6 | 0 | 1.0x40 | – |
| MUTE (unlabelled in manual) | P17 | 0 | 2.0x10 | ✔ (IR mute on/off) |
| V1 | P20 | 0 | 5.0x10 | ✔ (on at −60) |
| V2 | P21 | 0 | 1.0x08 | ✔ |
| V3 | P22 | 0 | 2.0x08 | ✔ (on from −59) |
| V4 | P23 | 0 | 3.0x08 | ✔ (−39) |
| V5 | P24 | 0 | 4.0x08 | ✔ (−29) |
| V6 | P25 | 0 | 5.0x08 | ✔ (−19) |
| V7 | P26 | 0 | 1.0x04 | ✔ (−13) |
| V8–V10 | P27–P29 | 0 | 2/3/4.0x04 | – |
| R1–R5 | P21–P25 | 1 | 1–5.0x08 | Change with DSP program |
| S1 | P26 | 1 | 1.0x04 | Always on so far |
| VCR | P30* | 1 | 5.0x04 | ✔ (*manual labels P30 "VCR1"; VCR input lit it) |
| V-AUX | P31 | 1 | 1.0x02 | ✔ |
| D-TV/CBL | P32 | 1 | 2.0x02 | ✔ |
| DVD | P33 | 1 | 3.0x02 | ✔ |

Unlabelled bits are also used (e.g. glyph 0: 3.0x02, 4.0x10, 5.0x02; glyph 1: 1.0x80 with DTS/CD, 4.0x40 with PLII, 1.0x10/2.0x10/3.0x20 with some DSP programs). These are probably decoder/field indicators not in the anode table. Not needed for HA.

## Screens

| Screen | Frames |
|---|---|
| Volume | `FA 00` + `VOLUME` + 8 spaces, `FA 07` + `-NN.0dB`; next steps `FA 07` + value only |
| Input | `FA 00` + 8-char name, `FA 08` + 6-char decoder field (`AUTO  `, ` AUTO `, spaces). Names: `MD/CD-R `, ` TUNER  `, `   CD   `, `  VCR   `, ` V-AUX  `, `D-TV/CBL`, `  DVD   ` |
| DSP program (idle) | `FA 00` + 14 chars, e.g. `  6ch Stereo  `, ` MUSIC VIDEO  `, `  TV THEATER  `, `  Mono Movie  `, `Variety/Sports`, ` MOVIE THTR 1 `, `  Spectacle   `, `   Sci-Fi     `, ` MOVIE THTR 2 `, `  Adventure   `, `   General    `, `  PRO LOGIC   `, `   Enhanced   `, ` PRO LOGIC \x97  ` (II), `  PL\x97 Movie   `, `  PL\x97 Music   `, `     DTS      `, ` Neo:6 Cinema `, ` Neo:6 Music  `, `CONCERT HALL  `, `  JAZZ CLUB   `, ` ROCK CONCERT `, `ENTERTAINMENT `, `    Disco     `, `     Game     ` |
| Mute | `FA 00` + `AUDIO MUTE ON ` (≈1.05 s, then DSP name); **mute icon = glyph 0 byte2.0x10 (P17)** stays set while muted. Unmute: `AUDIO MUTE OFF` ≈1 s, icon bit cleared |
| Timeout | Volume/input screen reverts to the DSP program after ~3.0 s |
| Standby | `F0` (display off), then power-down glitches (1-bit frames) |
| Power-on | `F9 A0`, `F0`, `F8 1D`, 16 × `FC` glyph defs, `F5`, `F6 0F`, `F7 00`+blank, `FB 00`, `FA 00`+blank, icons cleared, `F1`. **Repeated 3–4× ~44 ms apart**, then `VOLUME -NN.0dB` (volume shown at power-on), then the DSP name |

Character `0x97` is used for the "II" in Pro Logic II (mask ROM -0101 glyph).

## IR injection results (Phase 2, 2026-11-20)

- Injection via BC547 on REM1 works first time. **Yamaha notation is the on-wire NEC value:** address 0x7A, command 0x1A sent LSB-first with inverses, no bit reversal.
- Verified: VOL+ (1A), VOL− (1B), MUTE toggle (1C), POWER ON (1D, ignored when already on: discrete), STANDBY (1E → `F0`), direct inputs TUNER (16), CD (15), DVD (C1), V-AUX (55), D-TV/CBL (54).
- The first VOL± press after the volume screen has timed out only shows the volume; the change starts from the second press.
- **Exit criterion met:** self-test (100 alternating VOL+/VOL−, each verified on the display bus) passed 100/100 twice.
- Injector loading: REM1 rise time (10→90 %) with the BC547 fitted but off ≈ 10–11 µs, vs ≈ 10 µs without it: negligible.
- Latency TX start → first display frame ≈ 70–85 ms (NEC frame itself is ~68 ms).

## Open

- Meaning of `F7`, and the unlabelled icon bits (only if needed).
