// Yamaha RX-V540RDS display-bus sniffer + IR control (Smart Receiver, Phase 3)
//
// Capture: level-3 GPIO interrupt on the CEFD (CS) falling edge, allocated on
// core 1 (ESPHome's loopTask core, where setup() runs). The handler polls
// GPIO_IN_REG for the whole burst: a data bit on every rising SCK edge while CS
// is low (SPI mode 3, MSB first, ~500 kHz); a CS rising edge ends a frame; the
// burst ends after 2 ms of CS high (20 ms safety cap). Budget: the µC waits
// ~7.5 µs from CS low to the first SCK edge (measured 2026-11-20), so
// interrupt entry latency is not critical. A missed first bit shows up as a
// frame with bits % 8 != 0 and is counted in frames_bad.
//
// Decoding (loop): shadow display RAM / CGRAM, screen text -> volume, mute,
// input, decoder, DSP program, tuner; CGRAM icon bits -> mute and input.
//
// Control: Yamaha NEC codes (custom 0x7A) via remote_transmitter on REM1.
// Every command is verified on the display bus and retried if needed.
//
// Assumptions / limits:
//  - Nothing else in the firmware uses GPIO interrupts (no gpio_install_isr_service),
//    because this component owns the shared GPIO interrupt source.
//  - CS, SCK and DATA are GPIO 0..31 (one GPIO_IN_REG read).

#include "yamaha_vfd.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esphome/core/log.h"
#include "esphome/components/remote_base/remote_base.h"

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "soc/interrupts.h"

namespace esphome {
namespace yamaha_vfd {

static const char *const TAG = "yamaha_vfd";

static const uint32_t GAP_END_US = 2000;
static const uint32_t BURST_MAX_US = 20000;
static const uint32_t MUTE_BLINK_HOLD_MS = 1500;  // > one blink period of the MUTE icon

static const uint8_t CODE_VOL_UP = 0x1A;
static const uint8_t CODE_VOL_DOWN = 0x1B;
static const uint8_t CODE_MUTE = 0x1C;
static const uint8_t CODE_POWER_ON = 0x1D;
static const uint8_t CODE_STANDBY = 0x1E;

// Inputs: display name (trimmed), IR code (0 = no verified code), icon (CGRAM glyph, byte, mask).
struct InputDef {
  const char *name;
  uint8_t code;
  uint8_t glyph;
  uint8_t byte;
  uint8_t mask;
};
static const InputDef INPUTS[] = {
    {"MD/CD-R", 0xC9, 0, 2, 0x80}, {"TUNER", 0x16, 0, 3, 0x80}, {"CD", 0x15, 0, 4, 0x80},
    {"PHONO", 0x00, 0, 0, 0x40},  // icon per manual p. 45; no verified IR code
    {"VCR", 0x0F, 1, 4, 0x04},     {"V-AUX", 0x55, 1, 0, 0x02}, {"D-TV/CBL", 0x54, 1, 1, 0x02},
    {"DVD", 0xC1, 1, 2, 0x02},
};

// DSP program names as shown when idle (trimmed, 0x97 rendered as "II").
static const char *const PROGRAMS[] = {
    "6ch Stereo", "MUSIC VIDEO",  "TV THEATER",   "Mono Movie",   "Variety/Sports", "MOVIE THTR 1",
    "Spectacle",  "Sci-Fi",       "MOVIE THTR 2", "Adventure",    "General",        "PRO LOGIC",
    "Enhanced",   "PRO LOGIC II", "PLII Movie",   "PLII Music",   "DTS",            "Neo:6 Cinema",
    "Neo:6 Music", "CONCERT HALL", "JAZZ CLUB",   "ROCK CONCERT", "ENTERTAINMENT",  "Disco",
    "Game",
};

// Sub-program -> program, inferred from capture order and Yamaha's case
// convention (UPPERCASE = program, Mixed-case = sub-program). Not in the
// service manual. Names not listed here are their own program.
struct ProgramGroup {
  const char *sub;
  const char *group;
};
static const ProgramGroup PROGRAM_GROUPS[] = {
    {"Mono Movie", "TV THEATER"},     {"Variety/Sports", "TV THEATER"}, {"Spectacle", "MOVIE THTR 1"},
    {"Sci-Fi", "MOVIE THTR 1"},       {"Adventure", "MOVIE THTR 2"},    {"General", "MOVIE THTR 2"},
    {"Enhanced", "PRO LOGIC"},        {"PLII Movie", "PRO LOGIC II"},   {"PLII Music", "PRO LOGIC II"},
    {"Neo:6 Cinema", "DTS"},          {"Neo:6 Music", "DTS"},           {"Disco", "ENTERTAINMENT"},
    {"Game", "ENTERTAINMENT"},
};

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos)
    return "";
  size_t b = s.find_last_not_of(' ');
  return s.substr(a, b - a + 1);
}

