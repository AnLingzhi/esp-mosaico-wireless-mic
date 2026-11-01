# Hardware validation, 2026-10-31

## Verified

- Fixed ESP-IDF commit: `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`.
- Target: `esp32s31`; BSP reports hardware V1.2.
- Test host: Apple M5 MacBook Pro (Mac17,2), macOS 27.0.1 (26A434).
- Native FIFO unit tests: 1/1 pass (wrap, overflow, underflow, stream generation).
- Project pre-commit checks and compilation-database Cppcheck: pass.
- Final build: `20261031-160017-build-60078`, 1,406,688-byte binary.
- One upstream IDF warning about an untrusted optional CLI extension; no application
  compiler warning. The extension was not enabled.
- Product CLI app-update completed and reported connected/healthy.
- Device ID: `<redacted-device-id>`.
- Boot ID: `<redacted-boot-id>`; application `wireless_mic` 0.1.0,
  normal mode, crash count 0.
- Reported firmware SHA: `<redacted-firmware-sha>`.
- ES8311 opened at 16 kHz, mono, 16-bit, slot 0. Actual sample bytes accumulate
  with zero read errors. HFP profile registers and enables discoverability.
- Mac Bluetooth Settings visibly lists **Mosaico Mic** under nearby devices.

## Findings and limits

The first image had a Classic host but the controller remained BLE-only. It
never completed HFP startup and the installer correctly withheld healthy
acceptance. The completed operation was inspected before updating again.
Corrected both effective and default controller configuration to BR/EDR-only;
added a compile-time guard. Subsequent update and initialization succeeded.

Capture startup contains transient full-scale samples (140 clipped samples on
this successful boot); that count remained unchanged in subsequent observed
samples. This is not a clean-audio or gain-quality pass. Peak statistics are
lifetime maxima, so their 32768 value alone cannot describe current loudness.
Startup transients occur while Bluetooth transmission is still inactive.

Not yet verified: Bluetooth pairing, Mac system-input registration, actual mSBC
negotiation, recorded speech, CVSD fallback, input/output routing, latency,
30-minute stability, application switching, sleep/wake, reconnect, range and
battery life. Discovery and healthy boot do not establish these properties.

## Evidence

Local raw evidence is under `.codex-runs/validation/`: `classic-boot.log`,
`post-install-device.json`, `post-install.log`, `first-update-operation.json`,
`quality.log`, and `cppcheck.log`. Gateway log snapshots may contain previous
boots; use the standalone `classic-boot.log` and current Boot ID for acceptance.
Build logs are under `.codex-runs/idf-low-noise-build/`.

The earlier Snake rollback to touch-only 0.1.0 succeeded before this independent
prototype was installed. Its source and pre-IMU backup remain in `../snake`.

## Mac pairing and first recording

The user authorized pairing and local recording. macOS IOBluetooth pairing of the
verified board Bluetooth address completed with error 0; device reports paired
and connected. System Settings lists Mosaico Mic as the selected Bluetooth input.
CoreAudio independently reports Mosaico Mic as default input, 16000 Hz mono;
default output remains MacBook Pro speakers at 48000 Hz.

Firmware reports `SCO state=3 codec=mSBC pcm_rate=16000`. A named-device AVFoundation
capture (`:Mosaico Mic`) requested 15 seconds and produced 235520 samples / 14.72 s
of mono 16-bit WAV. No phone call or voice-assistant activation was initiated.
The 0.28 s difference between capture timeline and written samples is unclassified;
this first recording is not a timestamp-continuity or zero-loss acceptance pass.

Measured recording: RMS -27.09 dBFS, peak 0 dBFS, 7 clipped samples, 0.20% exactly
zero samples. Audio amplitude increases in the final seconds. No speech transcript
or subjective intelligibility assessment is claimed. Input gain remains 30 dB;
clipping and handling noise need listening and controlled near-speech tests.
The firmware's cumulative underflow counter settled at 6240 bytes near stream
startup and remained unchanged across the observed recording window; overflow=0,
codec read_errors=0. This proves PCM delivery, not radio packet-loss absence.

