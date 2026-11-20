/*
 * M66003 VFD bus sniffer + NEC IR injector – Smart Receiver bench tool (v3)
 * Target: ESP32 (ESP-WROOM-32), ESP-IDF v5.3+ / v6.x  (v2.1: longer burst gap, deeper queue)
 * Console: 115200 baud (IDF default).
 *
 * Why polling instead of the SPI slave peripheral (v1):
 *   The µC sends `FA <addr>` and the following character data as two CS
 *   frames separated by only ~5 µs of CS-high (scope, 2026-11-20). The
 *   ESP-IDF SPI slave driver cannot re-arm within 5 µs, so data frames lost
 *   their first 1–3 bits. Here core 1 samples the three pins directly and
 *   sees every CS edge, however short the gap.
 *
 * How it works:
 *   - Core 1 runs one task that spins until CEFD goes low, then masks
 *     interrupts on core 1 and samples GPIO_IN_REG in a tight loop (IRAM):
 *     a data bit is taken at every rising SCK edge while CS is low
 *     (SPI mode 3, MSB first: confirmed on the scope, B5).
 *   - A CS rising edge ends a frame. If CS goes low again within
 *     GAP_END_US, the next frame is captured in the same burst. After
 *     GAP_END_US of CS-high (or BURST_MAX_US / BURST_FRAMES as safety
 *     limits) interrupts are re-enabled and the frames are queued.
 *   - Core 0 (app_main) prints one line per frame.
 *
 * Output:
 *   <frame#> <t_us> <dt_us> <bits> g<gap_us>[flags]: <hex>  |<ascii>|
 *   gap_us = CS-high time before this frame inside a burst (0 = burst start)
 *   flags  = PARTIAL (bits % 8 != 0), TRUNC (> FRAME_MAX bytes), CUT (safety limit)
 *
 * Assumptions:
 *   - Inputs come through 1k/2.2k dividers from 5 V logic (~3.1 V at the pin).
 *   - SCK = 500 kHz (B5), so a 1 µs high/low phase: the loop samples far faster.
 *   - Nothing else runs on core 1 (task-WDT idle check for CPU1 is disabled
 *     in sdkconfig.defaults because this task never yields).
 *
 * v3 adds Phase 2 IR injection:
 *   - RMT TX on GPIO25 -> 4.7k -> BC547 base (100k B-E), collector on REM1,
 *     emitter on MG. GPIO high = transistor on = REM1 low = "IR burst".
 *     The signal is already demodulated (no 38 kHz carrier): we bypass the
 *     IR receiver module. RMT idle level is low = transistor off.
 *   - Keys typed in the serial monitor send Yamaha AMP codes (manual p. 138):
 *       u VOL+ (7A-1A)   d VOL- (7A-1B)   m MUTE (7A-1C)
 *       p POWER ON (7A-1D)  s STANDBY (7A-1E)
 *       c CD (7A-15)  t TUNER (7A-16)  v DVD (7A-C1)  x V-AUX (7A-55)
 *       b D-TV/CBL (7A-54)  r toggle bit order  h help
 *       T self-test: 100 commands (VOL+/VOL- alternating, so volume stays
 *         within +/-1 dB of where it is), each verified on the display bus
 *   - Bit order: NEC sends each byte LSB first. Whether Yamaha's "7A-1A"
 *     notation means the on-wire LSB-first value or an MSB-first value is
 *     not documented in the manual. Default = notation used as-is (LSB first
 *     on the wire); 'r' switches to bit-reversed bytes. The display tells
 *     which one is right.
 *   - No idle-line check yet: REM1 can't be read safely without a buffer
 *     (weak pull-up). Don't press the real remote while sending.
 *
 * Wiring (sniffer unchanged from v1):
 *   CKFD (DB-9 pin 8) -> divider -> GPIO18
 *   DTFD (DB-9 pin 2) -> divider -> GPIO23
 *   CEFD (DB-9 pin 6) -> divider -> GPIO5
 *   MG   (DB-9 pins 3, 4, 7) -> GND
 *   REM1 (DB-9 pin 1) <- BC547 collector (see above), GPIO25 drives the base
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "soc/gpio_reg.h"

#define PIN_CS        5
#define PIN_SCK       18
#define PIN_DATA      23
#define CS_MASK       (1u << PIN_CS)
#define CK_MASK       (1u << PIN_SCK)
#define DT_MASK       (1u << PIN_DATA)
#define PIN_IR        25       /* RMT TX -> BC547 base -> REM1 */

