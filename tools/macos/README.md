# Mac key settings

Build the native AppKit configuration window with `sh tools/macos/build_settings.sh`.
The script prints the standalone application path. No arguments or `--settings`
opens the window. The existing `--status`, `--apply`, and `--watch` modes retain
their device-specific F18-to-Fn behavior.

The watcher needs `~/Library/Application Support/MosaicoMic/board-serial.txt`
containing the paired board's 12 hexadecimal Bluetooth serial digits without
separators. Keep it local with user-only permissions; an absent or invalid value
disables the device match instead of matching another keyboard.

The window configures prefix key, Command/Option/Control/Shift, and an integer
delay from 0 to 2000 milliseconds. It previews the sequence and warns about Return
submitting text. `Apply and enable` writes atomically to
`~/Library/Application Support/MosaicoMic/prefix-draft.json`. Invalid or unreadable
existing configuration disables saving to avoid overwriting it.

When enabled, the `--watch` service installs a device-filtered CGEvent tap for
the verified Mosaico Bluetooth keyboard type. On F18/Fn press it suppresses the
original event, posts the configured prefix key and modifiers, waits the configured
delay, and synthesizes an Fn hold; F18/Fn release synthesizes Fn release. Other
keyboards are filtered by keyboard type and synthetic events carry a private
magic tag. The service requires macOS Accessibility and Input Monitoring permission.
TCC matches the exact signed application fingerprint, so do not re-sign or replace
the installed app without re-authorizing it. Runtime state is written atomically to
`~/Library/Application Support/MosaicoMic/runtime-status.json`.

Hardware acceptance still needs a real F18 press with Codex frontmost. A synthetic
F18 was observed entering the tap, but keyboard synthesis visibility across event
taps and actual focus/dictation behavior are not claimed as hardware acceptance.

`--test-config` validates supported keys, malformed fields, boundaries and JSON
round trips. UI layout and interactive save/reopen still need visual validation.

## Live diagnostics

The Chinese settings window includes a read-only, selectable log panel refreshed
every 500 ms. The watch process asynchronously writes `key-events.log` beside
the configuration, retaining the latest 400 lines from the current process.
Only F18/Fn candidate events, configured prefix actions, mapping/permission
state and execution stages are logged; ordinary text and other keys are not.
Event delivery means CGEventPost was called, not that dictation or focus succeeded.
Fn flagsChanged release and F18 keyUp both clear the held state and cancel pending
actions. Delayed actions execute on the same main run loop as incoming events.
An independent listen-only session event tap reports observation of this helper's
marked synthetic events; this does not prove focus changes or application handling.
`--test-events` covers repeated holds, early release cancellation and raw F18 release
without posting actual keyboard events.
`--test-log` checks bounded retention using a temporary file without posting keys.
Replacing an ad-hoc signed executable can require macOS permission reapproval.