// "-79.0dB", "+3.5dB", " 0.0dB"
static bool parse_db(const uint8_t *d, size_t n, float *out) {
  if (n < 6 || d[n - 2] != 'd' || d[n - 1] != 'B')
    return false;
  int sign = 1;
  size_t i = 0;
  if (d[0] == '-' || d[0] == '+' || d[0] == ' ') {
    sign = d[0] == '-' ? -1 : 1;
    i = 1;
  }
  int whole = 0, frac = 0;
  bool dot = false, any = false;
  for (; i < n - 2; i++) {
    if (d[i] >= '0' && d[i] <= '9') {
      any = true;
      if (dot) {
        frac = d[i] - '0';
        break;
      }
      whole = whole * 10 + (d[i] - '0');
    } else if (d[i] == '.') {
      dot = true;
    } else if (d[i] != ' ') {
      return false;
    }
  }
  if (!any || !dot)
    return false;
  *out = sign * (whole + frac / 10.0f);
  return true;
}

// ---------------------------------------------------------------------------
// Capture (interrupt context, IRAM, core 1)
// ---------------------------------------------------------------------------

static void IRAM_ATTR gpio_isr(void *arg) { static_cast<YamahaVfd *>(arg)->capture_burst(); }

void IRAM_ATTR YamahaVfd::capture_burst() {
  const uint32_t cs = 1u << this->cs_pin_;
  const uint32_t ck = 1u << this->sck_pin_;
  const uint32_t dt = 1u << this->data_pin_;
  const uint32_t gap_end = GAP_END_US * this->cpu_per_us_;
  const uint32_t burst_max = BURST_MAX_US * this->cpu_per_us_;

  bool timed_out = false;
  do {
    const uint32_t c_start = esp_cpu_get_cycle_count();
    uint32_t c_high = c_start;
    bool in_frame = false;
    RawFrame *f = nullptr;
    uint32_t prev = REG_READ(GPIO_IN_REG);

    for (;;) {
      const uint32_t in = REG_READ(GPIO_IN_REG);
      const uint32_t now = esp_cpu_get_cycle_count();

      if (!(in & cs)) {
        if (!in_frame) {  // frame start: claim a ring slot (or drop if full)
          const uint16_t next = (uint16_t) ((this->head_ + 1) % VFD_RING_SIZE);
          if (next == this->tail_) {
            f = nullptr;
          } else {
            f = &this->ring_[this->head_];
            f->bits = 0;
            for (size_t i = 0; i < VFD_FRAME_MAX; i++)
              f->d[i] = 0;
          }
          in_frame = true;
        }
        if ((in & ck) && !(prev & ck) && f != nullptr && f->bits < VFD_FRAME_MAX * 8) {
          if (in & dt)
            f->d[f->bits >> 3] |= (uint8_t) (0x80u >> (f->bits & 7));
          f->bits++;
        }
      } else if (in_frame) {  // CS rising: commit frame
        if (f != nullptr) {
          __asm__ __volatile__("" ::: "memory");  // frame data before publishing head
          this->head_ = (uint16_t) ((this->head_ + 1) % VFD_RING_SIZE);
        } else {
          this->dropped_ = this->dropped_ + 1;
        }
        in_frame = false;
        c_high = now;
      } else if (now - c_high > gap_end) {
        break;  // burst over
      }

      if (now - c_start > burst_max) {
        if (in_frame)
          this->dropped_ = this->dropped_ + 1;  // cut frame is discarded
        timed_out = true;
        break;
      }
      prev = in;
    }

    // Edges during the burst set the CS interrupt status again: clear it, then
    // re-check in case a new burst started in the meantime.
    REG_WRITE(GPIO_STATUS_W1TC_REG, cs);
    // After a timeout never re-loop: if CS is stuck low (cable unplugged, bad
    // divider) there are no more falling edges, so the ISR simply goes quiet
    // instead of hogging core 1.
  } while (!timed_out && !(REG_READ(GPIO_IN_REG) & cs));
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void YamahaVfd::setup() {
  this->cpu_per_us_ = esp_rom_get_cpu_ticks_per_us();

  gpio_config_t io{};
  io.pin_bit_mask = (1ULL << this->cs_pin_) | (1ULL << this->sck_pin_) | (1ULL << this->data_pin_);
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&io) != ESP_OK) {
    ESP_LOGE(TAG, "gpio_config failed");
    this->mark_failed();
    return;
  }
  gpio_set_intr_type((gpio_num_t) this->cs_pin_, GPIO_INTR_NEGEDGE);

  // setup() runs in ESPHome's loopTask, pinned to core 1, so the interrupt is
  // allocated on core 1 as well.
  esp_err_t err = esp_intr_alloc(ETS_GPIO_INTR_SOURCE, ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM, gpio_isr, this,
                                 &this->intr_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_intr_alloc failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  gpio_intr_enable((gpio_num_t) this->cs_pin_);

  // Restore the last input seen on the display (index into INPUTS, 0 = none).
  // Never sends anything: it only seeds the state until the display shows a
  // different input.
  this->input_pref_ = global_preferences->make_preference<uint32_t>(fnv1_hash("yamaha_vfd_input"));
  uint32_t idx = 0;
  if (this->input_pref_.load(&idx) && idx >= 1 && idx <= sizeof(INPUTS) / sizeof(INPUTS[0])) {
    this->input_ = INPUTS[idx - 1].name;
    this->saved_input_ = this->input_;
    ESP_LOGI(TAG, "restored last input: %s", this->input_.c_str());
  }

  // State is unknown after boot: probe with a harmless VOL- "wake" press later.
  this->probe_at_ = millis() + 8000;
}