#define FRAME_MAX     64       /* bytes per CS frame we keep */
#define BURST_FRAMES  32       /* frames per burst before forced flush */
#define GAP_END_US    2000     /* CS high this long ends a burst. Gaps inside one
                                  * screen update reach ~195 µs, so 200 µs was too
                                  * tight (lost an `FA` byte once). */
#define BURST_MAX_US  20000    /* max time with interrupts masked */
#define QUEUE_DEPTH   256      /* power-on init is ~60 frames in ~50 ms; printing
                                  * at 115200 is slower than that */

#define F_TRUNC       0x01
#define F_CUT         0x02

typedef struct {
    int64_t  t_us;       /* time of CS rising edge (frame end) */
    uint32_t c_end;      /* cycle count at frame end (internal) */
    uint32_t gap_us;     /* CS-high time before this frame in the burst */
    uint32_t c_cs;       /* cycle count when CS low was seen (internal) */
    uint16_t lead_100ns; /* CS low seen -> first SCK falling edge, 0.1 µs; 0xFFFF = none */
    uint16_t bits;
    uint8_t  flags;
    uint8_t  d[FRAME_MAX];
} frame_t;

static frame_t       s_burst[BURST_FRAMES];
static QueueHandle_t s_q;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_dropped;

static inline __attribute__((always_inline)) uint32_t pins(void)
{
    return REG_READ(GPIO_IN_REG);
}

/* Called with interrupts masked on this core and CS already low.
 * Returns the number of frames captured into s_burst. */
static int IRAM_ATTR capture_burst(uint32_t cpu_per_us)
{
    const uint32_t gap_end   = GAP_END_US * cpu_per_us;
    const uint32_t burst_max = BURST_MAX_US * cpu_per_us;
    const uint32_t c_start   = esp_cpu_get_cycle_count();
    uint32_t c_high   = c_start;
    bool     in_frame = true;
    int      n        = 0;
    frame_t *f        = &s_burst[0];
    uint32_t prev     = pins();

    memset(f, 0, sizeof *f);
    f->c_cs       = c_start;
    f->lead_100ns = 0xFFFF;

    for (;;) {
        const uint32_t in  = pins();
        const uint32_t now = esp_cpu_get_cycle_count();

        if (!(in & CS_MASK)) {                       /* CS low */
            if (!in_frame) {                         /* next frame in burst */
                if (n >= BURST_FRAMES) {
                    break;
                }
                f = &s_burst[n];
                memset(f, 0, sizeof *f);
                f->gap_us     = (now - c_high) / cpu_per_us;
                f->c_cs       = now;
                f->lead_100ns = 0xFFFF;
                in_frame  = true;
            }
            if (!(in & CK_MASK) && (prev & CK_MASK) && f->lead_100ns == 0xFFFF) {
                f->lead_100ns = (uint16_t)(((now - f->c_cs) * 10u) / cpu_per_us);
            }
            if ((in & CK_MASK) && !(prev & CK_MASK)) {   /* rising SCK */
                if (f->bits < FRAME_MAX * 8) {
                    if (in & DT_MASK) {
                        f->d[f->bits >> 3] |= (uint8_t)(0x80u >> (f->bits & 7));
                    }
                    f->bits++;
                } else {
                    f->flags |= F_TRUNC;
                }
            }
        } else if (in_frame) {                       /* CS rising: frame done */
            f->c_end = now;
            n++;
            in_frame = false;
            c_high   = now;
        } else if (now - c_high > gap_end) {         /* burst over */
            break;
        }

        if (now - c_start > burst_max) {             /* safety limit */
            if (in_frame) {
                f->c_end  = now;
                f->flags |= F_CUT;
                n++;
            }
            break;
        }
        prev = in;
    }
    return n;
}

