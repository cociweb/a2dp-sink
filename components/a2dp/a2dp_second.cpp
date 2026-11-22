#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP)

// Experiment: accept a second A2DP peer without editing btc_av.c.
//
// Bluedroid registers one stream, and its state machine tracks one peer. A
// second incoming link is rejected, then the idle ACL is dropped after 4 s.
// BTA itself has two stream slots. We wrap BTA_AvEnable so events for a
// second slot stay here, and BTA_AvRegister so the first slot keeps a single
// SBC endpoint and one endpoint remains for the second slot. The second slot
// is registered only after the first peer is open, so a lone phone still
// finds one endpoint.
//
// The peer that is playing owns the sink. Its START, SUSPEND, STOP and audio
// reach the stock state machine, which does not look at the stream handle.
// A peer that starts while the other plays is suspended with AVDT, so it
// stops transmitting and the playing peer is not interrupted. A playing peer
// that sends no audio for a while is treated as paused, so the other peer can
// take over without waiting for the phone's own AVDT suspend.

#include "esp_log.h"
#include "esp_timer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" void osi_free_func(void *ptr);
extern "C" void *osi_malloc_func(size_t size);
// Posts onto the BTU task. The buffer must come from osi_malloc_func.
extern "C" void bta_sys_sendmsg(void *msg);
extern "C" void *bta_av_hndl_to_scb(uint16_t handle);
extern "C" void __real_bta_av_ssm_execute(void *scb, uint16_t event, void *data);
// tBTM_PM_PWR_MD
struct PwrMode {
  uint16_t max;
  uint16_t min;
  uint16_t attempt;
  uint16_t timeout;
  uint8_t mode;
};
extern "C" uint8_t __real_BTM_SetPowerMode(uint8_t pm_id, uint8_t *addr, PwrMode *mode);
extern "C" uint8_t BTM_ReadPowerMode(uint8_t *addr, uint8_t *mode);
extern "C" void bta_sys_set_policy(uint8_t id, uint8_t policy, uint8_t *addr);

static const char *const TAG = "a2dp.2nd";

static constexpr uint8_t CHNL_AUDIO = 0x40;
static constexpr uint8_t TSEP_SNK = 1;
static constexpr uint8_t APP_ID_SECOND = 1;

static constexpr uint8_t EVT_ENABLE = 0;
static constexpr uint8_t EVT_REGISTER = 1;
static constexpr uint8_t EVT_OPEN = 2;
static constexpr uint8_t EVT_CLOSE = 3;
static constexpr uint8_t EVT_START = 4;
static constexpr uint8_t EVT_STOP = 5;
static constexpr uint8_t EVT_PROTECT_REQ = 6;
static constexpr uint8_t EVT_PROTECT_RSP = 7;
static constexpr uint8_t EVT_RC_OPEN = 8;
static constexpr uint8_t EVT_RC_CLOSE = 9;
static constexpr uint8_t EVT_REMOTE_CMD = 10;
static constexpr uint8_t EVT_REMOTE_RSP = 11;
static constexpr uint8_t EVT_VENDOR_CMD = 12;
static constexpr uint8_t EVT_VENDOR_RSP = 13;
static constexpr uint8_t EVT_RECONFIG = 14;
static constexpr uint8_t EVT_SUSPEND = 15;
static constexpr uint8_t EVT_PENDING = 16;
static constexpr uint8_t EVT_META = 17;
static constexpr uint8_t EVT_REJECT = 18;
static constexpr uint8_t EVT_RC_FEAT = 19;
static constexpr uint8_t EVT_MEDIA_CFG = 20;
static constexpr uint8_t EVT_MEDIA_DATA = 21;
static constexpr uint8_t EVT_INCOMING = 28;

static constexpr uint8_t RC_NONE = 0xFF;

struct Cos {
  bool (*init)(uint8_t seid, uint8_t *codec_type, uint8_t *codec_info, uint8_t *num_protect, uint8_t *protect,
               uint8_t tsep);
  void (*disc_res)(uint8_t hndl, uint8_t num_seps, uint8_t num_snk, uint8_t num_src, uint8_t *addr, uint16_t uuid);
  void (*cfg_res)(uint8_t hndl, uint8_t num_seps, uint8_t num_snk, uint8_t num_src, uint8_t *addr, uint16_t uuid);
  uint8_t (*getcfg)(uint8_t hndl, uint8_t codec, uint8_t *codec_info, uint8_t *sep_idx, uint8_t seid,
                    uint8_t *num_protect, uint8_t *protect);
  void (*setcfg)(uint8_t hndl, uint8_t codec, uint8_t *codec_info, uint8_t seid, uint8_t *addr, uint8_t num_protect,
                 uint8_t *protect, uint8_t local_sep, uint8_t avdt_handle);
  void (*open)(uint8_t hndl, uint8_t codec, uint8_t *codec_info, uint16_t mtu);
  void (*close)(uint8_t hndl, uint8_t codec, uint16_t mtu);
  void (*start)(uint8_t hndl, uint8_t codec, uint8_t *codec_info, bool *no_rtp);
  void (*stop)(uint8_t hndl, uint8_t codec);
  void *(*data)(uint8_t codec, uint32_t *len, uint32_t *timestamp);
  void (*delay)(uint8_t hndl, uint16_t delay_value);
};

