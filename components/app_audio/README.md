# app_audio

`app_audio` owns microphone capture for the wireless microphone application. It
uses only the BSP ES8311 ADC path; it never creates or opens a speaker device and
has no Bluetooth dependency.

## API contract

- `app_audio_start()` allocates one 320-byte capture buffer and one 1280-byte
  FIFO, then starts the sole codec-owning task. It waits at most eight seconds
  for the first successful microphone read and reports initialization failure or
  timeout to the caller, allowing the composition root to leave Recovery alive.
  A timed-out task remains the sole starting task, retains its fixed-lifetime
  resources, and keeps retrying; a later successful read makes subsequent
  `app_audio_start()` calls return OK.
  All BSP, I2C, I2S, and codec calls occur in that task, never in the caller.
- `app_audio_set_stream_rate()` accepts `0`, `8000`, or `16000`. Other values are
  ignored. Every accepted call atomically clears the FIFO and advances its
  generation, so an in-flight read cannot publish PCM from the previous stream.
- Rate `0` keeps local microphone validation running at 16 kHz but disables FIFO
  publication. Consequently `app_audio_read_pcm()` returns silence until a
  Bluetooth stream rate is selected.
- `app_audio_read_pcm()` is cross-core safe and nonblocking. For a non-NULL
  destination it always initializes and returns the requested byte count;
  unavailable bytes are zero. This lets the HFP clock callback submit a complete
  frame without reusing stale audio. NULL returns zero.
- `app_audio_get_stats()` copies a synchronized lifetime snapshot. `peak` is the
  largest absolute signed 16-bit sample (0..32768); `clipping_samples` counts
  exact `INT16_MIN` or `INT16_MAX` samples; `sample_rate` is the requested stream
  rate, including zero.

The 1280-byte FIFO bounds queued latency to 80 ms at 8 kHz and 40 ms at 16 kHz.
Capture uses 10 ms chunks. The FIFO keeps the newest audio when full and records
discarded bytes as `overflow_bytes`. Silence inserted by reads is recorded as
`underflow_bytes`. `sent_bytes` includes both captured PCM and inserted silence. Codec open/read
failures increment `read_errors`; read failures close the codec before a delayed
reopen, and failure logs are rate-limited. The capture task emits a synchronized
statistics line approximately every five seconds. Hardware failure remains
isolated to this component so the composition root can keep Recovery available.

## Mono channel choice

Capture requests one 16-bit channel with `channel_mask =
ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0)`, the first physical RX slot. This is explicit
rather than relying on a zero mask. The source evidence is the BSP's stereo
Philips I2S setup in
`submodule/esp-mosaico-bsp/components/esp-mosaico-bsp/onboard/audio.c`, the ES8311
standard-I2S two-slot map in `esp_codec_dev/device/es8311/es8311.c`, and the
codec-dev RX contract that `channel_mask` selects physical slots. The BSP's
`voice_wake_cmd` example independently demonstrates mono ES8311 capture at 16 kHz.

## Usage

```c
ESP_ERROR_CHECK_WITHOUT_ABORT(app_audio_start());
app_audio_set_stream_rate(16000);
/* From the incoming HFP clock callback: */
app_audio_read_pcm(outgoing_frame, outgoing_frame_bytes);
```

The component has no stop API: buffers, FIFO, task context, and codec handle have
firmware lifetime. This fixed ownership prevents callback/task use-after-free.

Input gain is fixed at 18 dB for the near-speech prototype (previously 30 dB).
A failed gain write closes the codec and retries; capture must not silently use
an unknown gain. Successful codec opening logs the applied requested gain.
