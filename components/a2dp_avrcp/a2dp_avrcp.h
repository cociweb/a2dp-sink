#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP_AVRCP)

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/a2dp/a2dp.h"
#include "esphome/components/media_player/media_player.h"

#include <cmath>

namespace esphome::a2dp {

/**
 * @brief AVRCP controller/target subcomponent — Parented<A2DP>.
 *
 * Registers callbacks on the hub for volume changes and exposes
 * transport control actions (play/pause/next/previous/stop/vol±).
 *
 * Volume range: 0-127 (Bluetooth AVRCP spec).
 */
class A2DPAVRCP : public Component, public Parented<A2DP> {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_media_player(media_player::MediaPlayer *player) { this->media_player_ = player; }
  void set_sync_volume(bool sync) { this->sync_volume_ = sync; }

  void setup() override {
    this->parent_->add_on_avrcp_volume_callback([this](uint8_t volume) {
      this->volume_callback_.call(volume);
      if (this->sync_volume_)
        this->apply_phone_volume_(volume);
    });
    if (this->sync_volume_ && this->media_player_ != nullptr) {
      this->parent_->set_avrcp_volume_source(
          [](void *ctx) { return static_cast<A2DPAVRCP *>(ctx)->player_volume_(); }, this);
      this->media_player_->add_on_state_callback([this](media_player::MediaPlayerState) {
        // A volume change can bounce for a moment while the player and the phone round it.
        // Only publish a value that stays put.
        const uint8_t volume = this->player_volume_();
        if (volume == this->last_published_)
          return;
        this->last_published_ = volume;
        this->set_timeout("avrcp_volume", 200, [this, volume]() {
          const uint8_t settled = this->player_volume_();
          if (settled == volume)
            this->parent_->set_avrcp_volume(settled);
          else
            this->last_published_ = settled;
        });
      });
    }
  }

  void dump_config() override {
    static const char *const TAG = "a2dp_avrcp";
    ESP_LOGCONFIG(TAG, "A2DP AVRCP:");
    ESP_LOGCONFIG(TAG, "  CT connected: %s", this->parent_->is_avrcp_ct_connected() ? "yes" : "no");
  }

  template<typename F>
  void add_on_volume_callback(F &&callback) {
    this->volume_callback_.add(std::forward<F>(callback));
  }

  void play()           { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_PLAY, true); }
  void pause()          { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_PAUSE); }
  void play_pause() {
    this->parent_->send_avrc_passthrough(
        this->parent_->is_audio_streaming() ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY,
        !this->parent_->is_audio_streaming());
  }
  void next_track()     { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_FORWARD, true); }
  void previous_track() { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_BACKWARD, true); }
  void stop()           { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_PAUSE); }
  void volume_up()      { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_VOL_UP); }
  void volume_down()    { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_VOL_DOWN); }

  uint8_t get_volume() const { return this->parent_->get_avrcp_volume(); }

  CallbackManager<void(uint8_t)> volume_callback_;

 protected:
  // Phone absolute volume becomes the media player volume. A local change of
  // that volume is reported back, so the phone slider follows it.
  void apply_phone_volume_(uint8_t volume) {
    if (this->media_player_ == nullptr)
      return;
    const float target = static_cast<float>(volume) / 127.0f;
    if (std::fabs(this->media_player_->volume - target) < (1.0f / 127.0f))
      return;
    this->media_player_->make_call().set_volume(target).perform();
  }

  uint8_t player_volume_() const {
    int volume = static_cast<int>(std::lround(this->media_player_->volume * 127.0f));
    if (volume < 0)
      volume = 0;
    if (volume > 127)
      volume = 127;
    return static_cast<uint8_t>(volume);
  }

  media_player::MediaPlayer *media_player_{nullptr};
  bool sync_volume_{false};
  uint8_t last_published_{255};
};

// ---------------------------------------------------------------------------
// Automation action classes
// ---------------------------------------------------------------------------

template<typename... Ts>
class A2DPAVRCPPlayAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->play(); }
};

template<typename... Ts>
class A2DPAVRCPPauseAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->pause(); }
};

template<typename... Ts>
class A2DPAVRCPPlayPauseAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->play_pause(); }
};

template<typename... Ts>
class A2DPAVRCPNextAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->next_track(); }
};

template<typename... Ts>
class A2DPAVRCPPreviousAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->previous_track(); }
};

template<typename... Ts>
class A2DPAVRCPStopAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};

template<typename... Ts>
class A2DPAVRCPVolumeUpAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->volume_up(); }
};

template<typename... Ts>
class A2DPAVRCPVolumeDownAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->volume_down(); }
};

}  // namespace esphome::a2dp

#endif  // USE_ESP32 && USE_A2DP_AVRCP
