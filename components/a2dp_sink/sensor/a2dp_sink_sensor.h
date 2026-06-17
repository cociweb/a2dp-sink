#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK) && defined(USE_SENSOR)

#include "esphome/components/a2dp/a2dp.h"
#include "esphome/components/a2dp_sink/a2dp_sink.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome::a2dp_sink {

/// @brief Sensor that reports the actual bitrate of the connected A2DP source.
class A2DPSinkBitrateSensor : public sensor::Sensor, public Component, public Parented<A2DPSink> {
 public:
  void setup() override {
    this->parent_->get_parent()->add_on_audio_cfg_callback([this](uint16_t sample_rate, uint8_t channels,
                                                                     uint32_t bitrate, uint8_t bits_per_sample) {
      this->publish_state(bitrate);
    });
  }

  void dump_config() override {
    static const char *const TAG = "a2dp_sink.sensor.bitrate";
    LOG_SENSOR("", "A2DP Sink Bitrate", this);
  }

  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
};

/// @brief Sensor that reports the actual bit depth of the connected A2DP source.
class A2DPSinkBitDepthSensor : public sensor::Sensor, public Component, public Parented<A2DPSink> {
 public:
  void setup() override {
    this->parent_->get_parent()->add_on_audio_cfg_callback([this](uint16_t sample_rate, uint8_t channels,
                                                                     uint32_t bitrate, uint8_t bits_per_sample) {
      this->publish_state(bits_per_sample);
    });
  }

  void dump_config() override {
    static const char *const TAG = "a2dp_sink.sensor.bit_depth";
    LOG_SENSOR("", "A2DP Sink Bit Depth", this);
  }

  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
};

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK && USE_SENSOR