Evidence: `.codex-runs/validation/mosaico-mic-test-20261031.wav` and `.json`,
`mac-recording.log`, `recording-device.log`, `mac-audio-after-pair.json`.
The local recording is not uploaded. 30-minute endurance, reconnect, sleep/wake,
CVSD fallback, latency, range and battery testing remain open.

## 0.2.1 touch actions and keyboard identification (not installed)

- Main page: warm off-white, blue-gray icons; blue microphone, neutral Enter and
  bottom-left gear, coral Clear. Gear opens click-only Z and slash ANSI setup
  keys with a Back button, not a Shift modifier or an automatic Mac wizard.
- Voice touch holds F18 (requires the device-local Mac Fn mapping). Physical
  hold/double-click behavior remains. Clear emits Command+A, release, Backspace,
  release; it acts on the current focus, with no private app data access.
- Motor feedback: 35 ms at 60 percent strength, once per effective control action.
- Native tests: 2/2 pass (physical gestures and exact key sequences).
- Pre-commit and compilation-database Cppcheck: pass.
- Build: `20261031-172418-build-98217`, 2,053,904-byte application, successful.
- USB discovery returned zero endpoints; the known board is absent/stale.
  This version has NOT been installed. Screen appearance, touch/haptic behavior,
  Mac keyboard identification, Fn mapping, and input-method text delivery remain
  unverified on hardware. The previous screenshot request closed its Iris session;
  the new screenshot path has not been verified and no device image is claimed.
- Evidence: `.codex-runs/validation/touch-actions-build-final.log`,
  `touch-actions-quality-final.log`, `touch-actions-cppcheck.log`,
  `new-ui-discovery-final.json`.

## 0.2.1 installation (2026-10-31)

- After physical AI-button entry, live Vibe Mode identity matched device
  `<redacted-device-id>`, Boot ID `<redacted-boot-id>`.
- `mosaico.py iris app-update --skip-build` installed the existing 2,053,904-byte
  application with ELF SHA-256 validation, reporting success and healthy startup.
  No base-firmware recovery, whole-flash erase, or pairing-data reset was performed.
- Live normal application: `wireless_mic` version `0.2.1`, Boot ID
  `<redacted-boot-id>`, lifecycle state 2, crash count 0. Firmware SHA-256:
  `<redacted-firmware-sha>`.
- Latest startup logs confirm 18 dB microphone gain, successful HID/HFP
  registration, touch-controller initialization, and touch input registration.
- This supersedes the preceding not-installed status. Actual screen appearance,
  touch/haptic actions, Mac keyboard identification, device-local Fn mapping,
  and input-method text delivery still require end-to-end verification.
- Evidence under `.codex-runs/validation/`: `flash021-before.json`,
  `flash021-install.log`, `flash021-after.json`, `flash021-startup.log`.

## 0.2.2 battery indicator (not installed)

- Follow-up layout change: Clear is bottom-left and Setup is bottom-right;
  only their coordinates changed. Logical actions, colors and haptics stay intact.

- Added a main-page top-right battery icon, measured percentage and active-charge
  symbol using the system demo's BQ27220 checked-word read/validation approach.
  Unknown readings display `--%`; 20 percent or less uses muted coral.
- Reads run every five seconds in a dedicated low-priority task, outside the
  display lock. Existing four buttons and keyboard behavior are unchanged.
- Host tests: 3/3 pass, including failed reads, battery absence, invalid SOC,
  zero/negative voltage, charging/discharging and 0..100 percent boundaries.
- Pre-commit checks passed. Build `20261031-175828-build-5516` passed;
  application size 2,074,240 bytes, two build warnings.
