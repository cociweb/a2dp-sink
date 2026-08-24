# A2DP log triage

Timestamps in user reports are local. Component tags below are what to grep.

## Symptom → likely cause

| Log / behaviour | Cause | Fix direction |
|---|---|---|
| `i2s_common: i2s_alloc_dma_desc: allocate DMA buffer failed` / `i2s_audio.speaker: Failed to initialize I2S channel` / `Driver failed to start; retrying in 1 second` | I2S DMA needs a **contiguous DMA-capable internal** block. Bluedroid heap in internal SRAM, fragmentation after `speaker->stop()`, or the Classic controller + WiFi eating the DMA pool even with SPIRAM_FIRST. | Keep `bt_allocation_in_psram: true`. Read `dma_largest` from `a2dp` diag. Do **not** overlay `speaker_source`. YAML `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST: y` is necessary, not sufficient. |
| `A2DP audio stopped` then immediately `i2s_alloc_dma_desc` / I2S retry 1s, ACL still up (`sniff` / `hci_status=26`) | Phone paused/stopped SBC. Media source went **IDLE** → stock `speaker_source` `finish()`es I2S. Classic BT still holds internal SRAM so DMA realloc fails. `speaker_source` does **not** finish on **PAUSED**. Sniff `0x1a` is secondary. | On audio stop while `is_connected()`, report **PAUSED** (keep reader in PAUSE wait). IDLE only on ACL drop or explicit STOP. |
| `sendspin.player: Failed to send audio chunk` right after I2S errors | Sendspin still trying to write; I2S never started or already torn down. **Secondary.** Same signature on Sendspin **start** (no A2DP line) is still DMA, not a Sendspin player bug. | Fix I2S/DMA (`dma_largest`). Do not patch Sendspin `STOP`. |
| I2S fails **before** `A2DP audio started` | YAML `bluetooth_connected` → `play_media a2dp://stream` stopped the speaker at **connect**, not at SBC start. | Same DMA/heap issue; switching earlier makes it obvious. |
| `A2DP audio started` then 1–2 s silence, then maybe reboot | Reader counted mixer warm-up `write_output()==0` as dead downstream and called `request_audio_suspend()`. Later Play ignored because source is IDLE. | Keep `ZERO_WRITE_STARTUP_STOP_COUNT`; do **not** auto `play_uri` on `AUDIO_STARTED`+IDLE. |
| `IllegalInstruction` / `__assert_func` / `bta_dm_disable_search_and_disc` | `disable()` deinit while ACL/discovery still live. | Disconnect, finish in `DISCONNECTED`; on timeout leave stack up. |
| `IllegalInstruction` in `esp_vApplicationTickHook` + `__assert_func` + `hci_layer` / `filter_incoming_event` (often `EXCVADDR: 0`) | Abort/assert, **not** a real illegal opcode. TickHook + `esp_restart_noos_dig` are the panic path. Real site is `__assert_func`’s caller. Mixed `btc_a2dp_source_*` + `bta_dm_sdp_result` + `osi_malloc` on a **sink** is typical Xtensa junk after heap smash — IDF compiles A2DP source objects whenever `CONFIG_BT_A2DP_ENABLE` is on; we are not a source. | Same family as DMA-heap starvation. Confirm firmware date vs `disable()` defer + `bt_allocation_in_psram` default. Do not add A2DP source code. |
| `BT_APPL: Pkt dropped` / sequence errors + underruns | Wi-Fi modem-sleep starving the shared radio. | Coexistence `prefer_bt_while_streaming` (disables Wi-Fi PS while streaming). |
| Healthy `rx`, `loop_rate` << ~84/s at 44.1 kHz stereo, ring overflow, `dropped=0` | PSRAM bus contention: ring + BT heap + reader stack all in PSRAM. | Reader `task_stack_in_psram: false`. Keep BT heap in PSRAM. |
| Drain never ends, underrun counter spins | `EVT_CMD_START` still set during drain. | Clear START when setting DRAIN. |
| `[a2dp:…] A2DP audio started` then reboot (`BT_HCI` mode 0, `new conn_srvc id:19`) | `AUDIO_STARTED` issued Wi-Fi PS + AVRCP HCI in the same loop as SBC start → `hci_layer` `filter_incoming_event` abort. Happens with or without Sendspin. | Defer coex 250 ms and AVRCP 1 s after audio start. Do not overlay `speaker_source`. |
| Sendspin plays, web GUI / native API dead, A2DP still discoverable | Classic BT (`auto_start`) + Sendspin Wi-Fi audio share the radio. **Also:** `CONNECTED` used to call `set_coex_preference_(true)` whenever `prefer_bt_while_discoverable` was false (the default), which set Wi-Fi PS to NONE at ACL connect and killed httpd/API before audio. | Prefer BT only from deferred `AUDIO_STARTED`. |

