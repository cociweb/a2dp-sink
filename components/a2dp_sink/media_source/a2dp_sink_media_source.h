#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK) && defined(USE_MEDIA_SOURCE)

#include "esphome/components/a2dp_sink/a2dp_sink.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/core/component.h"
#include "esphome/core/static_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace esphome::a2dp_sink {

/// @brief FreeRTOS task stack size (bytes) for the ring-buffer reader task.
static constexpr uint32_t READER_TASK_STACK = 4096;
/// @brief Priority for the reader task.
///
/// The reader is a realtime stage: it moves decoded PCM from the (PSRAM) ring buffer into
/// the small (~19 KB) downstream I2S speaker buffer, and must keep it topped up or the DAC
/// underflows and audio stutters. At the old priority (5) the task sat right at the real-time
/// edge — under background load (Wi-Fi housekeeping, sensors, and especially PSRAM-bus
/// contention when the BT stack and/or this task's stack also live in PSRAM) it was
/// descheduled long enough that its throughput dipped below the fixed A2DP arrival rate, so
/// the ring buffer filled and overflowed even with zero packet loss.
///
/// Raise it so the reader wakes promptly after each DMA drain and preempts non-realtime work,
/// giving comfortable headroom above real time. It stays well below the I2S speaker task (19,
/// the actual DAC feeder) and the networking/BT tasks, and the reader always blocks or
/// vTaskDelay()s when idle, so a higher priority never starves lower-priority tasks.
static constexpr UBaseType_t READER_TASK_PRIORITY = 10;
/// @brief Chunk size read per iteration in the reader task (bytes).
/// 2048 == 512 stereo 16-bit frames ≈ 11.5 ms at 44100 Hz.
///
/// Keep this small. Each iteration hands the chunk to write_output(), which ultimately
/// calls xRingbufferSend() on the downstream speaker's ring buffer (~19 KB) and only
/// succeeds if the WHOLE chunk fits atomically. A large chunk forces the reader to wait
/// for that small buffer to drain far enough to admit the whole write, so it blocks up to
/// WRITE_TIMEOUT_MS and frequently falls back to throttled partial writes — dropping
/// sustained throughput below the ~172 KB/s A2DP real-time rate and overflowing the PCM
/// ring buffer. Small, frequent writes match the DAC drain granularity and sustain
/// real-time throughput.
static constexpr size_t READER_CHUNK_SIZE = 2048;
/// @brief Max milliseconds the ring buffer will block waiting for data.
static constexpr uint32_t RB_READ_TIMEOUT_MS = 20;
/// @brief Timeout (ms) passed to write_output() per call.
static constexpr uint32_t WRITE_TIMEOUT_MS = 100;
/// @brief Polling interval (ms) when idle / draining.
static constexpr uint32_t IDLE_POLL_MS = 10;
static constexpr uint8_t ZERO_WRITE_STOP_COUNT = 3;
/// @brief Zero-write tolerance before the FIRST successful write (initial pipeline warm-up).
/// The speaker_source pipeline (mixer/resampler/speaker) can take several hundred ms to
/// start after play_uri, and its write_audio() legitimately returns 0 during that window.
/// Counting those early zero-writes against ZERO_WRITE_STOP_COUNT would suspend the BT
/// source before audio ever reaches the DAC, so allow a longer, bounded grace at startup
/// (~2 s at WRITE_TIMEOUT_MS per attempt) before giving up on a downstream that never runs.
static constexpr uint8_t ZERO_WRITE_STARTUP_STOP_COUNT = 20;
/// @brief Interval (ms) between low-frequency reader-task diagnostics log lines.
static constexpr uint32_t DIAG_LOG_INTERVAL_MS = 10000;

// --- Event bits: main loop → reader task ---
static constexpr EventBits_t EVT_CMD_START = BIT0;  ///< play_uri / resume
static constexpr EventBits_t EVT_CMD_STOP  = BIT1;  ///< stop immediately
static constexpr EventBits_t EVT_CMD_PAUSE = BIT2;  ///< pause output
static constexpr EventBits_t EVT_CMD_DRAIN = BIT3;  ///< BT audio stopped, drain buffer
static constexpr EventBits_t EVT_CMD_FLUSH = BIT6;  ///< track changed, discard stale buffered PCM