void YamahaVfd::dump_config() {
  ESP_LOGCONFIG(TAG, "Yamaha VFD bus sniffer:");
  ESP_LOGCONFIG(TAG, "  CS GPIO%u, SCK GPIO%u, DATA GPIO%u", (unsigned) this->cs_pin_, (unsigned) this->sck_pin_,
                (unsigned) this->data_pin_);
  ESP_LOGCONFIG(TAG, "  CPU %u MHz, interrupt %s", (unsigned) this->cpu_per_us_,
                this->intr_ != nullptr ? "allocated (level 3)" : "NOT allocated");
}

void YamahaVfd::loop() {
  while (this->tail_ != this->head_) {
    __asm__ __volatile__("" ::: "memory");  // read head before the slot contents
    const RawFrame f = this->ring_[this->tail_];  // ISR never writes the tail slot
    this->tail_ = (uint16_t) ((this->tail_ + 1) % VFD_RING_SIZE);
    this->handle_frame_(f);
  }

  // Resolve a pending "mute icon cleared": only real once the blink has stopped.
  if (this->mute_clear_pending_ && millis() - this->mute_icon_clear_ms_ > MUTE_BLINK_HOLD_MS) {
    this->mute_clear_pending_ = false;
    if (!this->mute_known_ || this->muted_) {
      this->muted_ = false;
      this->mute_known_ = true;
      this->mute_seq_++;
    }
  }

  if (this->probe_at_ != 0 && (int32_t) (millis() - this->probe_at_) >= 0) {
    this->probe_at_ = 0;
    // Only probe when the bus has been completely quiet since boot. A muted
    // receiver keeps rewriting its blinking MUTE icon, so it is never quiet;
    // VOL- would cancel the mute, so a muted receiver is never probed.
    if (!this->on_known_ && this->op_ == Op::NONE && this->frames_ok_ == 0)
      this->probe();
  }
  this->run_op_();

  // Persist the input only when it really changed (ESPHome batches flash writes).
  if (!this->input_.empty() && this->input_ != this->saved_input_) {
    for (size_t i = 0; i < sizeof(INPUTS) / sizeof(INPUTS[0]); i++) {
      if (this->input_ == INPUTS[i].name) {
        uint32_t idx = i + 1;
        this->input_pref_.save(&idx);
        this->saved_input_ = this->input_;
        break;
      }
    }
  }

  // Push model: notify only when something visible to HA changed.
  std::string sig = this->state_signature_();
  if (sig != this->last_sig_) {
    this->last_sig_ = std::move(sig);
    this->change_callback_.call();
  }
}