static void sniffer_task(void *arg)
{
    const uint32_t cpu_per_us = esp_rom_get_cpu_ticks_per_us();

    for (;;) {
        while (pins() & CS_MASK) {
            /* idle: spin until CS goes low (interrupts still enabled) */
        }
        portENTER_CRITICAL(&s_mux);
        const int n = capture_burst(cpu_per_us);
        portEXIT_CRITICAL(&s_mux);

        const int64_t  t_now = esp_timer_get_time();
        const uint32_t c_now = esp_cpu_get_cycle_count();
        for (int i = 0; i < n; i++) {
            s_burst[i].t_us = t_now - (int64_t)((c_now - s_burst[i].c_end) / cpu_per_us);
            if (xQueueSend(s_q, &s_burst[i], 0) != pdTRUE) {
                s_dropped++;
            }
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Volume tracking from the display bus (used by the self-test)           */
/* ---------------------------------------------------------------------- */

static volatile int32_t  s_db_tenths;   /* last volume seen, in 0.1 dB */
static volatile uint32_t s_db_seq;      /* increments on every volume frame */

/* Parse "-79.0dB" / "+3.5dB" / " 0.0dB" (7 bytes at display address 7). */
static bool parse_db(const uint8_t *d, size_t n, int32_t *tenths)
{
    if (n < 6 || d[n - 2] != 'd' || d[n - 1] != 'B') {
        return false;
    }
    int sign = (d[0] == '-') ? -1 : 1;
    int32_t v = 0;
    bool frac = false, any = false;
    for (size_t i = (d[0] == '-' || d[0] == '+' || d[0] == ' ') ? 1 : 0; i < n - 2; i++) {
        if (d[i] >= '0' && d[i] <= '9') {
            v = v * 10 + (d[i] - '0');
            any = true;
            if (frac) {
                break;              /* one decimal only */
            }
        } else if (d[i] == '.') {
            frac = true;
        } else if (d[i] != ' ') {
            return false;
        }
    }
    if (!any || !frac) {
        return false;
    }
    *tenths = sign * v;
    return true;
}

/* ---------------------------------------------------------------------- */
/* NEC IR injection (Phase 2)                                             */
/* ---------------------------------------------------------------------- */

#define YAMAHA_CUSTOM 0x7A

static rmt_channel_handle_t s_ir_chan;
static rmt_encoder_handle_t s_ir_enc;
static bool                 s_bit_reverse;   /* false = notation as-is */

typedef struct {
    char        key;
    uint8_t     cmd;
    const char *name;
} ir_key_t;

static const ir_key_t s_keys[] = {
    { 'u', 0x1A, "VOL+" },     { 'd', 0x1B, "VOL-" },     { 'm', 0x1C, "MUTE" },
    { 'p', 0x1D, "POWER ON" }, { 's', 0x1E, "STANDBY" },
    { 'c', 0x15, "CD" },       { 't', 0x16, "TUNER" },    { 'v', 0xC1, "DVD" },
    { 'x', 0x55, "V-AUX" },    { 'b', 0x54, "D-TV/CBL" },
};

static uint8_t rev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

static void ir_init(void)
{
    const rmt_tx_channel_config_t cfg = {
        .gpio_num          = PIN_IR,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 1000000,          /* 1 tick = 1 µs */
        .mem_block_symbols = 64,               /* one NEC frame = 34 symbols */
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&cfg, &s_ir_chan));
    const rmt_copy_encoder_config_t ecfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&ecfg, &s_ir_enc));
    ESP_ERROR_CHECK(rmt_enable(s_ir_chan));
}

/* One NEC frame: 9 ms mark, 4.5 ms space, 32 bits LSB first
 * (addr, ~addr, cmd, ~cmd), 560 µs stop mark. Mark = GPIO high. */