// --- Event bits: reader task → main loop ---
/// Task finished normally and wants the orchestrator notified of IDLE.
static constexpr EventBits_t EVT_TASK_WANT_IDLE  = BIT4;
/// Task has suspended; main loop may safely call task_.deallocate().
static constexpr EventBits_t EVT_TASK_SUSPENDED  = BIT5;

static constexpr EventBits_t EVT_ALL_CMD_BITS =
    EVT_CMD_START | EVT_CMD_STOP | EVT_CMD_PAUSE | EVT_CMD_DRAIN | EVT_CMD_FLUSH;
static constexpr EventBits_t EVT_ALL_BITS =
    EVT_ALL_CMD_BITS | EVT_TASK_WANT_IDLE | EVT_TASK_SUSPENDED;

/**
 * @brief A MediaSource that consumes audio data from an A2DPSink ring buffer.
 *
 * Accepted URI: "a2dp://stream"
 *
 * Lifecycle:
 *   - play_uri("a2dp://stream") → starts the reader FreeRTOS task, reports PLAYING.
 *   - BT source starts streaming → PCM data flows from the ring buffer to the
 *     speaker pipeline via write_output().
 *   - BT audio stopped / disconnected → drains the ring buffer for
 *     pcm_drain_throttle_ms_, suspends, then main loop reports IDLE.
 *   - handle_command(STOP) → signals task to stop; main loop reports IDLE.
 *
 * Threading:
 *   - set_state_() is only called from the main loop (loop() method).
 *   - The reader task signals state transitions via event bits and suspends;
 *     the main loop calls task_.deallocate() once it has processed the bits.
 */
class A2DPSinkMediaSource : public Component,
                            public media_source::MediaSource,
                            public Parented<A2DPSink> {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_task_stack_in_psram(bool v) { this->task_stack_in_psram_ = v; }
  void set_debug_logging(bool v) { this->debug_logging_ = v; }

  // --- MediaSource interface ---
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  bool can_handle(const std::string &uri) const override;

 protected:
  /// @brief Static trampoline for the FreeRTOS task.
  static void s_reader_task_(void *arg);
  /// @brief The reader task body.
  void reader_task_();

  /// @brief Result of the pre-roll (jitter buffer priming) wait.
  enum class PrerollResult {
    PROCEED,  ///< Target reached, timed out, or streaming stopped — continue the read loop.
    STOP,     ///< EVT_CMD_STOP was signalled — the task must exit without reporting IDLE.
  };

  /// @brief Block until the ring buffer holds @p output_delay_ms worth of PCM.
  ///
  /// Builds a jitter buffer so the speaker pipeline has headroom to absorb BT
  /// sniff cycles, WiFi roam scans, and PSRAM latency. Time spent waiting while
  /// the buffer is empty (e.g. before Bluetooth starts streaming) is not counted
  /// against the fill timeout, so the gate keeps waiting for the stream to begin
  /// but cannot hang forever once data is actually flowing. Abortable by STOP.
  PrerollResult wait_for_preroll_(uint32_t output_delay_ms);

  /// @brief Start the reader task (idempotent).
  void start_task_();

  StaticTask task_;
  EventGroupHandle_t event_group_{nullptr};
  bool task_stack_in_psram_{false};
  bool pending_stop_{false};

  // --- Diagnostics (reader task → main loop) ---
  bool debug_logging_{false};
  std::atomic<uint32_t> diag_underruns_{0};       ///< Times the ring buffer was empty while playing.
  std::atomic<uint32_t> diag_partial_writes_{0};  ///< write_output() accepted fewer bytes than offered.
  std::atomic<uint32_t> diag_loops_{0};           ///< Reader-task main-loop iterations (for loop rate).
  std::atomic<uint64_t> diag_written_bytes_{0};   ///< Total PCM bytes handed to the speaker pipeline.
  std::atomic<uint32_t> diag_min_stack_free_{0xFFFFFFFFu};  ///< Min reader-task stack watermark (bytes).
  uint32_t diag_last_log_at_{0};
  uint32_t diag_prev_loops_{0};
};

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK && USE_MEDIA_SOURCE