std::string YamahaVfd::state_signature_() const {
  std::string s;
  s.reserve(96);
  s += (char) ('0' + this->on_known_ + 2 * this->on_ + 4 * this->mute_known_ + 8 * this->muted_ +
               16 * this->volume_valid_ + 32 * (this->op_ != Op::NONE));
  char v[16];
  snprintf(v, sizeof(v), "|%.1f|", this->volume_db_);
  s += v;
  s += this->input_ + '|' + this->decoder_ + '|' + this->program_ + '|' + this->tuner_ + '|';
  s.append(reinterpret_cast<const char *>(this->ram_), 14);
  return s;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

void YamahaVfd::handle_frame_(const RawFrame &f) {
  if (f.bits == 0)
    return;  // CS pulse without clocks (power transitions)
  if (f.bits % 8 != 0) {
    this->frames_bad_++;
    this->pending_addr_ = -1;
    ESP_LOGW(TAG, "partial frame (%u bits) dropped", (unsigned) f.bits);
    return;
  }
  this->frames_ok_++;
  const size_t n = f.bits / 8;
  if (this->trace_) {
    char hex[VFD_FRAME_MAX * 3 + 1];
    char txt[VFD_FRAME_MAX + 1];
    size_t hp = 0;
    for (size_t i = 0; i < n; i++) {
      hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X ", f.d[i]);
      txt[i] = (f.d[i] >= 0x20 && f.d[i] < 0x7F) ? (char) f.d[i] : '.';
    }
    txt[n] = 0;
    ESP_LOGI(TAG, "bus %2u: %-30s |%s|", (unsigned) n, hex, txt);
  }
  const uint8_t c = f.d[0];

  if (n == 2 && c == 0xFA) {  // set write address
    this->pending_addr_ = f.d[1];
    return;
  }
  if (n == 7 && c == 0xFC) {  // CGRAM glyph definition
    memcpy(this->glyph_[f.d[1] & 0x0F], &f.d[2], 5);
    this->pending_addr_ = -1;
    this->on_glyph_();
    return;
  }
  if (n == 2 && (c & 0xF0) == 0x80) {  // extra segments per position: not needed
    this->pending_addr_ = -1;
    return;
  }
  if ((c & 0xF0) == 0xF0) {  // controller commands
    if (n == 1 && c == 0xF0) {
      this->on_ = false;
      this->on_known_ = true;
    } else if (n == 1 && c == 0xF1) {
      this->on_ = true;
      this->on_known_ = true;
    }
    this->pending_addr_ = -1;
    return;
  }
  if (this->pending_addr_ >= 0) {  // character data after FA <addr>
    const uint8_t start = (uint8_t) this->pending_addr_;
    uint8_t len = 0;
    for (size_t i = 0; i < n && start + i < sizeof(this->ram_); i++, len++)
      this->ram_[start + i] = f.d[i];
    // The address auto-increments: a following data frame without a new FA
    // continues here (seen: FA 0E, 90, 91). Any command frame resets it.
    this->pending_addr_ = (start + len < (int) sizeof(this->ram_)) ? (int16_t) (start + len) : -1;
    this->on_text_write_(start, len);
  }
}

std::string YamahaVfd::program_group() const {
  if (this->program_.empty())
    return "";
  for (const auto &g : PROGRAM_GROUPS) {
    if (this->program_ == g.sub)
      return g.group;
  }
  return this->program_;
}

std::string YamahaVfd::raw_text_() const { return std::string(reinterpret_cast<const char *>(this->ram_), 14); }

std::string YamahaVfd::display_text() const {
  std::string out;
  for (size_t i = 0; i < 14; i++) {
    const uint8_t ch = this->ram_[i];
    if (ch == 0x97) {
      out += "II";
    } else if (ch >= 0x20 && ch < 0x7F) {
      out += (char) ch;
    } else {
      out += '?';
    }
  }
  return out;
}

void YamahaVfd::on_text_write_(uint8_t start, uint8_t len) {
  const std::string raw = this->raw_text_();
  const uint8_t end = start + len;

  // Volume screen: "VOLUME" at 0..5, value written at 7..13.
  if (raw.compare(0, 6, "VOLUME") == 0 && start <= 7 && end >= 14) {
    float db;
    if (parse_db(&this->ram_[7], 7, &db)) {
      this->volume_db_ = db;
      this->volume_valid_ = true;
      this->last_volume_ms_ = millis();
      this->vol_seq_++;
      this->on_ = true;
      this->on_known_ = true;
    }
    return;
  }
  if (raw == "AUDIO MUTE ON ") {
    this->mute_clear_pending_ = false;
    this->mute_icon_set_ms_ = millis();
    this->muted_ = true;
    this->mute_known_ = true;
    this->mute_seq_++;
    return;
  }
  if (raw == "AUDIO MUTE OFF") {
    this->mute_clear_pending_ = false;
    this->muted_ = false;
    this->mute_known_ = true;
    this->mute_seq_++;
    return;
  }
  // Input screen: 8-char name at 0, then 6-char decoder field at 8.
  if ((start == 0 && len == 8) || (start == 8 && len == 6)) {
    const std::string name = trim(raw.substr(0, 8));
    for (const auto &in : INPUTS) {
      if (name == in.name) {
        this->input_ = name;
        if (start == 8)
          this->decoder_ = trim(raw.substr(8, 6));
        break;
      }
    }
    return;
  }
  // Full-line screens: DSP program or tuner ("A2:FM 88.10MHz").
  if (start == 0 && len == 14) {
    // Inputs without an INPUT MODE field (CD, VCR, TUNER) are written as one
    // 14-char line: "   CD         ". The mode field is then absent.
    const std::string name = trim(raw.substr(0, 8));
    if (trim(raw.substr(8, 6)).empty()) {
      for (const auto &in : INPUTS) {
        if (name == in.name) {
          this->input_ = name;
          this->decoder_ = "N/A";
          return;
        }
      }
    }
    if (raw[2] == ':' && (raw.find("MHz") != std::string::npos || raw.find("kHz") != std::string::npos)) {
      this->tuner_ = trim(raw);
      this->input_ = "TUNER";
      return;
    }
    const std::string p = trim(this->display_text());
    for (const char *prog : PROGRAMS) {
      if (p == prog) {
        this->program_ = p;
        return;
      }
    }
  }
}

void YamahaVfd::on_glyph_() {
  // Icons live in CGRAM glyphs 0 (grid 15G) and 1 (16G). Ignore test patterns
  // (VFD check) by requiring at most one input icon to be lit.
  const InputDef *lit = nullptr;
  int count = 0;
  for (const auto &in : INPUTS) {
    if (this->glyph_[in.glyph][in.byte] & in.mask) {
      lit = &in;
      count++;
    }
  }
  if (count > 1)
    return;
  if (lit != nullptr)
    this->input_ = lit->name;
  // MUTE icon, anode P17. It blinks while muted, so: set = muted right away;
  // clear = unmuted only if no set write follows within MUTE_BLINK_HOLD_MS
  // (resolved in loop()).
  const bool m = (this->glyph_[0][1] & 0x10) != 0;
  if (m) {
    // A (blinking) MUTE icon also proves the receiver is on: no probe needed.
    this->on_ = true;
    this->on_known_ = true;
    this->mute_icon_set_ms_ = millis();
    this->mute_clear_pending_ = false;
    if (!this->mute_known_ || !this->muted_) {
      this->muted_ = true;
      this->mute_known_ = true;
      this->mute_seq_++;
    }
  } else if (!this->mute_known_) {
    // First icon data after boot shows no mute: still wait out a blink cycle.
    this->mute_icon_clear_ms_ = millis();
    this->mute_clear_pending_ = true;
  } else if (this->muted_ && !this->mute_clear_pending_) {
    this->mute_icon_clear_ms_ = millis();
    this->mute_clear_pending_ = true;
  }
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

void YamahaVfd::send_nec_(uint8_t cmd) {
  // NEC frame encoded here instead of via remote_base::NECProtocol, because
  // ESPHome only compiles protocol encoders that the YAML itself references.
  // On-wire byte order (each LSB first): 0x7A, 0x85 (~0x7A), cmd, ~cmd.
  // With carrier_duty_percent: 100% the "mark" is a solid level on REM1.
  if (this->tx_ == nullptr)
    return;
  ESP_LOGD(TAG, "TX 7A-%02X", (unsigned) cmd);
  static const uint32_t HEADER_MARK = 9000, HEADER_SPACE = 4500;
  static const uint32_t BIT_MARK = 560, ONE_SPACE = 1690, ZERO_SPACE = 560;
  const uint8_t bytes[4] = {0x7A, (uint8_t) ~0x7A, cmd, (uint8_t) ~cmd};

  auto call = this->tx_->transmit();
  remote_base::RemoteTransmitData *dst = call.get_data();
  dst->set_carrier_frequency(38000);
  dst->reserve(2 + 32 * 2 + 1);
  dst->item(HEADER_MARK, HEADER_SPACE);
  for (uint8_t b : bytes) {
    for (uint8_t i = 0; i < 8; i++)
      dst->item(BIT_MARK, (b & (1u << i)) ? ONE_SPACE : ZERO_SPACE);
  }
  dst->mark(BIT_MARK);
  call.perform();
}

void YamahaVfd::send_command(uint8_t cmd) { this->send_nec_(cmd); }

void YamahaVfd::start_op_(Op op) {
  this->op_ = op;
  this->attempts_ = 0;
  this->misses_ = 0;
  this->same_count_ = 0;
  this->steps_ = 0;
  this->waiting_ = false;
  this->last_was_step_ = false;
  this->last_dir_ = 0;
  this->deadline_ = 0;
}

void YamahaVfd::finish_(const char *msg) {
  ESP_LOGI(TAG, "%s", msg);
  this->op_ = Op::NONE;
  this->waiting_ = false;
}

void YamahaVfd::probe() { this->start_op_(Op::PROBE); }

void YamahaVfd::set_power(bool on) {
  this->want_bool_ = on;
  this->start_op_(Op::POWER);
}

void YamahaVfd::set_mute(bool mute) {
  this->want_bool_ = mute;
  this->start_op_(Op::MUTE);
}

void YamahaVfd::set_volume_db(float db) {
  this->want_db_ = db;
  this->start_op_(Op::VOLUME);
}

void YamahaVfd::select_input(const std::string &name) {
  for (const auto &in : INPUTS) {
    if (name == in.name && in.code != 0) {
      this->want_str_ = name;
      this->want_code_ = in.code;
      this->start_op_(Op::SOURCE);
      return;
    }
  }
  ESP_LOGW(TAG, "no IR code for input '%s'", name.c_str());
}

void YamahaVfd::run_op_() {
  if (this->op_ == Op::NONE)
    return;
  const uint32_t now = millis();
  const bool expired = this->deadline_ == 0 || (int32_t) (now - this->deadline_) >= 0;

  switch (this->op_) {
    case Op::PROBE:
      if (this->deadline_ == 0) {
        this->seq_ref_ = this->vol_seq_;
        this->probe_frames_ = this->frames_ok_;
        // VOL- rather than VOL+: the first press only shows the volume, and if
        // the volume screen happened to be up already it lowers by 1 dB instead
        // of raising it.
        this->send_nec_(CODE_VOL_DOWN);
        this->deadline_ = millis() + 1000;
      } else if (this->vol_seq_ != this->seq_ref_) {
        // The volume screen answered: on, and not muted (IR volume does
        // nothing while muted).
        this->on_ = true;
        this->on_known_ = true;
        if (!this->mute_known_) {
          this->muted_ = false;
          this->mute_known_ = true;
          this->mute_seq_++;
        }
        this->finish_("probe: receiver is on");
      } else if (this->frames_ok_ != this->probe_frames_) {
        // Any display traffic answers the press: with mute on, VOL+ does not
        // show the volume, but the display is still alive. In standby the
        // display stays silent.
        this->on_ = true;
        this->on_known_ = true;
        this->finish_("probe: receiver is on");
      } else if (expired) {
        this->on_ = false;
        this->on_known_ = true;
        this->finish_("probe: no answer, receiver in standby");
      }
      return;

    case Op::POWER:
      if (this->on_known_ && this->on_ == this->want_bool_) {
        this->finish_(this->want_bool_ ? "power on: done" : "standby: done");
      } else if (expired) {
        if (this->attempts_ >= 3) {
          this->finish_("power: FAILED after 3 attempts");
          return;
        }
        this->attempts_++;
        this->send_nec_(this->want_bool_ ? CODE_POWER_ON : CODE_STANDBY);
        this->deadline_ = millis() + (this->want_bool_ ? 4000 : 1500);
      }
      return;

    case Op::SOURCE:
      if (this->input_ == this->want_str_) {
        this->finish_("input: done");
      } else if (expired) {
        if (this->attempts_ >= 3) {
          this->finish_("input: FAILED after 3 attempts");
          return;
        }
        this->attempts_++;
        this->send_nec_(this->want_code_);
        this->deadline_ = millis() + 1500;
      }
      return;

    case Op::MUTE:
      // MUTE is a toggle. The state comes from the MUTE
      // icon (CGRAM glyph 0) or the "AUDIO MUTE ON/OFF" screen.
      // Unknown state (e.g. right after a reboot): toggle once and let the
      // "AUDIO MUTE ON/OFF" screen tell us the result; a wrong result is
      // corrected by the next attempt. Only ever on an explicit request.
      if (this->mute_known_ && this->muted_ == this->want_bool_) {
        this->finish_("mute: done");
        return;
      }
      if (this->waiting_ && this->mute_seq_ != this->seq_ref_)
        this->waiting_ = false;  // got an answer: re-evaluate now
      if (!this->waiting_ || expired) {
        if (this->attempts_ >= 3) {
          this->finish_("mute: FAILED after 3 attempts");
          return;
        }
        this->attempts_++;
        this->seq_ref_ = this->mute_seq_;
        this->send_nec_(CODE_MUTE);
        this->waiting_ = true;
        this->deadline_ = millis() + 1500;
      }
      return;

    case Op::VOLUME: {
      if (this->on_known_ && !this->on_) {
        this->finish_("volume: receiver is off");
        return;
      }
      if (this->mute_known_ && this->muted_) {
        // Observed: IR VOL+/- does nothing while muted (the knob still works).
        this->finish_("volume: receiver is muted, not changing");
        return;
      }
      if (this->waiting_) {
        if (this->vol_seq_ != this->seq_ref_) {
          this->waiting_ = false;
          this->misses_ = 0;
          // The value moved against our step: someone turned the knob. The
          // user wins, stop here.
          if (this->last_was_step_ && (this->volume_db_ - this->prev_db_) * this->last_dir_ < 0) {
            this->finish_("volume: stopped, manual change detected");
            return;
          }
          if (this->last_was_step_) {
            // Same value after a real step = limit reached (or the press only
            // woke the screen). Two in a row: stop.
            if (this->volume_db_ == this->prev_db_) {
              if (++this->same_count_ >= 2) {
                this->finish_("volume: limit reached");
                return;
              }
            } else {
              this->same_count_ = 0;
            }
          }
        } else if (expired) {
          this->waiting_ = false;
          if (++this->misses_ > 4) {
            this->finish_("volume: FAILED, no response");
            return;
          }
        } else {
          return;
        }
      }
      // Volume screen times out after ~3 s; the first press after that only
      // shows the volume, so wake it first.
      const bool screen = this->volume_valid_ && (now - this->last_volume_ms_) < 2000;
      if (!screen) {
        this->seq_ref_ = this->vol_seq_;
        this->last_was_step_ = false;
        this->send_nec_(CODE_VOL_DOWN);  // wake press, same reasoning as the probe
        this->waiting_ = true;
        this->deadline_ = millis() + 1000;
        return;
      }
      const float diff = this->want_db_ - this->volume_db_;
      if (std::fabs(diff) < 0.25f) {
        this->finish_("volume: done");
        return;
      }
      // Step size (1 dB per IR press, 0.5 dB on the knob) need not match the
      // target grid: stop as soon as the next step would reverse direction,
      // i.e. we are at the closest reachable value.
      const int8_t dir = diff > 0 ? 1 : -1;
      if (this->last_dir_ != 0 && dir != this->last_dir_) {
        this->finish_("volume: done (closest step)");
        return;
      }
      if (this->steps_ >= 250) {
        this->finish_("volume: FAILED, too many steps");
        return;
      }
      this->prev_db_ = this->volume_db_;
      this->seq_ref_ = this->vol_seq_;
      this->last_was_step_ = true;
      this->last_dir_ = dir;
      this->send_nec_(dir > 0 ? CODE_VOL_UP : CODE_VOL_DOWN);
      this->waiting_ = true;
      this->deadline_ = millis() + 600;
      this->steps_++;
      return;
    }

    case Op::NONE:
      return;
  }
}

}  // namespace yamaha_vfd
}  // namespace esphome
