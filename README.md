# ESPHome A2DP Sink and AVRCP Components

A2DP Sink and AVRCP external components for ESPHome.

These components allow an ESP32 running ESPHome to act as a Bluetooth Classic A2DP audio sink, expose the sink as an ESPHome media source, publish Bluetooth connection and metadata sensors, and send AVRCP media control commands back to the source device.

## Usage

This repository is structured as an ESPHome external component repository.

```yaml
external_components:
  - source: github://cociweb/a2dp-sink@main
    components: [a2dp, a2dp_sink, a2dp_avrcp]
```

For local development:

```yaml
external_components:
  - source: /path/to/a2dp
    components: [a2dp, a2dp_sink, a2dp_avrcp]
    refresh: 0s
```

## Requirements

- ESPHome with ESP-IDF framework
- ESP32 with Bluetooth Classic support
- Speaker/media pipeline configured in ESPHome
- PSRAM is recommended for stable audio buffering

The components configure the required ESP-IDF Bluetooth options for A2DP and AVRCP. If BLE components are present in the same YAML, the Bluetooth controller mode is configured for BTDM; otherwise it uses BR/EDR-only mode.

## Components

### `a2dp`

The central Bluetooth Classic A2DP hub. It owns the ESP-IDF Bluetooth stack lifecycle, A2DP sink callbacks, PCM ring buffer, auto reconnect state, and AVRCP metadata dispatch.

```yaml
a2dp:
  id: a2dp_hub
  device_name: My Speaker
  auto_start: true
  auto_reconnect: true
  discoverable_duration: 3min
  ring_buffer_size: 262144
  use_psram: true
```

#### Configuration variables

- **id** (*Required*): Component ID.
- **device_name** (*Optional*, default `ESPHome`): Bluetooth device name advertised to source devices.
- **auto_start** (*Optional*, default `false`): Start Bluetooth automatically during setup.
- **auto_reconnect** (*Optional*, default `false`): Reconnect to the last connected source after boot.
- **discoverable_duration** (*Optional*): Duration for discoverable mode.
- **ring_buffer_size** (*Optional*, default `131072`): PCM ring buffer size in bytes.
- **use_psram** (*Optional*, default `false`): Prefer PSRAM for the PCM ring buffer.
- **preferred_sample_rate** (*Optional*, default `auto`): Preferred SBC sample rate. Supported values are `auto`, `44100`, and `48000`.
- **preferred_bits_per_sample** (*Optional*, default `16`): Output PCM width used by the media source path. Supported values are `16` and `32`.
- **bt_allocation_in_psram** (*Optional*, default `false`): When `false`, Bluetooth stack allocations stay in internal SRAM (`CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=n`). This keeps the realtime A2DP path off the slower PSRAM bus and helps avoid audio stutter. Set to `true` only if internal RAM is exhausted.
- **diagnostics** (*Optional*, default `false`): Emit a low-frequency (~10s) debug log line with ring-buffer fill level, high-water mark, bytes received, dropped/overflow bytes, and free internal/PSRAM heap. Useful for isolating realtime audio problems without per-packet logging.
- **coexistence** (*Optional*): Wi-Fi/Bluetooth coexistence tuning.

#### Automations

```yaml
- a2dp.enable: a2dp_hub
- a2dp.disable: a2dp_hub
- a2dp.restart_discovery: a2dp_hub
```

### `a2dp_sink`

The A2DP sink subcomponent that consumes audio from the `a2dp` hub.

```yaml
a2dp_sink:
  id: a2dp_receiver
  a2dp_id: a2dp_hub
  sample_rate: 44100
```

#### Configuration variables

- **id** (*Required*): Sink ID.
- **a2dp_id** (*Required*): Parent `a2dp` hub ID.
- **sample_rate** (*Optional*, default `44100`): Output sample rate.
- **pcm_drain_throttle** (*Optional*, default `500ms`): Maximum wait while draining PCM data.
- **speaker_output_delay** (*Optional*, default `200ms`): Speaker output startup delay.
- **speaker_pipeline_delay** (*Optional*, default `200ms`): Media pipeline startup delay.

#### Triggers

- **on_audio_start**: Fired when the Bluetooth source starts streaming audio ("A2DP audio started").
- **on_audio_stop**: Fired when the Bluetooth source stops streaming audio ("A2DP audio stopped").

```yaml
a2dp_sink:
  id: a2dp_receiver
  a2dp_id: a2dp_hub
  on_audio_start:
    - switch.turn_on: amplifier_power
  on_audio_stop:
    - switch.turn_off: amplifier_power
```

#### Platforms

The `a2dp_sink` component provides additional ESPHome platforms:

- **binary_sensor.a2dp_sink**: Bluetooth source connected state.
- **text_sensor.a2dp_sink**: Connected Bluetooth source name.
- **media_source.a2dp_sink**: `a2dp://stream` source for `speaker_source`.

Example:

```yaml
media_source:
  - platform: a2dp_sink
    id: a2dp_media_source
    a2dp_sink_id: a2dp_receiver
    task_stack_in_psram: true
    debug_logging: false

binary_sensor:
  - platform: a2dp_sink
    a2dp_sink_id: a2dp_receiver
    name: Bluetooth Connected

text_sensor:
  - platform: a2dp_sink
    a2dp_sink_id: a2dp_receiver
    name: Bluetooth Source Name
```

### `a2dp_avrcp`

Optional AVRCP controller component for media controls and volume change events.

```yaml
a2dp_avrcp:
  id: avrcp_ctrl
  a2dp_id: a2dp_hub
  on_volume_changed:
    - logger.log:
        format: "AVRCP volume: %u/127"
        args: [x]
```

#### Automation actions