## Heap line

`A2DP audio started (heap_internal=… B dma_largest=… B psram=… B)` and periodic `a2dp: diag: … dma_largest=…`. I2S 100 ms 44.1 kHz 16-bit stereo is ~17 KB of samples plus DMA descriptors — `dma_largest` must stay above that. A large `heap_internal` total with a tiny `dma_largest` means fragmentation.

## Reader diagnostics

With media_source `debug_logging: true`: `loop_rate`, `underruns`, `partial_writes`, `written`, `min_stack_free`. Hub `diagnostics: true`: ring fill, high-water, rx/dropped, heaps, `dma_largest`. Both are low-frequency; leave off in production YAML.

## YAML that must stay aligned

- `a2dp.use_psram: true` (PCM ring) **and** `bt_allocation_in_psram: true` (or omit; default true).
- Do not set `bt_allocation_in_psram: false` on original ESP32 with a live I2S speaker.
- `pause_wifi_sources_on_connect: false` means Sendspin is not paused by the hub; the speaker_source `play_media` still stops the Sendspin **source** on BT connect if YAML does that.
- External component list for HA: `[a2dp, a2dp_sink, a2dp_avrcp]` only — no `speaker_source` overlay.

## Iteration log (append, newest first)

### 2026-08-25 — A2DP stop → IDLE → I2S DMA realloc fail (ACL still up)

- **Logs:** `speaker_source_media_player: State changed to IDLE` → `[a2dp:303] A2DP audio stopped` → `i2s_alloc_dma_desc: allocate DMA buffer failed` → `i2s_audio.speaker: Failed to initialize I2S channel` retry 1s. Then `BT_HCI sniff hdl 0x81`, `hcif mode change … status 0x1a`, `bta_dm_pm_btm_status hci_status=26`.
- **Match:** DMA-fail row, plus IDLE-on-pause. Phone paused/stopped audio; ACL **still connected** (sniff). Stock `speaker_source` only `finish()`es I2S on **IDLE**, not PAUSED.
- **Ruled out:** `speaker_source` overlay; mixer/resampler YAML; Sendspin `STOP`→IDLE; sniff `0x1a` as the I2S cause (HCI_ERR_COMMAND_DISALLOWED on sniff is secondary).
- **Fix (a2dp-sink only):** After drain, if `is_connected()` and not STOP, keep the reader in PAUSE wait and `set_state_(PAUSED)`. IDLE only on ACL drop or explicit STOP. `AUDIO_STARTED` while PAUSED resumes PLAYING (still ignore IDLE).
- **Remaining:** Switching to Sendspin while ACL is still up still STOPs A2DP → IDLE → `finish()`; that realloc can still fail if `dma_largest` is too small. Need a rebuild of this commit to confirm the pause path.

### 2026-08-25 — fix: reboot at `A2DP audio started` (Sendspin OK, API/web dead)

- **User correction:** logs are from current `disconnect-fix`, not the 02:22 binary. `[a2dp:287]` in HA can be a different translation-unit line; do not treat it as “stale firmware”.
- **Logs:** Sendspin plays; web/API dead while A2DP is discoverable; `BT_HCI` mode 0 → `new conn_srvc id:19` → `A2DP audio started` → reboot, **with or without** Sendspin. Previous boot: TickHook abort, `filter_incoming_event`, `EXCVADDR: 0`.
- **Cause in this tree:** `AUDIO_STARTED` immediately called `set_coex_preference_(true)` (`esp_wifi_set_ps`) and `request_avrcp_metadata()` (HCI) in the same loop as SBC leaving sniff. `CONNECTED` also preferred BT (`!prefer_bt_while_discoverable`) and killed Wi-Fi/API before audio.
- **Fix:** Prefer BT only after audio, deferred 250 ms. Defer AVRCP metadata/RN_TRACK_CHANGE 1 s. Do not prefer BT on ACL connect. Still no `speaker_source` overlay.