static void ir_send_nec(uint8_t addr, uint8_t cmd)
{
    if (s_bit_reverse) {
        addr = rev8(addr);
        cmd  = rev8(cmd);
    }
    const uint8_t bytes[4] = { addr, (uint8_t)~addr, cmd, (uint8_t)~cmd };
    rmt_symbol_word_t sym[34];
    int n = 0;

    sym[n++] = (rmt_symbol_word_t){ .level0 = 1, .duration0 = 9000, .level1 = 0, .duration1 = 4500 };
    for (int b = 0; b < 4; b++) {
        for (int i = 0; i < 8; i++) {
            const bool one = (bytes[b] >> i) & 1;
            sym[n++] = (rmt_symbol_word_t){ .level0 = 1, .duration0 = 560,
                                            .level1 = 0, .duration1 = one ? 1690 : 560 };
        }
    }
    sym[n++] = (rmt_symbol_word_t){ .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 };

    const rmt_transmit_config_t tc = { .loop_count = 0 };   /* eot level 0 = released */
    ESP_ERROR_CHECK(rmt_transmit(s_ir_chan, s_ir_enc, sym, n * sizeof(sym[0]), &tc));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_ir_chan, 200));  /* sym[] lives on the stack */
}

static void print_help(void)
{
    printf("# keys:");
    for (size_t i = 0; i < sizeof s_keys / sizeof s_keys[0]; i++) {
        printf(" %c=%s", s_keys[i].key, s_keys[i].name);
    }
    printf("  r=bit order (now %s)  T=self-test  h=help\n", s_bit_reverse ? "REVERSED" : "as-is");
}