extern "C" const Cos bta_av_a2d_cos;

extern "C" void __real_BTA_AvEnable(uint16_t sec_mask, uint16_t features, void (*cb)(uint8_t event, void *data));
extern "C" void __real_BTA_AvRegister(uint8_t chnl, const char *name, uint8_t app_id, void (*data_cb)(uint8_t, void *),
                                      const void *cos, const void *rc_cos, uint8_t tsep);

static void (*real_cb_)(uint8_t event, void *data) = nullptr;

static uint8_t saved_chnl_ = CHNL_AUDIO;
static uint8_t saved_tsep_ = TSEP_SNK;
static const void *saved_rc_ = nullptr;
static bool have_reg_ = false;
static bool second_registered_ = false;

static uint8_t second_hndl_ = 0;
static uint8_t second_addr_[6] = {};
static bool have_second_addr_ = false;
static uint8_t second_rc_ = RC_NONE;

static uint8_t primary_addr_[6] = {};
static bool have_primary_ = false;

static uint8_t seps_taken_ = 0;

static bool same_addr_(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }

static bool is_second_addr_(const uint8_t *addr) { return have_second_addr_ && same_addr_(addr, second_addr_); }

static void remember_second_addr_(const uint8_t *addr) {
  memcpy(second_addr_, addr, 6);
  have_second_addr_ = true;
}

