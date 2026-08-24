# A2DP sink — agent notes

External ESPHome components for **Bluetooth Classic A2DP** on **original ESP32 only** (WROOM/WROVER). ESP32-S3 and other BLE-only chips are rejected at config time.

Devices (Sonocotta) typically share one I2S DAC (`speaker_source` → mixer → resampler → `i2s_audio`) among Sendspin, HTTP, and A2DP. Mixer/resampler YAML (`task_stack_in_psram`) is a user constraint: do not change it unless asked.

## Hard rules (do not regress)

1. **Classic BT heap stays in PSRAM by default.** `bt_allocation_in_psram` defaults to `true` (`CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST`). Original ESP32 I2S DMA **cannot** use PSRAM. Putting the Bluedroid heap in internal SRAM is what makes `i2s_alloc_dma_desc: allocate DMA buffer failed` appear when A2DP starts. Free PSRAM is a red herring.
2. **Do not steal playback from `AUDIO_STARTED` while the media source is IDLE.** `main` ignores that event. YAML `play_media: a2dp://stream` (e.g. `bluetooth_connected`) owns source switching. Auto `play_uri` / `request_play_uri_` races Sendspin and the orchestrator.
3. **Do not overlay or fork `speaker_source` / Sendspin** to “keep I2S running” across source switches. Stock ESPHome stops the speaker on `PLAY_URI`; that works on `main` when rule 1 holds. Patching the pipeline hides the heap bug and will not ship on HA addon builds that only pull this repo.
4. **Never `esp_bluedroid_disable()` / `deinit_bt_()` while ACL or inquiry is still up.** That asserts in `bta_dm_disable_search_and_disc`. `disable()` must disconnect first and finish teardown from `DISCONNECTED` (timeout: leave the stack up, do not deinit).
5. On audio stop, **clear `EVT_CMD_START` before setting `EVT_CMD_DRAIN`**, or the reader never finishes draining.

For symptoms, log signatures, and keep/drop vs `main`, read `.cursor/skills/a2dp-classic-esp32/SKILL.md`.
