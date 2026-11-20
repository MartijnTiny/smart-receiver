# m66003_sniffer – Phase 1 bench tool

Passive polling sniffer (v2: core 1 samples the pins directly) for the RX-V540RDS display bus (IC601 → IC501 M66003).
Prints one line per CEFD frame over USB serial at 115200 baud.

## Wiring (ESP-WROOM-32 devkit)

| DB-9 (receiver side) | Signal | Divider | ESP32 |
|---|---|---|---|
| 8 | CKFD | 2.2 k top / 3.3 k to GND | GPIO18 |
| 2 | DTFD | 2.2 k / 3.3 k | GPIO23 |
| 6 | CEFD | 2.2 k / 3.3 k | GPIO5 |
| 3, 4, 7 | MG | — | GND (all three) |
| 1, 9 | REM1, /BLK | **not connected for now** (no divider on REM1!) | — |

Power-up order: ESP on USB first, then the receiver.

## Build / flash / log

```
idf.py set-target esp32
idf.py build flash
python -m serial.tools.miniterm COM5 115200 > capture_vol_up.txt
```

One capture file per action from the probe-plan capture script (step D).

## Output

```
<frame#> <t_us> <dt_us> <bits> g<gap_us>[PARTIAL][TRUNC][CUT]: <hex>  |<ascii>|
```

- `dt_us` is the gap since the previous frame. A steady value means periodic refresh.
- `PARTIAL` means the bit count isn't a multiple of 8 (wrong mode, or noise on SCK).
- `g` is the CS-high time before the frame within a burst (`FA xx` → data is ~5 µs).
- `TRUNC` means the frame is over 64 bytes and the tail was dropped.

Expected (from the emsyscode M66003 driver, to be confirmed):
`FA <addr> <ASCII…>` for text, `FC <90–9F> <5 bytes>` for glyphs.
