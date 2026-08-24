# A2DP log triage

Timestamps in user reports are local. Component tags below are what to grep.

## Symptom → likely cause

| Log / behaviour | Cause | Fix direction |
|---|---|---|
| `i2s_common: i2s_alloc_dma_desc: allocate DMA buffer failed` / `i2s_audio.speaker: Failed to initialize I2S channel` / `Driver failed to start; retrying in 1 second` | I2S DMA needs **internal** SRAM. Classic BT heap is in internal SRAM, or speaker was stopped and cannot realloc. | `bt_allocation_in_psram: true` and YAML `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST: y`. Do not overlay `speaker_source`. |
| `sendspin.player: Failed to send audio chunk` right after I2S errors | Sendspin still PLAYING; I2S already torn down. Secondary. | Fix I2S/DMA; chunks recover when the speaker stays up. |
| I2S fails **before** `A2DP audio started` | YAML `bluetooth_connected` → `play_media a2dp://stream` stopped the speaker at **connect**, not at SBC start. | Same DMA/heap issue; switching earlier makes it obvious. |
| `A2DP audio started` then 1–2 s silence, then maybe reboot | Reader counted mixer warm-up `write_output()==0` as dead downstream and called `request_audio_suspend()`. Later Play ignored because source is IDLE. | Keep `ZERO_WRITE_STARTUP_STOP_COUNT`; do **not** auto `play_uri` on `AUDIO_STARTED`+IDLE. |
| `IllegalInstruction` / `__assert_func` / `bta_dm_disable_search_and_disc` | `disable()` deinit while ACL/discovery still live. | Disconnect, finish in `DISCONNECTED`; on timeout leave stack up. |
| `BT_APPL: Pkt dropped` / sequence errors + underruns | Wi-Fi modem-sleep starving the shared radio. | Coexistence `prefer_bt_while_streaming` (disables Wi-Fi PS while streaming). |
| Healthy `rx`, `loop_rate` << ~84/s at 44.1 kHz stereo, ring overflow, `dropped=0` | PSRAM bus contention: ring + BT heap + reader stack all in PSRAM. | Reader `task_stack_in_psram: false`. Keep BT heap in PSRAM. |
| Drain never ends, underrun counter spins | `EVT_CMD_START` still set during drain. | Clear START when setting DRAIN. |
| `Allocate tx dma channel failed` after reboot, `heap_internal` ~18–20 KB | Previous session leaked or BT+pipeline left internal heap too small for I2S re-init. | Same as DMA row; confirm BT SPIRAM_FIRST survived sdkconfig merge. |

## Heap line

`A2DP audio started (heap_internal=… B psram=… B)` — added on `disconnect-fix`. Internal heap must stay large enough for I2S DMA (~20 KB+ descriptors + buffers; 100 ms 44.1 kHz 16-bit stereo is ~17 KB of samples plus DMA structs).

## Reader diagnostics

With media_source `debug_logging: true`: `loop_rate`, `underruns`, `partial_writes`, `written`, `min_stack_free`. Hub `diagnostics: true`: ring fill, high-water, rx/dropped, heaps. Both are low-frequency; leave off in production YAML.

## YAML that must stay aligned

- `a2dp.use_psram: true` (PCM ring) **and** `bt_allocation_in_psram: true` (or omit; default true).
- Do not set `bt_allocation_in_psram: false` on original ESP32 with a live I2S speaker.
- `pause_wifi_sources_on_connect: false` means Sendspin is not paused by the hub; the speaker_source `play_media` still stops the Sendspin **source** on BT connect if YAML does that.
- External component list for HA: `[a2dp, a2dp_sink, a2dp_avrcp]` only — no `speaker_source` overlay.