### 2026-08-25 — Sendspin OK, API/web dead, A2DP start reboots (with or without Sendspin)

- **Logs:** `BT_HCI: hcif mode change: hdl 0x81, mode 0` → `BT_APPL: new conn_srvc id:19, app_id:1` → `[a2dp:287] A2DP audio started` then reboot. Line 287 is the **plain** `ESP_LOGI(..., "A2DP audio started")` in `b7eb709` (compiled 2026-08-24 02:22). Current tree logs heap at ~307.
- **Observation:** Sendspin playback is fine (I2S DMA **is** allocated on this boot). Web GUI and API are not. A2DP is discoverable. Starting A2DP audio reboots **even if Sendspin is not playing**.
- **Ruled out:** Sendspin-specific stop/steal; `speaker_source` overlay; “A2DP only crashes because it tears down Sendspin’s I2S” (reproduces without Sendspin).
- **Conclusion:** Two stacked symptoms, one firmware. (1) API/web death = BT+Wi-Fi radio contention while Sendspin streams. (2) Reboot at SBC start = same AUDIO_STARTED / HCI heap smash as the TickHook crash, on **stale** firmware that lacks deferred `disable()` and `dma_largest` logging.
- **Next:** Flash ≥ `1231c6e`. Fingerprint: new log must include `heap_internal` and `dma_largest`. If it still reboots, capture that line plus the new crash dump. No pipeline changes.

### 2026-08-25 — crash: TickHook IllegalInstruction, HCI `filter_incoming_event`

- **Logs:** Previous-boot crash, core 0, `IllegalInstruction`, PC `esp_vApplicationTickHook`, `EXCVADDR: 0`. Backtrace: TickHook → `esp_restart_noos_dig` → `__assert_func` → `filter_incoming_event`/`hal_says_packet_ready` (`hci_layer.c:456`) → decoded `btc_a2dp_source_prep_2_send` → `bta_dm_sdp_result` → `osi_alarm_is_active` → `osi_malloc_func`.
- **Firmware:** ESPHome 2026.9.0-dev **compiled 2026-08-24 02:22:58**. That is **before** `4981ea5` (defer `disable()` teardown, 23:14) and **before** `30bc669` (BT heap default PSRAM, 23:54). IDF decode used 5.5.5; at that tag line 456 is `stream = packet->data + packet->offset` (NULL `packet->data` → `EXCVADDR 0`), not `bta_dm_disable`.
- **Ruled out:** A2DP **source** role (IDF always links `btc_a2dp_source.c` with `CONFIG_BT_A2DP_ENABLE`; sink-only app). New `speaker_source` overlay. This is **not** the same assert as `bta_dm_disable_search_and_disc`.
- **Conclusion:** Panic-path TickHook + NULL HCI packet / heap smash. Same internal-DMA/BT-heap pressure as the I2S failures. Stale firmware missing last night’s teardown + PSRAM-default commits.
- **Next:** Rebuild/flash `disconnect-fix` at or after `1231c6e`. On the next crash, compare `__assert_func` caller: `bta_dm_disable*` vs `hci_layer`. Need `dma_largest` from a boot that includes that log field. No pipeline or A2DP-source code until then.

### 2026-08-25 — Sendspin does not start, same DMA fail

- **Logs:** `i2s_common: i2s_alloc_dma_desc: allocate DMA buffer failed` → `i2s_std_set_slot` / `i2s_channel_init_std_mode` → `i2s_audio.speaker: Failed to initialize I2S channel` retry 1s → `sendspin.player: Failed to send audio chunk`. Uptime ~535 s. No `A2DP audio started` in the snippet.
- **Already in YAML:** `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST: y`, `a2dp.auto_start: true`, I2S `buffer_duration: 100ms`.
- **Ruled out:** new Sendspin bug; `speaker_source` overlay / Sendspin `STOP`→IDLE (already rejected).
- **Conclusion:** same DMA-internal-heap failure as A2DP-switch. SPIRAM_FIRST in YAML is not enough; I2S still cannot get a contiguous DMA block while Classic BT is up. Chunk warnings are secondary.
- **Next evidence (do not code further architecture until these exist):** `a2dp: diag:` with `dma_largest` from firmware that includes that field; whether I2S ever started this boot; flashed `a2dp-sink` commit. If `dma_largest` << 20 KB, next lever is shrinking I2S DMA need / internal consumers — still not a pipeline fork.

