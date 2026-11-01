# Mosaico Mic

ESP-Mosaico / ESP32-S31 wireless microphone prototype for a Mac, using Classic
Bluetooth HFP Hands-Free (HF) and the board's ES8311 microphone. Independent app;
Snake source remains in the sibling project. Device Bluetooth name: **Mosaico Mic**.

## Microphone and touch controls

- ES8311 microphone -> I2S -> bounded PCM FIFO -> HFP HF -> Mac system input.
- mSBC uses 16 kHz mono signed 16-bit PCM; CVSD falls back to 8 kHz.
- No speaker playback. Select Mac speakers or another output for listening.
- No game, simulator, Wi-Fi receiver or desktop audio driver is needed.
- Touchscreen: hold-to-talk, Enter, Clear All, and a gear opening an ANSI keyboard setup page.
- Clear All sends Command+A then Backspace to the current Mac focus. Keep the
  cursor in the intended text field; this does not inspect or target an app.
- Physical AI button: hold for Fn, double-click for Enter. Short haptic feedback.
- Fn requires this board's F18-to-Fn Mac mapping; direct HID Fn is not assumed.
- USB ESP-Iris remains the update link; retained Vibe Mode/partitions are preserved.
- Pairing uses SSP Just Works and remains discoverable while idle. Pair only in a
  trusted environment during prototype tests. No pairing record is auto-erased.

## Build and install

Use the workspace-pinned ESP-IDF commit
`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` (6.2.0), target `esp32s31`.
From the workspace root, use its `idf-low-noise-build` runner to build this project,
then `python mosaico.py iris system-update --project projects/wireless_mic` with
an explicitly selected device. All board operations go through `mosaico.py`.
Do not use direct flash commands or erase the board.

The pinned IDF internal HFP codec uses legacy PCM callbacks. This is an explicit
prototype compatibility choice; an SDK upgrade must review its replacement API.
No third-party SBC codec is added. Dependencies come from the existing BSP/Iris.

## Mac acceptance

1. Pair **Mosaico Mic** in Bluetooth settings.
2. Choose **Mosaico Mic** under Sound > Input, keeping your normal sound output.
3. Record without a phone call. Confirm real speech and not a loopback/test tone.
4. Inspect firmware logs: `SCO ... codec=mSBC pcm_rate=16000` confirms negotiation;
   a WAV file's sample rate alone does not. CVSD must be reported as fallback.
5. Check microphone peak/clipping/read-error and FIFO underflow/overflow counters.
6. Test 30 minutes, stop/restart capture, application switching, sleep/wake,
   disconnect/reconnect, range and battery operation before claiming stability.

Initial test host: Apple M5 MacBook Pro (Mac17,2), macOS 27.0.1 (26A434).
BSP runtime reports board hardware V1.2; this is firmware-reported identification.

## Ownership

`main` starts retained recovery, microphone capture and HFP. `app_audio` owns codec,
worker and synchronized FIFO. `app_hfp` owns Bluetooth and consumes PCM via public
interfaces. Codec/transport errors are logged; startup failure leaves Recovery
reachable. No automatic NVS erase or hidden call/voice-assistant activation.

Build and test results belong in `docs/validation.md`. Pairing, compilation or
healthy boot alone does not establish usable Mac microphone interoperability.

## Mac helper v0.3.0

`tools/macos/build_settings.sh` builds the native settings application and runs
configuration, key-event and bounded-log tests. The settings window includes an
application version and read-only diagnostics. The default prefix is
Command+Option+Control+L, followed by 100 ms and an Fn hold until release.
macOS Accessibility/Input Monitoring authorization is required; rebuilding the
ad-hoc signed application can require reauthorization. This prototype's HID
mapping reads the paired board serial from a private local file; see
`tools/macos/README.md` for setup. This repository is a workspace application, not a standalone BSP
checkout: place it in `esp-mosaico-vibe/projects/wireless_mic` with the pinned
workspace dependencies present.
