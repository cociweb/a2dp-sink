#include "a2dp_sink_media_source.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK) && defined(USE_MEDIA_SOURCE)

#include "esphome/core/application.h"
#include "esphome/core/log.h"

static const char *const TAG = "a2dp_sink.media_source";

static constexpr char A2DP_URI[] = "a2dp://stream";

namespace esphome::a2dp_sink {

// ---------------------------------------------------------------------------
// Static task trampoline
// ---------------------------------------------------------------------------

void A2DPSinkMediaSource::s_reader_task_(void *arg) {
  static_cast<A2DPSinkMediaSource *>(arg)->reader_task_();
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void A2DPSinkMediaSource::setup() {
  ESP_LOGCONFIG(TAG, "Setting up A2DP Sink Media Source...");

  this->event_group_ = xEventGroupCreate();
  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event group");
    this->mark_failed();
    return;
  }

  // BT audio streaming state changes → update event bits so the reader task reacts.
  // NOTE: these lambdas run on the main loop thread (dispatched via the event queue
  // in A2DPSink::loop()), so xEventGroupSetBits is safe here.
  this->parent_->add_on_audio_streaming_callback([this](bool streaming) {
    if (this->pending_stop_)
      return;
    if (this->get_state() == media_source::MediaSourceState::IDLE)
      return;
    if (streaming) {
      xEventGroupClearBits(this->event_group_, EVT_CMD_DRAIN | EVT_CMD_PAUSE);
      xEventGroupSetBits(this->event_group_, EVT_CMD_START);
    } else {
      // Clear EVT_CMD_START so the reader's drain branch does not immediately
      // mistake the still-set start bit for a stream resume: leaving it set would
      // cancel the drain every iteration, so the task would never finish draining,
      // never suspend, and instead spin on the empty ring buffer counting underruns.
      xEventGroupClearBits(this->event_group_, EVT_CMD_START);
      xEventGroupSetBits(this->event_group_, EVT_CMD_DRAIN);
    }
  });

  this->parent_->get_parent()->add_on_connection_callback([this](bool connected) {
    if (this->pending_stop_)
      return;
    if (!connected && this->get_state() != media_source::MediaSourceState::IDLE) {
      xEventGroupSetBits(this->event_group_, EVT_CMD_DRAIN);
    }
  });

#ifdef USE_A2DP_AVRCP
  this->parent_->add_on_avrcp_track_change_callback([this]() {
    if (this->pending_stop_)
      return;
    if (this->get_state() != media_source::MediaSourceState::IDLE)
      xEventGroupSetBits(this->event_group_, EVT_CMD_FLUSH);
  });
#endif
}

void A2DPSinkMediaSource::loop() {
  if (this->event_group_ == nullptr)
    return;

  EventBits_t bits = xEventGroupGetBits(this->event_group_);

  // Task wants to transition the orchestrator to IDLE.
  if (bits & EVT_TASK_WANT_IDLE) {
    xEventGroupClearBits(this->event_group_, EVT_TASK_WANT_IDLE);
    if (!this->pending_stop_)
      this->set_state_(media_source::MediaSourceState::IDLE);
  }

  // Task has suspended and is safe to deallocate.
  if (bits & EVT_TASK_SUSPENDED) {
    xEventGroupClearBits(this->event_group_, EVT_TASK_SUSPENDED);
    this->task_.deallocate();
    if (this->pending_stop_) {
      this->pending_stop_ = false;
    }
  }

  if (this->debug_logging_) {
    uint32_t now = millis();
    if ((now - this->diag_last_log_at_) >= DIAG_LOG_INTERVAL_MS) {
      uint32_t interval_ms = now - this->diag_last_log_at_;
      this->diag_last_log_at_ = now;
      uint32_t loops = this->diag_loops_.load(std::memory_order_relaxed);
      uint32_t loop_rate = interval_ms > 0 ? ((loops - this->diag_prev_loops_) * 1000) / interval_ms : 0;
      this->diag_prev_loops_ = loops;
      uint32_t stack_free = this->diag_min_stack_free_.load(std::memory_order_relaxed);
      if (stack_free == 0xFFFFFFFFu)
        stack_free = 0;  // Reader task has not run yet.
      ESP_LOGD(TAG,
               "reader diag: loop_rate=%u/s underruns=%u partial_writes=%u written=%llu KB min_stack_free=%u B",
               (unsigned) loop_rate, (unsigned) this->diag_underruns_.load(std::memory_order_relaxed),
               (unsigned) this->diag_partial_writes_.load(std::memory_order_relaxed),
               (unsigned long long) (this->diag_written_bytes_.load(std::memory_order_relaxed) / 1024),
               (unsigned) stack_free);
    }
  }
}

void A2DPSinkMediaSource::dump_config() {
  ESP_LOGCONFIG(TAG, "A2DP Sink Media Source:");
  ESP_LOGCONFIG(TAG, "  Task Stack: %s", this->task_stack_in_psram_ ? "PSRAM" : "internal");
}

// ---------------------------------------------------------------------------
// MediaSource interface
// ---------------------------------------------------------------------------

bool A2DPSinkMediaSource::can_handle(const std::string &uri) const {
  return uri == A2DP_URI;
}

bool A2DPSinkMediaSource::play_uri(const std::string &uri) {
  if (!this->can_handle(uri))
    return false;

  if (this->get_state() == media_source::MediaSourceState::PLAYING) {
    ESP_LOGD(TAG, "play_uri: already playing");
    return true;
  }
  if (!this->parent_->get_parent()->is_enabled()) {
    ESP_LOGW(TAG, "play_uri: a2dp hub is not enabled");
    return false;
  }

  // Flush stale data from a previous session.
  this->parent_->get_parent()->set_audio_output_enabled(true);
  this->parent_->get_parent()->reset_audio_buffer();

  xEventGroupClearBits(this->event_group_, EVT_ALL_BITS);
  xEventGroupSetBits(this->event_group_, EVT_CMD_START);
  this->pending_stop_ = false;

  this->start_task_();
  this->set_state_(media_source::MediaSourceState::PLAYING);
  ESP_LOGI(TAG, "Started — waiting for Bluetooth audio");
  return true;
}

void A2DPSinkMediaSource::handle_command(media_source::MediaSourceCommand command) {
  switch (command) {
    case media_source::MediaSourceCommand::STOP:
      ESP_LOGI(TAG, "STOP");
      this->parent_->get_parent()->set_audio_output_enabled(false);
      this->parent_->get_parent()->request_audio_suspend();
#ifdef USE_A2DP_AVRCP
      this->parent_->get_parent()->send_avrc_passthrough(ESP_AVRC_PT_CMD_PAUSE);
#endif
      xEventGroupClearBits(this->event_group_, EVT_ALL_CMD_BITS | EVT_TASK_WANT_IDLE);
      xEventGroupSetBits(this->event_group_, EVT_CMD_STOP);
      if (this->task_.is_created()) {
        this->pending_stop_ = true;
        this->set_state_(media_source::MediaSourceState::IDLE);
      } else {
        this->pending_stop_ = false;
        this->set_state_(media_source::MediaSourceState::IDLE);
      }
      break;

    case media_source::MediaSourceCommand::PAUSE:
      if (this->get_state() == media_source::MediaSourceState::PLAYING) {
        ESP_LOGI(TAG, "PAUSE");
        xEventGroupClearBits(this->event_group_, EVT_CMD_START);
        xEventGroupSetBits(this->event_group_, EVT_CMD_PAUSE);
        this->set_state_(media_source::MediaSourceState::PAUSED);
      }
      break;

    case media_source::MediaSourceCommand::PLAY:
      if (this->get_state() == media_source::MediaSourceState::PAUSED) {
        ESP_LOGI(TAG, "PLAY (resume)");
        xEventGroupClearBits(this->event_group_, EVT_CMD_PAUSE);
        xEventGroupSetBits(this->event_group_, EVT_CMD_START);
        this->set_state_(media_source::MediaSourceState::PLAYING);
      }
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Task management
// ---------------------------------------------------------------------------

void A2DPSinkMediaSource::start_task_() {
  if (this->task_.is_created())
    return;
  if (!this->task_.create(s_reader_task_, "a2dp_reader", READER_TASK_STACK, this, READER_TASK_PRIORITY,
                          this->task_stack_in_psram_)) {
    ESP_LOGE(TAG, "Failed to create reader task");
  }
}

// ---------------------------------------------------------------------------
// Pre-roll (jitter buffer priming)
// ---------------------------------------------------------------------------

A2DPSinkMediaSource::PrerollResult A2DPSinkMediaSource::wait_for_preroll_(uint32_t output_delay_ms) {
  if (output_delay_ms == 0)
    return PrerollResult::PROCEED;

  uint32_t sample_rate = this->parent_->get_actual_sample_rate();
  uint8_t channels = this->parent_->get_actual_channels();
  if (sample_rate == 0)
    sample_rate = 44100;
  if (channels == 0)
    channels = 2;

  // Bytes of 16-bit PCM equivalent to output_delay_ms of playback.
  uint64_t target = (uint64_t) sample_rate * channels * sizeof(int16_t) * output_delay_ms / 1000;
  // Never gate on more than half the ring buffer, otherwise a large output delay
  // could stall playback or force the writer to drop the audio we are waiting for.
  size_t ring_size = this->parent_->get_parent()->get_ring_buffer_size();
  if (ring_size > 0 && target > ring_size / 2)
    target = ring_size / 2;
  if (target == 0)
    return PrerollResult::PROCEED;

  // Once audio is actually flowing, cap how long we wait to reach the target so a
  // slow / low-bitrate source cannot delay playback indefinitely.
  const uint32_t fill_timeout_ms = output_delay_ms + 500;
  uint32_t waited = 0;
  while (true) {
    EventBits_t bits = xEventGroupGetBits(this->event_group_);
    if (bits & EVT_CMD_STOP)
      return PrerollResult::STOP;
    // Streaming stopped again before we finished priming — let the drain path run.
    if ((bits & EVT_CMD_DRAIN) && !(bits & EVT_CMD_START))
      return PrerollResult::PROCEED;

    size_t fill = this->parent_->get_parent()->get_ring_buffer_fill();
    if (fill >= target)
      return PrerollResult::PROCEED;

    if (fill == 0) {
      // No audio yet (e.g. Bluetooth has not started streaming): don't count this
      // against the fill timeout so we keep waiting for the stream to begin.
      waited = 0;
    } else {
      waited += IDLE_POLL_MS;
      if (waited >= fill_timeout_ms)
        return PrerollResult::PROCEED;
    }
    vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
  }
}

// ---------------------------------------------------------------------------
// Reader task — NEVER calls set_state_() directly; uses event bits instead.
// ---------------------------------------------------------------------------

void A2DPSinkMediaSource::reader_task_() {
  ESP_LOGD(TAG, "Reader task started");

  const uint32_t drain_ms = this->parent_->get_pcm_drain_throttle_ms();
  const uint32_t output_delay_ms = this->parent_->get_output_delay_ms();
  uint8_t zero_write_count = 0;
  // Set once the downstream speaker pipeline has accepted at least one write. Until then
  // the pipeline is still warming up (its write_audio() returns 0 for a few hundred ms),
  // so early zero-writes must not be mistaken for a dead downstream and tear down the
  // stream. See ZERO_WRITE_STARTUP_STOP_COUNT.
  bool have_written_output = false;
  // Pre-roll (jitter buffer priming) is armed at start and re-armed on every
  // streaming (re)start so the ring buffer is re-primed after sniff-induced
  // stop/start cycles — not just once when the task is first created.
  bool need_preroll = true;

  auto audio_source =
      audio::RingBufferAudioSource::create(this->parent_->get_ring_buffer(), READER_CHUNK_SIZE, 2 * sizeof(int16_t));
  if (audio_source == nullptr) {
    ESP_LOGE(TAG, "Failed to create ring buffer audio source");
    xEventGroupSetBits(this->event_group_, EVT_TASK_WANT_IDLE | EVT_TASK_SUSPENDED);
    App.wake_loop_threadsafe();
    vTaskSuspend(nullptr);
    return;
  }

  // Main read loop.
  while (true) {
    if (this->debug_logging_) {
      this->diag_loops_.fetch_add(1, std::memory_order_relaxed);
      uint32_t stack_free = uxTaskGetStackHighWaterMark(nullptr);
      uint32_t prev = this->diag_min_stack_free_.load(std::memory_order_relaxed);
      while (stack_free < prev &&
             !this->diag_min_stack_free_.compare_exchange_weak(prev, stack_free, std::memory_order_relaxed)) {
      }
    }
    EventBits_t bits = xEventGroupGetBits(this->event_group_);

    if (bits & EVT_CMD_STOP)
      goto task_exit_no_idle;

    if (bits & EVT_CMD_FLUSH) {
      xEventGroupClearBits(this->event_group_, EVT_CMD_FLUSH | EVT_CMD_DRAIN);
      if (audio_source->available() > 0)
        audio_source->consume(audio_source->available());
      this->parent_->get_parent()->reset_audio_buffer();
      continue;
    }

    if (bits & EVT_CMD_DRAIN) {
      // BT audio stopped: drain remaining ring buffer data, then signal IDLE.
      uint32_t drain_waited = 0;
      while (drain_waited < drain_ms) {
        bits = xEventGroupGetBits(this->event_group_);
        if (bits & EVT_CMD_STOP)
          goto task_exit_no_idle;
        if (bits & EVT_CMD_START) {
          // BT resumed streaming — cancel drain and re-prime the jitter buffer.
          xEventGroupClearBits(this->event_group_, EVT_CMD_DRAIN);
          need_preroll = true;
          goto read_chunk;
        }
        if (audio_source->available() == 0) {
          audio_source->fill(pdMS_TO_TICKS(RB_READ_TIMEOUT_MS), false);
        }
        size_t available = audio_source->available();
        if (available == 0) {
          drain_waited += IDLE_POLL_MS;
          vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
          continue;
        }
        if (xEventGroupGetBits(this->event_group_) & EVT_CMD_STOP)
          goto task_exit_no_idle;
        audio::AudioStreamInfo info(16, this->parent_->get_actual_channels(),
                                    this->parent_->get_actual_sample_rate());
        size_t written = this->write_output(audio_source->data(), available, WRITE_TIMEOUT_MS, info);
        if (written > 0) {
          zero_write_count = 0;
          have_written_output = true;
          audio_source->consume(written);
        } else if (++zero_write_count >= ZERO_WRITE_STOP_COUNT) {
          this->parent_->get_parent()->set_audio_output_enabled(false);
          this->parent_->get_parent()->request_audio_suspend();
          goto task_exit_with_idle;
        }
      }
      // Drain timeout expired — signal the main loop to report IDLE.
      xEventGroupClearBits(this->event_group_, EVT_CMD_DRAIN);
      goto task_exit_with_idle;
    }

    if (bits & EVT_CMD_PAUSE) {
      vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
      continue;
    }

read_chunk:
    {
      if (need_preroll) {
        if (this->wait_for_preroll_(output_delay_ms) == PrerollResult::STOP)
          goto task_exit_no_idle;
        need_preroll = false;
      }
      if (audio_source->available() == 0) {
        audio_source->fill(pdMS_TO_TICKS(RB_READ_TIMEOUT_MS), false);
      }
      size_t available = audio_source->available();
      if (available == 0) {
        if (this->debug_logging_)
          this->diag_underruns_.fetch_add(1, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
        continue;
      }
      if (xEventGroupGetBits(this->event_group_) & EVT_CMD_STOP)
        goto task_exit_no_idle;
      audio::AudioStreamInfo info(16, this->parent_->get_actual_channels(),
                                  this->parent_->get_actual_sample_rate());
      size_t written = this->write_output(audio_source->data(), available, WRITE_TIMEOUT_MS, info);
      if (written > 0) {
        zero_write_count = 0;
        have_written_output = true;
        if (this->debug_logging_) {
          this->diag_written_bytes_.fetch_add(written, std::memory_order_relaxed);
          if (written < available)
            this->diag_partial_writes_.fetch_add(1, std::memory_order_relaxed);
        }
        audio_source->consume(written);
      } else if (++zero_write_count >=
                 (have_written_output ? ZERO_WRITE_STOP_COUNT : ZERO_WRITE_STARTUP_STOP_COUNT)) {
        // Downstream never accepted audio (before first write) or stopped accepting it
        // mid-stream (after first write): give up and release the BT source.
        this->parent_->get_parent()->set_audio_output_enabled(false);
        this->parent_->get_parent()->request_audio_suspend();
        goto task_exit_with_idle;
      }
    }
  }

task_exit_with_idle:
  ESP_LOGD(TAG, "Reader task: drain done, signalling IDLE");
  if (audio_source->available() > 0)
    audio_source->consume(audio_source->available());
  this->parent_->get_parent()->reset_audio_buffer();
  xEventGroupSetBits(this->event_group_, EVT_TASK_WANT_IDLE | EVT_TASK_SUSPENDED);
  App.wake_loop_threadsafe();
  vTaskSuspend(nullptr);
  return;

task_exit_no_idle:
  ESP_LOGD(TAG, "Reader task: stopped by command");
  if (audio_source->available() > 0)
    audio_source->consume(audio_source->available());
  this->parent_->get_parent()->reset_audio_buffer();
  xEventGroupSetBits(this->event_group_, EVT_TASK_SUSPENDED);
  App.wake_loop_threadsafe();
  vTaskSuspend(nullptr);
}

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK && USE_MEDIA_SOURCE