static void log_addr_(const char *what, const uint8_t *addr) {
  ESP_LOGI(TAG, "%s %02x:%02x:%02x:%02x:%02x:%02x", what, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static bool cos_init_(uint8_t seid, uint8_t *codec_type, uint8_t *codec_info, uint8_t *num_protect, uint8_t *protect,
                      uint8_t tsep) {
  // Each register starts at seid 0. Keep one SBC endpoint per stream so the
  // first registration does not consume both slots.
  if (seid == 0)
    seps_taken_ = 0;
  if (seps_taken_ >= 1)
    return false;
  const bool ok = bta_av_a2d_cos.init(seid, codec_type, codec_info, num_protect, protect, tsep);
  if (ok)
    seps_taken_++;
  return ok;
}

static void cos_disc_(uint8_t h, uint8_t a, uint8_t b, uint8_t c, uint8_t *d, uint16_t e) {
  bta_av_a2d_cos.disc_res(h, a, b, c, d, e);
}
static void cos_cfg_(uint8_t h, uint8_t a, uint8_t b, uint8_t c, uint8_t *d, uint16_t e) {
  bta_av_a2d_cos.cfg_res(h, a, b, c, d, e);
}
static uint8_t cos_get_(uint8_t h, uint8_t c, uint8_t *info, uint8_t *idx, uint8_t seid, uint8_t *np, uint8_t *p) {
  return bta_av_a2d_cos.getcfg(h, c, info, idx, seid, np, p);
}
static void cos_set_(uint8_t h, uint8_t c, uint8_t *info, uint8_t seid, uint8_t *addr, uint8_t np, uint8_t *p,
                     uint8_t local, uint8_t avdt) {
  bta_av_a2d_cos.setcfg(h, c, info, seid, addr, np, p, local, avdt);
}
static void cos_open_(uint8_t h, uint8_t c, uint8_t *info, uint16_t mtu) { bta_av_a2d_cos.open(h, c, info, mtu); }
static void cos_close_(uint8_t h, uint8_t c, uint16_t mtu) { bta_av_a2d_cos.close(h, c, mtu); }
static void cos_start_(uint8_t h, uint8_t c, uint8_t *info, bool *no_rtp) { bta_av_a2d_cos.start(h, c, info, no_rtp); }
static void cos_stop_(uint8_t h, uint8_t c) { bta_av_a2d_cos.stop(h, c); }
static void *cos_data_(uint8_t c, uint32_t *len, uint32_t *ts) { return bta_av_a2d_cos.data(c, len, ts); }
static void cos_delay_(uint8_t h, uint16_t d) { bta_av_a2d_cos.delay(h, d); }

static const Cos our_cos_ = {
    cos_init_, cos_disc_, cos_cfg_, cos_get_, cos_set_, cos_open_, cos_close_, cos_start_, cos_stop_, cos_data_,
    cos_delay_,
};

// BTA_AV_AP_STOP_EVT in IDF 6.1.1: BTA_SYS_EVT_START(BTA_ID_AV=18) plus the
// 12th enumerator in bta_av_int.h (API_DISABLE is 0). BTA_AvStop fans this
// out to every stream; posting it ourselves with layer_specific set routes
// it to one stream control block (bta_av_hdl_event).
static constexpr uint16_t AP_STOP_EVT = (18u << 8) + 12u;
// BTA_AV_API_CLOSE_EVT is the enumerator before AP_START.
static constexpr uint16_t API_CLOSE_EVT = (18u << 8) + 10u;

static constexpr uint8_t BTA_ID_AVK = 19;
static constexpr uint8_t BTA_ID_AV = 18;
static constexpr uint8_t HCI_ENABLE_SNIFF_MODE = 0x04;

struct StopMsg {
  uint16_t event;
  uint16_t len;
  uint16_t offset;
  uint16_t layer_specific;
  bool suspend;
  bool flush;
};

// tBTA_AV_SUSPEND (event data of BTA_AV_SUSPEND_EVT).
struct SuspendEvt {
  uint8_t chnl;
  uint8_t hndl;
  bool initiator;
  uint8_t status;
};

enum class Owner : uint8_t { NONE, PRIMARY, SECOND };

static Owner owner_ = Owner::NONE;
static uint8_t primary_hndl_ = 0;
static void (*stock_data_cb_)(uint8_t, void *) = nullptr;
static uint8_t *primary_cfg_ = nullptr;
static uint8_t *second_cfg_ = nullptr;
// Which stream's codec settings the stock decoder holds right now.
static Owner decoder_for_ = Owner::NONE;

// A suspend we asked for. If the phone times out or refuses, bta_av_suspend_cfm
// closes the whole stream. We drop that close for these streams.
static bool suspend_pending_first_ = false;
static bool suspend_pending_second_ = false;

// Silence after which the playing peer is reported as paused. Five seconds is the
// short end of the phone's own AVDT suspend (5-10 s), so a brief gap does not
// look like a pause and a real pause is not left waiting on the phone.
static constexpr uint32_t STALL_MS = 5000;
// Silence after which another peer's audio or START takes the sink over.
static constexpr uint32_t TAKEOVER_SILENCE_MS = 1000;

// Set by a real SUSPEND or STOP, cleared by START. Trailing audio of a
// suspended stream never claims the sink.
// Time of the last SUSPEND/STOP (0 when none). A phone may resume audio
// without a new START, so only audio right after the suspend is trailing.
static uint32_t first_suspended_ = 0;
static uint32_t second_suspended_ = 0;
static constexpr uint32_t TRAILING_AUDIO_MS = 500;
// Time of our last AVDT suspend request per stream, to avoid repeating it.
static constexpr uint32_t RESUSPEND_MS = 3000;
static uint32_t suspend_req_ms_[2] = {0, 0};
static bool second_open_ = false;
static volatile uint32_t last_data_ms_ = 0;
static esp_timer_handle_t stall_timer_ = nullptr;

static uint32_t now_ms_() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

static uint8_t hndl_of_(Owner who) { return who == Owner::SECOND ? second_hndl_ : primary_hndl_; }

// tBTA_AV_START: chnl, hndl, status, initiator, suspending.
struct StartEvt {
  uint8_t chnl;
  uint8_t hndl;
  uint8_t status;
  bool initiator;
  bool suspending;
};

// The stock callback copies sizeof(tBTA_AV) bytes, so pass a zeroed, larger buffer.
union StockEvent {
  StartEvt start;
  SuspendEvt suspend;
  uint8_t raw[256];
};

static void suspend_stream_(uint8_t hndl) {
  suspend_req_ms_[hndl == second_hndl_ ? 1 : 0] = now_ms_();
  if (hndl == second_hndl_)
    suspend_pending_second_ = true;
  else
    suspend_pending_first_ = true;
  auto *msg = static_cast<StopMsg *>(osi_malloc_func(sizeof(StopMsg)));
  if (msg == nullptr) {
    ESP_LOGW(TAG, "no memory to suspend stream 0x%02x", hndl);
    return;
  }
  memset(msg, 0, sizeof(*msg));
  msg->event = AP_STOP_EVT;
  msg->layer_specific = hndl;
  msg->suspend = true;
  msg->flush = true;
  bta_sys_sendmsg(msg);
}

// BTA_AV_AP_START_EVT: the enumerator after API_CLOSE_EVT. Posted with
// layer_specific set it starts one stream (bta_av_do_start sends AVDT START).
static constexpr uint16_t AP_START_EVT = (18u << 8) + 11u;

// The second peer asked to play while the first played, and we suspended it.
// A phone that was suspended by its sink often waits for a new START or an
// AVRCP PLAY, which the user's pause/play does not always produce. When the
// first peer stops, start the stream ourselves.
static bool second_wants_ = false;
static uint32_t resume_req_ms_ = 0;
static constexpr uint32_t RESUME_RETRY_MS = 5000;

static void resume_second_() {
  if (!second_wants_ || !second_open_ || second_hndl_ == 0 || owner_ != Owner::NONE || suspend_pending_second_)
    return;
  const uint32_t now = now_ms_();
  if (resume_req_ms_ != 0 && now - resume_req_ms_ < RESUME_RETRY_MS)
    return;
  resume_req_ms_ = now;
  auto *msg = static_cast<StopMsg *>(osi_malloc_func(sizeof(StopMsg)));
  if (msg == nullptr)
    return;
  memset(msg, 0, sizeof(*msg));
  msg->event = AP_START_EVT;
  msg->layer_specific = second_hndl_;
  ESP_LOGI(TAG, "first peer stopped; starting the second peer that wanted to play");
  bta_sys_sendmsg(msg);
}

// The stock decoder is configured by MEDIA_CFG. Send the new owner's settings
// when ownership moves.
static void ensure_decoder_(Owner who) {
  if (decoder_for_ == who || stock_data_cb_ == nullptr)
    return;
  uint8_t *cfg = (who == Owner::SECOND) ? second_cfg_ : primary_cfg_;
  if (cfg == nullptr)
    return;
  stock_data_cb_(EVT_MEDIA_CFG, cfg);
  decoder_for_ = who;
}

// Tell the stock state machine a stream stopped or started, without touching
// the phone's link.
static void stop_stock_(uint8_t hndl) {
  StockEvent evt{};
  evt.suspend = SuspendEvt{CHNL_AUDIO, hndl, true, 0};
  real_cb_(EVT_SUSPEND, &evt);
}

static void start_stock_(uint8_t hndl) {
  StockEvent evt{};
  evt.start = StartEvt{CHNL_AUDIO, hndl, 0, true, false};
  real_cb_(EVT_START, &evt);
}

// Give the sink to `who`. The stock player is idle or paused at this point.
static void claim_(Owner who) {
  ensure_decoder_(who);
  owner_ = who;
  last_data_ms_ = now_ms_();
  start_stock_(hndl_of_(who));
}

// Move the sink from the silent owner to `who`.
static void take_over_(Owner who) {
  const Owner prev = owner_;
  owner_ = Owner::NONE;
  stop_stock_(hndl_of_(prev));
  const uint8_t prev_hndl = hndl_of_(prev);
  if (!(prev == Owner::SECOND ? suspend_pending_second_ : suspend_pending_first_))
    suspend_stream_(prev_hndl);
  claim_(who);
}

static bool owner_silent_() { return now_ms_() - last_data_ms_ > TAKEOVER_SILENCE_MS; }

// True when the first SBC frame of the packet is digital silence. A paused
// phone keeps sending frames whose scale factors are not all zero, so the
// whole frame body is checked: every byte after the 4-byte header must be
// zero. Anything unexpected counts as audible.
// Layout: BT_HDR (8 bytes), one media header byte, then the SBC frames.
static bool sbc_silent_(const void *data) {
  const auto *hdr = static_cast<const uint8_t *>(data);
  const uint16_t len = static_cast<uint16_t>(hdr[2] | (hdr[3] << 8));
  const uint16_t offset = static_cast<uint16_t>(hdr[4] | (hdr[5] << 8));
  const uint8_t *payload = hdr + 8 + offset;
  if (len < 6 || (payload[0] & 0x0f) == 0)
    return false;
  const uint8_t *frame = payload + 1;
  if (frame[0] != 0x9C)
    return false;
  // Frame length from the header: blocks, channel mode, subbands, bitpool.
  const uint8_t blocks = static_cast<uint8_t>(4u << ((frame[1] >> 4) & 3));
  const uint8_t mode = (frame[1] >> 2) & 3;  // 0 mono, 1 dual, 2 stereo, 3 joint
  const uint8_t subbands = (frame[1] & 1) ? 8 : 4;
  const uint8_t channels = mode == 0 ? 1 : 2;
  const uint8_t bitpool = frame[2];
  uint16_t bits = static_cast<uint16_t>(32 + 4 * subbands * channels);
  if (mode == 0 || mode == 1)
    bits += static_cast<uint16_t>(blocks * channels * bitpool);
  else if (mode == 2)
    bits += static_cast<uint16_t>(blocks * bitpool);
  else
    bits += static_cast<uint16_t>(subbands + blocks * bitpool);
  const uint16_t frame_len = (bits + 7) / 8;
  if (frame_len < 5 || frame_len > static_cast<uint16_t>(len - 1))
    return false;
  for (uint16_t i = 4; i < frame_len; i++) {
    if (frame[i] != 0)
      return false;
  }
  return true;
}

// The stock power manager asks for sniff once per idle period. A phone may
// refuse (HCI status 0x24) and then stays active for many seconds, which takes
// airtime from the playing stream. Ask again every 2 s while the second link is
// active and the first peer plays. Runs in the BTU task with the audio packets.
static constexpr uint32_t SNIFF_RETRY_MS = 2000;
static uint32_t sniff_try_ms_ = 0;

static void retry_second_sniff_() {
  if (!second_open_ || !have_second_addr_ || owner_ != Owner::PRIMARY)
    return;
  const uint32_t now = now_ms_();
  if (now - sniff_try_ms_ < SNIFF_RETRY_MS)
    return;
  sniff_try_ms_ = now;
  uint8_t mode = 0;
  if (BTM_ReadPowerMode(second_addr_, &mode) != 0 || mode != 0)
    return;
  PwrMode sniff{800, 400, 4, 1, 2};
  __real_BTM_SetPowerMode(0x80, second_addr_, &sniff);
}

// Only audible data keeps an owner alive or claims the sink. Silent frames of
// the owner still go to the stock player.
static void on_data_(Owner who, void *data) {
  retry_second_sniff_();
  const uint32_t suspended_at = (who == Owner::SECOND) ? second_suspended_ : first_suspended_;
  const bool suspended = suspended_at != 0 && now_ms_() - suspended_at < TRAILING_AUDIO_MS;
  const bool audible = !sbc_silent_(data);
  const char *name = who == Owner::SECOND ? "second" : "first";
  bool pass = false;
  if (owner_ == who) {
    if (audible)
      last_data_ms_ = now_ms_();
    pass = true;
  } else if (suspended || !audible) {
    // Trailing or silent audio never claims the sink.
  } else if (owner_ == Owner::NONE) {
    ESP_LOGI(TAG, "%s peer audio resumed", name);
    claim_(who);
    pass = true;
  } else if (owner_silent_()) {
    ESP_LOGI(TAG, "%s peer audio replaces the silent %s peer", name, who == Owner::SECOND ? "first" : "second");
    take_over_(who);
    pass = true;
  } else {
    uint32_t &req = suspend_req_ms_[who == Owner::SECOND ? 1 : 0];
    if (req == 0 || now_ms_() - req > RESUSPEND_MS) {
      ESP_LOGI(TAG, "%s peer sends audio while the other plays; suspending it", name);
      suspend_stream_(hndl_of_(who));
      if (who == Owner::SECOND)
        second_wants_ = true;
    }
  }
  if (pass && stock_data_cb_ != nullptr)
    stock_data_cb_(EVT_MEDIA_DATA, data);
  else if (data != nullptr)
    osi_free_func(data);
}

static volatile uint32_t last_stall_ms_ = 0;

// Used by the reconnect decision in a2dp.cpp.
extern "C" bool a2dp_second_recent_stall(uint32_t window_ms) {
  const uint32_t at = last_stall_ms_;
  return at != 0 && now_ms_() - at < window_ms;
}

static void stall_check_(void *) {
  const Owner who = owner_;
  if (who == Owner::NONE || now_ms_() - last_data_ms_ < STALL_MS)
    return;
  last_stall_ms_ = now_ms_();
  ESP_LOGW(TAG, "%s peer sent no audible audio for %u ms; reporting it as paused",
           who == Owner::SECOND ? "second" : "first", (unsigned) STALL_MS);
  owner_ = Owner::NONE;
  stop_stock_(hndl_of_(who));
  if (who == Owner::PRIMARY)
    resume_second_();
}

static void first_media_(uint8_t event, void *data) {
  if (event == EVT_MEDIA_CFG) {
    primary_cfg_ = static_cast<uint8_t *>(data);
    if (owner_ != Owner::SECOND && stock_data_cb_ != nullptr) {
      stock_data_cb_(event, data);
      decoder_for_ = Owner::PRIMARY;
    }
    return;
  }
  if (event == EVT_MEDIA_DATA) {
    on_data_(Owner::PRIMARY, data);
    return;
  }
  if (stock_data_cb_ != nullptr)
    stock_data_cb_(event, data);
}

static void second_media_(uint8_t event, void *data) {
  if (event == EVT_MEDIA_CFG) {
    second_cfg_ = static_cast<uint8_t *>(data);
    ESP_LOGI(TAG, "second stream configured");
    if (owner_ == Owner::SECOND)
      ensure_decoder_(Owner::SECOND);
    return;
  }
  if (event == EVT_MEDIA_DATA)
    on_data_(Owner::SECOND, data);
}

static void register_second_() {
  if (second_registered_ || !have_reg_)
    return;
  second_registered_ = true;
  ESP_LOGI(TAG, "registering second A2DP stream");
  __real_BTA_AvRegister(saved_chnl_, "A2DP2", APP_ID_SECOND, second_media_, &our_cos_, saved_rc_, saved_tsep_);
}

static bool hndl_event_(uint8_t event) {
  return event == EVT_OPEN || event == EVT_CLOSE || event == EVT_START || event == EVT_STOP ||
         event == EVT_PROTECT_REQ || event == EVT_PROTECT_RSP || event == EVT_RECONFIG || event == EVT_SUSPEND;
}

static bool rc_event_(uint8_t event) {
  return event == EVT_RC_OPEN || event == EVT_RC_CLOSE || event == EVT_REMOTE_CMD || event == EVT_REMOTE_RSP ||
         event == EVT_VENDOR_CMD || event == EVT_VENDOR_RSP || event == EVT_META || event == EVT_RC_FEAT;
}

// tBTA_AV_START: chnl, hndl, status, initiator, suspending.
static bool real_start_(const uint8_t *bytes) { return bytes[2] == 0 && bytes[4] == 0; }

static void on_second_event_(uint8_t event, uint8_t *bytes, void *data) {
  if (event == EVT_OPEN && bytes[8] != 0) {
    ESP_LOGI(TAG, "second stream open failed, status %u", bytes[8]);
  } else if (event == EVT_OPEN) {
    remember_second_addr_(bytes + 2);
    second_open_ = true;
    second_suspended_ = 0;
    log_addr_("second peer connected", second_addr_);
    if (!have_primary_)
      ESP_LOGW(TAG, "second stream took this peer while no first peer is open");
    bta_sys_set_policy(BTA_ID_AV, HCI_ENABLE_SNIFF_MODE, second_addr_);
  } else if (event == EVT_CLOSE) {
    ESP_LOGI(TAG, "second peer disconnected");
    second_open_ = false;
    second_wants_ = false;
    if (!have_primary_) {
      // The stock state machine still holds this peer as its only link.
      owner_ = Owner::NONE;
      real_cb_(event, data);
    } else if (owner_ == Owner::SECOND) {
      // The first link stays; the stream stopped without closing the stock link.
      owner_ = Owner::NONE;
      stop_stock_(second_hndl_);
    }
    have_second_addr_ = false;
    second_rc_ = RC_NONE;
    second_cfg_ = nullptr;
    if (decoder_for_ == Owner::SECOND)
      decoder_for_ = Owner::NONE;
  } else if (event == EVT_START) {
    if (!real_start_(bytes)) {
      ESP_LOGI(TAG, "second stream START ignored: status %u, suspending %u, initiator %u", bytes[2], bytes[4],
               bytes[3]);
      return;
    }
    second_suspended_ = 0;
    if (owner_ != Owner::PRIMARY)
      second_wants_ = false;
    if (owner_ == Owner::PRIMARY) {
      if (owner_silent_()) {
        ESP_LOGI(TAG, "second peer plays now; the first peer is silent");
        take_over_(Owner::SECOND);
      } else {
        ESP_LOGI(TAG, "second peer started while the first plays (last audible audio %u ms ago); suspending it",
                 (unsigned) (now_ms_() - last_data_ms_));
        suspend_stream_(second_hndl_);
        second_wants_ = true;
      }
    } else if (owner_ == Owner::NONE) {
      ensure_decoder_(Owner::SECOND);
      owner_ = Owner::SECOND;
      last_data_ms_ = now_ms_();
      ESP_LOGI(TAG, "second peer plays now");
      real_cb_(event, data);
    }
  } else if (event == EVT_SUSPEND || event == EVT_STOP) {
    second_suspended_ = now_ms_();
    if (event == EVT_SUSPEND) {
      // tBTA_AV_SUSPEND: chnl, hndl, initiator, status.
      ESP_LOGI(TAG, "second stream suspend result: status %u, we asked %u", bytes[3], bytes[2]);
      if (bytes[2] == 0)
        second_wants_ = false;
      suspend_pending_second_ = false;
    }
    if (owner_ == Owner::SECOND) {
      owner_ = Owner::NONE;
      ESP_LOGI(TAG, "second peer stopped");
      real_cb_(event, data);
    }
  }
}

// Events of the first stream. Returns true when the stock callback should not
// see the event.
static bool on_first_event_(uint8_t event, uint8_t *bytes) {
  if (event == EVT_OPEN) {
    if (bytes[8] == 0) {
      memcpy(primary_addr_, bytes + 2, 6);
      have_primary_ = true;
      log_addr_("first peer connected", primary_addr_);
      register_second_();
    } else {
      ESP_LOGI(TAG, "first stream open failed, status %u", bytes[8]);
    }
  } else if (event == EVT_CLOSE) {
    if (have_primary_)
      ESP_LOGI(TAG, "first peer disconnected");
    have_primary_ = false;
    const bool was_playing = owner_ == Owner::PRIMARY;
    if (was_playing)
      owner_ = Owner::NONE;
    if (decoder_for_ == Owner::PRIMARY)
      decoder_for_ = Owner::NONE;
    primary_cfg_ = nullptr;
    first_suspended_ = 0;
    suspend_pending_first_ = false;
    if (second_open_) {
      // Keep the stock link: it now serves the second peer.
      if (was_playing)
        stop_stock_(bytes[1]);
      ESP_LOGI(TAG, "first peer left; the second peer keeps the player");
      return true;
    }
  } else if (event == EVT_START) {
    if (!real_start_(bytes))
      return false;
    first_suspended_ = 0;
    if (owner_ == Owner::SECOND) {
      if (owner_silent_()) {
        ESP_LOGI(TAG, "first peer plays now; the second peer is silent");
        take_over_(Owner::PRIMARY);
      } else {
        ESP_LOGI(TAG, "first peer started while the second plays (last audible audio %u ms ago); suspending it",
                 (unsigned) (now_ms_() - last_data_ms_));
        suspend_stream_(bytes[1]);
      }
      return true;
    }
    ensure_decoder_(Owner::PRIMARY);
    owner_ = Owner::PRIMARY;
    last_data_ms_ = now_ms_();
  } else if (event == EVT_SUSPEND || event == EVT_STOP) {
    first_suspended_ = now_ms_();
    if (event == EVT_SUSPEND) {
      if (suspend_pending_first_)
        ESP_LOGI(TAG, "first stream suspend result: status %u", bytes[3]);
      suspend_pending_first_ = false;
    }
    if (owner_ == Owner::SECOND)
      return true;
    if (owner_ == Owner::PRIMARY) {
      owner_ = Owner::NONE;
      // The stock machine sees this SUSPEND first; the START request is
      // handled after it in the BTU queue.
      resume_second_();
    }
  }
  return false;
}

static void demux_(uint8_t event, void *data) {
  auto *bytes = static_cast<uint8_t *>(data);

  if (event == EVT_ENABLE) {
    second_hndl_ = 0;
    primary_hndl_ = 0;
    have_second_addr_ = false;
    second_rc_ = RC_NONE;
    have_primary_ = false;
    second_registered_ = false;
    owner_ = Owner::NONE;
    decoder_for_ = Owner::NONE;
    primary_cfg_ = nullptr;
    second_cfg_ = nullptr;
    suspend_pending_first_ = false;
    suspend_pending_second_ = false;
    first_suspended_ = 0;
    second_suspended_ = 0;
    second_open_ = false;
    if (stall_timer_ == nullptr) {
      const esp_timer_create_args_t args = {.callback = stall_check_, .name = "a2dp_stall"};
      if (esp_timer_create(&args, &stall_timer_) == ESP_OK)
        esp_timer_start_periodic(stall_timer_, 500 * 1000);
    }
  }

  if (event == EVT_REGISTER && bytes != nullptr) {
    if (bytes[2] == APP_ID_SECOND) {
      second_hndl_ = bytes[1];
      ESP_LOGI(TAG, "second stream handle 0x%02x", second_hndl_);
      return;
    }
    primary_hndl_ = bytes[1];
  }

  if (bytes != nullptr && second_hndl_ != 0 && hndl_event_(event) && bytes[1] == second_hndl_) {
    on_second_event_(event, bytes, data);
    return;
  }

  if (bytes != nullptr && (event == EVT_PENDING || event == EVT_INCOMING)) {
    if (is_second_addr_(bytes) || (have_primary_ && !same_addr_(bytes, primary_addr_))) {
      if (!is_second_addr_(bytes))
        remember_second_addr_(bytes);
      return;
    }
  }

  if (event == EVT_REJECT && bytes != nullptr &&
      ((second_hndl_ != 0 && bytes[6] == second_hndl_) || is_second_addr_(bytes)))
    return;

  if (bytes != nullptr && rc_event_(event)) {
    const uint8_t rc = bytes[0];
    if (event == EVT_RC_OPEN) {
      const uint8_t *addr = bytes + 8;
      if (is_second_addr_(addr) || (have_primary_ && !same_addr_(addr, primary_addr_))) {
        second_rc_ = rc;
        if (!is_second_addr_(addr))
          remember_second_addr_(addr);
        log_addr_("second peer AVRCP ignored", addr);
        return;
      }
    } else if (rc == second_rc_) {
      if (event == EVT_RC_CLOSE)
        second_rc_ = RC_NONE;
      return;
    }
  }

  if (bytes != nullptr && hndl_event_(event) && on_first_event_(event, bytes))
    return;

  if (real_cb_ != nullptr)
    real_cb_(event, data);
}

// bta_av_suspend_cfm closes the stream when a suspend fails or times out. For a
// suspend we asked for, keep the link up: the phone keeps its connection and we
// keep discarding its audio.
extern "C" void __wrap_bta_av_ssm_execute(void *scb, uint16_t event, void *data) {
  if (event == API_CLOSE_EVT && data == nullptr) {
    const bool second = suspend_pending_second_ && second_hndl_ != 0 && scb == bta_av_hndl_to_scb(second_hndl_);
    const bool first = suspend_pending_first_ && primary_hndl_ != 0 && scb == bta_av_hndl_to_scb(primary_hndl_);
    if (second || first) {
      ESP_LOGW(TAG, "%s stream did not confirm our suspend; keeping the link", second ? "second" : "first");
      return;
    }
  }
  __real_bta_av_ssm_execute(scb, event, data);
}

extern "C" void __wrap_BTA_AvEnable(uint16_t sec_mask, uint16_t features, void (*cb)(uint8_t, void *)) {
  real_cb_ = cb;
  __real_BTA_AvEnable(sec_mask, features, demux_);
}

extern "C" void __wrap_BTA_AvRegister(uint8_t chnl, const char *name, uint8_t app_id, void (*data_cb)(uint8_t, void *),
                                      const void *cos, const void *rc_cos, uint8_t tsep) {
  saved_chnl_ = chnl;
  saved_tsep_ = tsep;
  saved_rc_ = rc_cos;
  have_reg_ = true;
  // The stock callout's init() accepts every endpoint, so one register would
  // take both. Ours stops after the first.
  const void *use = (cos == static_cast<const void *>(&bta_av_a2d_cos)) ? static_cast<const void *>(&our_cos_) : cos;
  // The first stream's audio goes through first_media_ so we can hold it back
  // while the second peer owns the sink.
  stock_data_cb_ = data_cb;
  __real_BTA_AvRegister(chnl, name, app_id, first_media_, use, rc_cos, tsep);
}

// The stock power manager asks for a short sniff interval (30-54 slots) once a
// stream is idle. On the second link that interval costs the playing stream
// airtime, since we are slave on the first link and master on the second.
// Use a long interval for the second link; a START wakes it up anyway.
static constexpr uint8_t BTM_PM_MD_SNIFF = 2;
static constexpr uint16_t SECOND_SNIFF_MAX = 800;
static constexpr uint16_t SECOND_SNIFF_MIN = 400;

extern "C" uint8_t __wrap_BTM_SetPowerMode(uint8_t pm_id, uint8_t *addr, PwrMode *mode) {
  if (mode != nullptr && addr != nullptr && second_open_ && is_second_addr_(addr) &&
      (mode->mode & 0x0f) == BTM_PM_MD_SNIFF && mode->max < SECOND_SNIFF_MAX) {
    PwrMode longer = *mode;
    longer.max = SECOND_SNIFF_MAX;
    longer.min = SECOND_SNIFF_MIN;
    return __real_BTM_SetPowerMode(pm_id, addr, &longer);
  }
  return __real_BTM_SetPowerMode(pm_id, addr, mode);
}

#endif
