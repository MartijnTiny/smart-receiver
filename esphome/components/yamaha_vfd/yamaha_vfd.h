#pragma once

// Yamaha RX-V540RDS display-bus sniffer + IR control (Smart Receiver, Phase 3).
// Protocol: see "Display bus protocol (observed).md" in the project.

#include <cstdint>
#include <functional>
#include <string>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/core/hal.h"
#include "esphome/components/remote_transmitter/remote_transmitter.h"
#include "esp_intr_alloc.h"

namespace esphome {
namespace yamaha_vfd {

static const size_t VFD_FRAME_MAX = 32;   // longest real frame: 14 chars
static const size_t VFD_RING_SIZE = 256;  // frames buffered between ISR and loop()

struct RawFrame {
  uint16_t bits;
  uint8_t d[VFD_FRAME_MAX];
};

enum class Op : uint8_t { NONE, PROBE, POWER, SOURCE, MUTE, VOLUME };

class YamahaVfd : public Component {
 public:
  void set_pins(uint8_t cs, uint8_t sck, uint8_t data) {
    this->cs_pin_ = cs;
    this->sck_pin_ = sck;
    this->data_pin_ = data;
  }
  void set_transmitter(remote_transmitter::RemoteTransmitterComponent *tx) { this->tx_ = tx; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ---- state, for template entities ----
  bool power_known() const { return this->on_known_; }
  bool is_on() const { return this->on_; }
  bool mute_known() const { return this->mute_known_; }
  bool is_muted() const { return this->muted_; }
  bool has_volume() const { return this->volume_valid_; }
  float volume_db() const { return this->volume_db_; }
  std::string input() const { return this->input_; }
  std::string decoder() const { return this->decoder_; }
  std::string program() const { return this->program_; }
  std::string program_group() const;  // UPPERCASE parent of a sub-program (inferred grouping)
  std::string tuner() const { return this->tuner_; }
  std::string display_text() const;
  uint32_t frames_ok() const { return this->frames_ok_; }
  uint32_t frames_bad() const { return this->frames_bad_; }
  uint32_t frames_dropped() const { return this->dropped_; }
  bool busy() const { return this->op_ != Op::NONE; }

  // ---- control (each verified against the display bus, with retries) ----
  void set_power(bool on);
  void set_mute(bool mute);
  void set_volume_db(float db);
  void select_input(const std::string &name);
  void probe();                       // VOL- "wake" press: learns power + volume
  void send_command(uint8_t cmd);     // raw Yamaha AMP code (custom 0x7A), no verification
  void set_trace(bool on) { this->trace_ = on; }  // log every bus frame (hex + text)

  // Called whenever the decoded state or display text changes.
  void add_on_change_callback(std::function<void()> &&cb) { this->change_callback_.add(std::move(cb)); }

  // Called from the GPIO ISR trampoline.
  void capture_burst();

 protected:
  void handle_frame_(const RawFrame &f);
  void on_text_write_(uint8_t start, uint8_t len);
  void on_glyph_();
  void run_op_();
  void start_op_(Op op);
  void finish_(const char *msg);
  void send_nec_(uint8_t cmd);
  std::string raw_text_() const;
  std::string state_signature_() const;

  // pins / hardware
  uint8_t cs_pin_{5}, sck_pin_{18}, data_pin_{23};
  uint32_t cpu_per_us_{240};
  intr_handle_t intr_{nullptr};
  remote_transmitter::RemoteTransmitterComponent *tx_{nullptr};

  // ISR -> loop ring buffer (single producer = ISR, single consumer = loop, same core)
  RawFrame ring_[VFD_RING_SIZE];
  volatile uint16_t head_{0};
  volatile uint16_t tail_{0};
  volatile uint32_t dropped_{0};

  // shadow of the M66003 state
  uint8_t ram_[16]{};
  uint8_t glyph_[16][5]{};
  int16_t pending_addr_{-1};

  // decoded state
  bool on_known_{false}, on_{false};
  bool mute_known_{false}, muted_{false};
  bool volume_valid_{false};
  float volume_db_{0};
  std::string input_, decoder_, program_, tuner_;
  uint32_t last_volume_ms_{0};
  uint32_t vol_seq_{0}, mute_seq_{0};
  // The MUTE icon blinks while muted (glyph rewritten with the bit toggling):
  // a cleared bit only counts after no set bit has followed for a while.
  uint32_t mute_icon_set_ms_{0};
  uint32_t mute_icon_clear_ms_{0};
  bool mute_clear_pending_{false};
  uint32_t frames_ok_{0}, frames_bad_{0};

  // command state machine
  Op op_{Op::NONE};
  uint8_t attempts_{0};
  uint8_t misses_{0};
  uint8_t same_count_{0};
  uint16_t steps_{0};
  bool waiting_{false};
  bool last_was_step_{false};
  int8_t last_dir_{0};  // +1 / -1: direction of the previous volume step
  bool want_bool_{false};
  float want_db_{0};
  float prev_db_{0};
  uint8_t want_code_{0};
  std::string want_str_;
  uint32_t deadline_{0};
  uint32_t seq_ref_{0};
  uint32_t probe_at_{0};
  uint32_t probe_frames_{0};
  bool trace_{false};

  // change notification (push instead of poll)
  CallbackManager<void()> change_callback_;
  std::string last_sig_;

  // Last input survives ESP reboots: the receiver only redraws the input
  // icon/name on a change or at its own power-on, so it cannot be re-read.
  ESPPreferenceObject input_pref_;
  std::string saved_input_;
};

class ChangeTrigger : public Trigger<> {
 public:
  explicit ChangeTrigger(YamahaVfd *parent) {
    parent->add_on_change_callback([this]() { this->trigger(); });
  }
};

}  // namespace yamaha_vfd
}  // namespace esphome