static bool wait_db_change(uint32_t seq0, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 10) {
        if (s_db_seq != seq0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

/* 100 commands, alternating VOL+ / VOL- so the volume never drifts more than
 * one step. The first press only wakes the volume screen (receiver
 * behaviour), so it isn't counted. Each counted press must produce a new
 * dB value in the right direction within 600 ms. */
static void self_test(void)
{
    const int N = 100;
    printf("# SELFTEST start: %d commands\n", N);

    uint32_t seq = s_db_seq;
    ir_send_nec(YAMAHA_CUSTOM, 0x1A);                 /* wake volume screen */
    if (!wait_db_change(seq, 1000)) {
        printf("# SELFTEST abort: no volume frame after wake-up press\n");
        return;
    }
    int32_t cur = s_db_tenths;
    vTaskDelay(pdMS_TO_TICKS(200));

    int ok = 0, miss = 0;
    for (int i = 0; i < N; i++) {
        const bool up = (i % 2) == 0;
        seq = s_db_seq;
        ir_send_nec(YAMAHA_CUSTOM, up ? 0x1A : 0x1B);
        if (wait_db_change(seq, 600)) {
            const int32_t now = s_db_tenths;
            if ((up && now > cur) || (!up && now < cur)) {
                ok++;
            } else {
                miss++;
                printf("# SELFTEST #%d %s: wrong value %ld -> %ld (0.1 dB)\n",
                       i, up ? "VOL+" : "VOL-", (long)cur, (long)now);
            }
            cur = now;
        } else {
            miss++;
            printf("# SELFTEST #%d %s: no response\n", i, up ? "VOL+" : "VOL-");
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    printf("# SELFTEST done: %d/%d ok, %d missed\n", ok, N, miss);
}

static void keys_task(void *arg)
{
    /* RX buffer must exceed the 128-byte HW FIFO; no TX buffer, so printf
     * keeps writing to the UART directly. */
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    print_help();

    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == 'r') {
            s_bit_reverse = !s_bit_reverse;
            printf("# bit order now %s\n", s_bit_reverse ? "REVERSED" : "as-is");
            continue;
        }
        if (c == 'h') {
            print_help();
            continue;
        }
        if (c == 'T') {
            self_test();
            continue;
        }
        for (size_t i = 0; i < sizeof s_keys / sizeof s_keys[0]; i++) {
            if (s_keys[i].key == (char)c) {
                printf("# TX %s %02X-%02X (%s) t=%" PRId64 "\n", s_keys[i].name, YAMAHA_CUSTOM,
                       s_keys[i].cmd, s_bit_reverse ? "reversed" : "as-is", esp_timer_get_time());
                ir_send_nec(YAMAHA_CUSTOM, s_keys[i].cmd);
                break;
            }
        }
    }
}

void app_main(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_CS) | (1ULL << PIN_SCK) | (1ULL << PIN_DATA),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    s_q = xQueueCreate(QUEUE_DEPTH, sizeof(frame_t));
    if (s_q == NULL) {
        printf("# queue alloc failed\n");
        return;
    }

    printf("\n# m66003_sniffer v3.1 (polling sniffer + NEC TX on GPIO%d) ready: CS=GPIO%d SCK=GPIO%d DATA=GPIO%d, %" PRIu32 " MHz\n",
           PIN_IR, PIN_CS, PIN_SCK, PIN_DATA, esp_rom_get_cpu_ticks_per_us());
    printf("# frame t_us dt_us bits g<gap_us> L<CS->SCK lead us>[flags]: hex |ascii|\n");

    xTaskCreatePinnedToCore(sniffer_task, "sniff", 4096, NULL,
                            configMAX_PRIORITIES - 1, NULL, 1);

    ir_init();
    xTaskCreatePinnedToCore(keys_task, "keys", 4096, NULL, 5, NULL, 0);

    uint32_t n = 0, dropped_seen = 0;
    int      fa_addr = -1;              /* address set by the last FA frame */
    uint16_t lead_min = 0xFFFF;         /* smallest CS->first SCK seen */
    int64_t  t_prev = 0;
    frame_t  f;
    char     line[48 + FRAME_MAX * 3 + FRAME_MAX + 8];

    for (;;) {
        if (xQueueReceive(s_q, &f, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (f.bits == 0) {
            continue;                     /* CS pulse without clocks */
        }
        if (s_dropped != dropped_seen) {
            dropped_seen = s_dropped;
            printf("# dropped frames so far: %" PRIu32 "\n", dropped_seen);
        }

        const size_t bytes = (f.bits + 7) / 8;

        /* Track volume for the self-test: `FA 07` followed by "-NN.Ndb". */
        if (f.bits == 16 && f.d[0] == 0xFA) {
            fa_addr = f.d[1];
        } else {
            int32_t db;
            if (fa_addr == 0x07 && (f.bits % 8) == 0 && parse_db(f.d, bytes, &db)) {
                s_db_tenths = db;
                s_db_seq++;
            }
            fa_addr = -1;
        }
        if (f.lead_100ns < lead_min) {
            lead_min = f.lead_100ns;
            printf("# new min CS->SCK lead: %u.%u us (frame %" PRIu32 ", %s)\n", (unsigned)(lead_min / 10), (unsigned)(lead_min % 10), n,
                   f.gap_us ? "inside burst" : "burst start");
        }
        int p = snprintf(line, sizeof line, "%" PRIu32 " %" PRId64 " %" PRId64 " %u g%" PRIu32 " L%u.%u%s%s%s:",
                         n++, f.t_us, t_prev ? f.t_us - t_prev : 0, (unsigned)f.bits, f.gap_us,
                         (unsigned)(f.lead_100ns / 10), (unsigned)(f.lead_100ns % 10),
                         (f.bits % 8) ? " PARTIAL" : "",
                         (f.flags & F_TRUNC) ? " TRUNC" : "",
                         (f.flags & F_CUT) ? " CUT" : "");
        for (size_t i = 0; i < bytes; i++) {
            p += snprintf(line + p, sizeof line - p, " %02X", f.d[i]);
        }
        p += snprintf(line + p, sizeof line - p, "  |");
        for (size_t i = 0; i < bytes && p < (int)sizeof line - 3; i++) {
            line[p++] = (f.d[i] >= 0x20 && f.d[i] < 0x7F) ? (char)f.d[i] : '.';
        }
        line[p++] = '|';
        line[p]   = '\0';
        puts(line);
        t_prev = f.t_us;
    }
}