- Hardware install could not start: selected device had no available endpoint.
  No flash write was attempted. Actual readings, round-screen appearance and
  audio/touch concurrency remain unverified. No simulator was used, honoring
  the user's actual-board-only requirement.
- Evidence: `.codex-runs/validation/battery-build.log`, `battery-quality.log`,
  `battery-discovery.json`, `battery-before.json`, and `battery-tests/`.

## 0.2.2 installed (2026-10-31)

- Supersedes the offline/not-installed status above. The selected device
  `<redacted-device-id>` completed recovery-first app-update in
  34.9 seconds, with ELF SHA-256 verification and healthy startup.
- Before Boot ID: `<redacted-boot-id>`; after: `<redacted-boot-id>`.
  Live application is `wireless_mic 0.2.2`, normal mode, lifecycle 2, crash count 0.
  SHA-256: `<redacted-firmware-sha>`.
- Latest application startup reports BQ27220 online at 3623 mV, valid SOC 3%,
  and a subsequent active-charging observation. Earlier retained logs include
  a gauge seal failure, but the installed application initialized successfully.
- No whole-flash erase or pairing-data reset. Input gain remains 18 dB and
  HID/HFP registration succeeded. Physical appearance and touch-position
  acceptance remain user-observed; no screenshot acceptance is claimed.
- Evidence: `.codex-runs/validation/flash022-install.log`, `flash022-before.json`,
  `flash022-after.json`, and `flash022-startup.log`.


## Mac prefix executor (2026-10-31, hardware pending)

- Added native configuration UI and actual Mac-side F18/Fn prefix executor; no
  board firmware or partition changes. The verified Mosaico keyboard type (40)
  is required. Synthetic events carry a private user-data tag and are ignored by
  the executor to prevent recursion.
- Enabled current configuration: Command + Option + Shift + L, 215 ms delay,
  then synthesized Fn hold/release. Existing device-specific F18-to-Fn mapping
  remains active.
- Final TCC registration is active for the installed app: Accessibility granted,
  Input Monitoring granted, runtime state `event_tap_active`, process PID 84114.
  Earlier TCC failures were traced to stale code-requirement fingerprints after
  re-signing; do not rebuild/re-sign the app without re-authorizing it.
- The Mosaico Bluetooth keyboard disconnected before a real F18 press. A synthetic
  F18 event reached a listening probe at keyboard type 40, so event routing is
  observed, but actual prefix focus, synthesized Fn hold, and dictation behavior
  remain unverified. No board flash was performed.
- Evidence: `.codex-runs/validation/prefix-chain-events.log`,
  `~/Library/Application Support/MosaicoMic/runtime-status.json`, and
  `prefix-draft.json`.

## Mac v0.3.0 and connection UI (2026-11-01)

- Fixed Fn flagsChanged release handling: clear held state, cancel delayed Fn,
  and release the synthesized modifier. Consecutive holds, early-release
  cancellation, raw F18 release, configuration and bounded-log tests pass.
- Mac v0.3.0 (4) defaults/current user configuration: Command+Option+Control+L,
  100 ms. Added application icon, version label and Chinese runtime log panel.
- Live checks confirmed Accessibility enabled, event tap active, and keyboard
  mapping matched=1/mapping_verified=1. Repeated physical press/release cycles
  were observed in the helper log. The user subsequently confirmed it works.
  This is functional user acceptance, not long-duration stability certification.
- A session event observer distinguishes posting from downstream observation;
  existing evidence does not independently prove target-app focus handling.
- Firmware 0.2.3 includes separate audio/HID status indicators and attempts HFP
  reconnect to the sole bonded host every ten seconds while idle. Multiple bonds
  are not guessed. Four controls/status tests and the pinned-IDF build passed.
- The 0.2.3 OTA reported outcome_unknown after writing: final healthy reconnect
  was not observed within the installer deadline. No repeat write was performed.
  Do not treat that update as fully accepted or icons as visually verified.
