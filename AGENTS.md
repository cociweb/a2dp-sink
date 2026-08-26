# A2DP sink — agent notes

External ESPHome components for **Bluetooth Classic A2DP** on **original ESP32 only** (WROOM/WROVER). ESP32-S3 and other BLE-only chips are rejected at config time.

Devices (Sonocotta) typically share one I2S DAC (`speaker_source` → mixer → resampler → `i2s_audio`) among Sendspin, HTTP, and A2DP. Mixer/resampler YAML (`task_stack_in_psram`) is a user constraint: do not change it unless asked.

## Close the loop (mandatory)

After **every** new log dump or “same bug again” report:

1. Match logs to `.cursor/skills/a2dp-classic-esp32/debugging.md` **before** writing code.
2. If they match an existing row, follow that row. Do **not** invent a parallel fix (`speaker_source` overlay, Sendspin `STOP`→IDLE, a direct `play_uri()` call from `AUDIO_STARTED`, YAML automations as the fix for component bugs).
3. Append the iteration to `debugging.md` (date, symptom, ruled out, next evidence). Update this file / `SKILL.md` if a rule changed.
4. Only then change code. A diagnosis not written back **will** be repeated.

## Hard rules (do not regress)

1. **Classic BT heap stays in PSRAM by default.** `bt_allocation_in_psram` defaults to `true` (`CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST`). That is **necessary but not sufficient**: I2S DMA needs a **contiguous DMA-capable internal** block (`dma_largest` in the `a2dp` diag / `A2DP audio started` line). Free PSRAM and even a large `heap_internal` total are red herrings if `dma_largest` is too small. Same `i2s_alloc_dma_desc: allocate DMA buffer failed` on **Sendspin start** is still this problem, not a Sendspin bug.
2. **Never call `play_uri()` directly from `AUDIO_STARTED`** — that bypasses the orchestrator's arbitration and can steal Sendspin mid-stop. If `AUDIO_STARTED` fires while IDLE, use `request_play_uri_(A2DP_URI)` instead (dedup with a pending flag) — the same sanctioned "orchestrator, please play me" call `SendspinMediaSource::on_stream_start()` uses for itself. It queues through the normal control path so the orchestrator still stops whatever is active first. Do **not** request play while already `PLAYING`/`PAUSED` — that would make `try_execute_play_uri_` stop-then-restart us and reopen the DMA-realloc bug. The component owns switching here (not YAML): live HA YAML is often read-only from this checkout, so don't push fixes as YAML automations.
3. **Do not overlay or fork `speaker_source` / Sendspin** to “keep I2S running” across source switches. Stock ESPHome stops the speaker on `PLAY_URI`; that works on `main` when rule 1 holds. Patching the pipeline hides the heap bug and will not ship on HA addon builds that only pull this repo.
4. **Never `esp_bluedroid_disable()` / `deinit_bt_()` while ACL or inquiry is still up.** That asserts in `bta_dm_disable_search_and_disc`. `disable()` must disconnect first and finish teardown from `DISCONNECTED` (timeout: leave the stack up, do not deinit). `IllegalInstruction` in `esp_vApplicationTickHook` is the abort/restart stub — the real site is `__assert_func`’s caller (`bta_dm_disable*` vs `hci_layer` are different). Mixed `btc_a2dp_source_*` on a sink is addr2line junk (IDF links source+sink together).
5. On audio stop, **clear `EVT_CMD_START` before setting `EVT_CMD_DRAIN`**, or the reader never finishes draining.
6. On BT audio stop **while ACL is still up**, report **PAUSED**, not IDLE. Stock `speaker_source` `finish()`es I2S only on IDLE; realloc of DMA then fails under Classic BT. IDLE only on ACL drop or explicit STOP. Still ignore `AUDIO_STARTED` while IDLE.
7. **Only auto-reconnect when `disc_rsn == ESP_A2D_DISC_RSN_ABNORMAL` AND audio was actually streaming (`was_streaming`) when the link dropped.** `disc_rsn` alone is **not reliable**: some phones trigger a Bluedroid-internal race when exiting sniff mode (`bta_dm_act no entry for connected service cbs`) even on an explicit, graceful phone-initiated disconnect, and Bluedroid still reports it as `ABNORMAL`. `was_streaming` narrows the auto-reconnect to the one case the feature was built for — a real mid-playback link loss (e.g. WiFi-interference supervision timeout) — and stops fighting an explicit disconnect of an otherwise-idle connection (ACL/AVRCP up, no SBC data flowing) just because Bluedroid mislabels the reason. `start_discovery_()` still runs either way so the device stays passively connectable.

For symptoms, log signatures, and keep/drop vs `main`, read `.cursor/skills/a2dp-classic-esp32/SKILL.md`.