```yaml
- a2dp_avrcp.play: avrcp_ctrl
- a2dp_avrcp.pause: avrcp_ctrl
- a2dp_avrcp.play_pause: avrcp_ctrl
- a2dp_avrcp.next: avrcp_ctrl
- a2dp_avrcp.previous: avrcp_ctrl
- a2dp_avrcp.stop: avrcp_ctrl
- a2dp_avrcp.volume_up: avrcp_ctrl
- a2dp_avrcp.volume_down: avrcp_ctrl
```

### `text_sensor.a2dp`

Publishes AVRCP metadata from the active Bluetooth source.

```yaml
text_sensor:
  - platform: a2dp
    id: a2dp_track_title
    name: Track Title
    type: title

  - platform: a2dp
    id: a2dp_track_artist
    name: Track Artist
    type: artist

  - platform: a2dp
    id: a2dp_track_album
    name: Track Album
    type: album
```

Supported types:

- `title`
- `artist`
- `album`
- `album_artist` *(accepted for YAML compatibility, but AVRCP does not expose a standard album artist attribute through this ESP-IDF API)*

## Realtime audio tuning & diagnostics

Bluetooth A2DP is a realtime source: decoded PCM arrives from the phone at a fixed
rate and must reach I2S with bounded latency. A few settings and diagnostics help
keep that path healthy and make stutter observable instead of guesswork:

- **Match the negotiated sample rate.** Most phones negotiate A2DP/SBC at
  `44100 Hz`. Configure `a2dp_sink: sample_rate: 44100` and, ideally, run the whole
  speaker/I2S chain at 44100 Hz (the TAS5805M supports 44.1 kHz) so no resampling
  is needed. If the output must stay at 48000 Hz, keep the resampler stage but be
  aware it adds realtime work. You can also nudge SBC negotiation with
  `a2dp: preferred_sample_rate: 44100`.
- **Keep the realtime path off the PSRAM bus.** The ESP32 has a single, relatively slow
  PSRAM controller shared by both CPU cores. Putting *everything* on the audio hot path in
  PSRAM at once — the PCM ring buffer (`use_psram: true`), the Bluetooth stack
  (`bt_allocation_in_psram: true`) **and** the reader task stack
  (`media_source: … task_stack_in_psram: true`) — makes those consumers fight over that one
  bus. The BT stack's constant PSRAM traffic then stalls the reader task's execution, so its
  loop rate drops below the fixed A2DP arrival rate and the ring buffer overflows **even with
  zero packet loss / zero `dropped` bytes** (the tell-tale is a healthy `rx` but a `loop_rate`
  well under ~84/s at 44100 Hz stereo). Keep only the large PCM ring buffer in PSRAM; leave
  `bt_allocation_in_psram: false` (the default) and set `task_stack_in_psram: false` so the
  realtime reader executes from fast internal SRAM. Only move BT or the task stack into PSRAM
  if internal RAM is genuinely exhausted.
- **Give Bluetooth the radio while streaming.** The ESP32 shares a single 2.4 GHz
  radio between Wi-Fi and Bluetooth. With Wi-Fi power-save (modem sleep) enabled — the
  ESP-IDF default — the radio periodically parks on Wi-Fi and starves the realtime
  A2DP link, which shows up as controller-level packet loss in the log
  (`BT_APPL: Pkt dropped`, `Sequence numbers error`) together with rising `underruns`
  and a ring buffer that never fills. Enable the coexistence block and keep
  `prefer_bt_while_streaming: true` (the default) so the component disables Wi-Fi
  power-save while audio is streaming and restores your configured mode afterwards:

  ```yaml
  a2dp:
    coexistence:
      software_coexistence: true
      prefer_bt_while_streaming: true
  ```
- **Size the ring buffer for latency, not for hiding drops.** A larger buffer only
  masks a pacing problem while adding seconds of latency and a large post-pause
  backlog. Prefer a modest buffer plus the diagnostics below to find the real cause.
- **Enable diagnostics while investigating.** Set `a2dp: diagnostics: true` and
  `media_source: … debug_logging: true`. Roughly every 10 seconds the log then shows:

  - hub: ring-buffer fill %, high-water mark, total bytes received, dropped/overflow
    bytes (audio silently discarded because the buffer was full), free internal heap,
    free PSRAM;
  - reader task: loop rate, underrun count (buffer empty while playing), partial-write
    count, total bytes written to the speaker pipeline, and the minimum task stack
    watermark.

  Rising `dropped` bytes point to the producer overrunning a too-small/too-slow
  consumer; rising `underruns` point to the consumer starving (RF loss, downstream
  backpressure, or resampler/mixer stalls). These counters isolate whether the
  bottleneck is upstream (Bluetooth) or downstream (speaker pipeline).

## Complete example

A complete local-development example is available at:

```text
components/a2dp/yaml/esp32-idf-a2dp-sink.yaml
```

## Repository structure

```text
components/
├── a2dp/
│   ├── __init__.py
│   ├── a2dp.cpp
│   ├── a2dp.h
│   ├── text_sensor/
│   └── yaml/
├── a2dp_sink/
│   ├── __init__.py
│   ├── a2dp_sink.cpp
│   ├── a2dp_sink.h
│   ├── binary_sensor/
│   ├── media_source/
│   └── text_sensor/
└── a2dp_avrcp/
    ├── __init__.py
    └── a2dp_avrcp.h
```

## Notes

- AVRCP `stop` support depends on the source device and media app.
- Track metadata updates depend on AVRCP metadata and notification support from the source.
- `auto_reconnect` stores the last connected Bluetooth address in ESPHome preferences and retries reconnect after boot.

## License

Add the project license file before publishing the repository.
