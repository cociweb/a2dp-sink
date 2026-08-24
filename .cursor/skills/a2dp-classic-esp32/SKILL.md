---
name: a2dp-classic-esp32
description: >-
  Guides ESPHome A2DP sink work on original ESP32 (Bluetooth Classic): I2S DMA
  vs BT heap, Sendspin source switching, Bluedroid disable, Wi-Fi coexistence,
  and log triage. Use when changing a2dp, a2dp_sink, a2dp_avrcp, sharing one
  I2S speaker with Sendspin/HTTP, I2S DMA allocation failures, A2DP
  start-then-stop, disconnect/disable crashes, or comparing disconnect-fix to
  main.
---

# A2DP Classic ESP32

## Before changing memory, I2S, or the orchestrator

Diff against `origin/main` first. `main` already played Sendspin until A2DP took over. Most “I2S is dead” bugs on `disconnect-fix` were **BT heap in internal SRAM**, not a broken mixer.

Keep new work in **this repo**. HA devices load `github://cociweb/a2dp-sink@…`. Edits in the ESPHome fork (`speaker_source`, Sendspin) do not reach those builds.

Do not change mixer/resampler `task_stack_in_psram` unless the user asks.

## Architecture (one speaker, many sources)

```
phone  --A2DP/SBC-->  a2dp hub (PCM ring) --> a2dp_sink media_source
sendspin/HTTP ----------------------------------> speaker_source
                                                    |
                                              mixer/resampler
                                                    |
                                              i2s_audio (DMA in internal SRAM)
```

`speaker_source` stops the **source**, then `speaker->stop()`, waits `is_stopped()`, then `play_uri`. Stopping I2S **frees DMA**. Restart must realloc from **internal** heap. Classic BT already running is the usual thief of that heap.

Sendspin `STOP` sets client state `EXTERNAL_SOURCE` and does **not** go IDLE until the stream ends. `SpeakerSourceMediaPlayer::try_execute_play_uri_` waits for IDLE; historically that wait completed because `speaker->stop()` failed Sendspin writes. Do not “fix” that by forking ESPHome — fix BT SRAM use.

## Do / do not

| Do | Do not |
|---|---|
| Default `bt_allocation_in_psram: true`; do not overwrite YAML `sdkconfig_options` | Force `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=n` to “save PSRAM bus” |
| Ignore `AUDIO_STARTED` when media source is IDLE | `play_uri("a2dp://stream")` / `request_play_uri_` from that callback |
| `disable()`: disconnect, wait for ACL, then `deinit_bt_()` | `deinit_bt_()` immediately in `disable()` |
| Clear `EVT_CMD_START` when starting drain | Leave START set so drain cancels every loop |
| Treat mixer warm-up zero-writes as start-up (`ZERO_WRITE_STARTUP_STOP_COUNT`) | Treat first 3×100 ms of 0-byte writes as a dead speaker |
| Keep reader chunk at 2048; stack in **internal** RAM if A2DP stutters with `use_psram: true` | Put PCM ring **and** BT heap **and** reader stack in PSRAM at once |
| Enable `diagnostics: true` / media_source `debug_logging: true` for one device | Raise log level globally and drown the serial |

## Keep vs drop (`disconnect-fix` vs `main`)

**Keep:** `a2dp.disconnect`; deferred `disable()` teardown; drain START-bit clear; ESPHome 2026.8 coexistence / `request_bluetooth` / IDF 5.1 sdkconfig names; original-ESP32-only check; auto-reconnect + AVRCP resume; preroll; reader prio 10; zero-write start-up grace; `diagnostics` default off.

**Drop / never reintroduce:** BT heap default following `use_psram` in a way that can be false; IDLE `AUDIO_STARTED` auto-play; vendoring `speaker_source`; Sendspin `STOP` → IDLE as an I2S workaround.

## Debug order

1. Confirm variant is original ESP32, not S3.
2. At `A2DP audio started`, read `heap_internal`. If it is ~20 KB or falling, DMA restart will fail. PSRAM free MB does not matter.
3. Map logs with [debugging.md](debugging.md).
4. If Sendspin dies **when A2DP starts** and I2S logs `allocate DMA buffer failed`, check `bt_allocation_in_psram` / `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST` before touching mixer or `speaker_source`.
5. If A2DP plays 1–2 s then goes silent with no I2S error, look at zero-write suspend and `AUDIO_STARTED` while IDLE.
6. If disable/reboot shows `bta_dm_disable` / `IllegalInstruction` in the BT task, teardown raced ACL.

HA addon YAML under Docker/CIFS is often not writable from this environment. Copy keys into the addon configs yourself, or compile from a writable checkout.
