#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK) && defined(USE_SENSOR)

#include "esphome/components/a2dp/a2dp.h"
#include "esphome/components/a2dp_sink/a2dp_sink.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome::a2dp_sink {

enum A2DPSinkSensorType {
  A2DP_SINK_SENSOR_BITRATE,
  A2DP_SINK_SENSOR_BIT_DEPTH,
};

/// @brief Sensor that reports A2DP audio stream parameters (bitrate or bit depth).
class A2DPSinkSensor : public sensor::Sensor, public Component, public Parented<A2DPSink> {
 public:
  void set_sensor_type(A2DPSinkSensorType type) { this->sensor_type_ = type; }

  void setup() override {
    this->parent_->get_parent()->add_on_audio_cfg_callback([this](uint16_t sample_rate, uint8_t channels,
                                                                     uint32_t bitrate, uint8_t bits_per_sample) {
      switch (this->sensor_type_) {
        case A2DP_SINK_SENSOR_BITRATE:
          this->publish_state(bitrate);
          break;
        case A2DP_SINK_SENSOR_BIT_DEPTH:
          this->publish_state(bits_per_sample);
          break;
      }
    });
  }

  void dump_config() override {
    static const char *const TAG = "a2dp_sink.sensor";
    const char *type_str = this->sensor_type_ == A2DP_SINK_SENSOR_BITRATE ? "bitrate" : "bit_depth";
    ESP_LOGCONFIG(TAG, "  A2DP Sink %s:", type_str);
    LOG_SENSOR("", "A2DP Sink", this);
  }

  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

 protected:
  A2DPSinkSensorType sensor_type_{A2DP_SINK_SENSOR_BITRATE};
};

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK && USE_SENSOR
