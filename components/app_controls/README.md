# Microphone controls

GPIO7 retains 30 ms debounce, 250 ms hold-to-talk, and a 300 ms double-click
window for one Enter. Touch voice is immediate on press; release/slide-off
releases Fn. Setup-page clicks send Z (usage 0x1d) or slash (0x38), once each.

The UI emits completed clicks for Enter and Clear All. Enter is one 40 ms pulse.
Clear All sends Command+A, all-up, Backspace, all-up, each for at least 40 ms after
transport acceptance. It acts on the current Mac focus, not a particular app;
keep focus in an editable text field. No private app data or focus is inspected.
Actions are ignored while Fn or another sequence is active, never queued
for later. Disconnects cancel all gestures/sequences and require release before
rearming. Mapping readback is not proof of actual host key delivery.

All HID reports and motor operations are serialized by one 10 ms polling task.
UI feedback requests trigger a 35 ms motor pulse at 60 percent strength. Every
new physical down edge also requests one pulse; holds do not repeatedly vibrate.
Motor startup failure is logged without disabling audio or keys.

Host tests under tests/controls cover gesture timing and the exact destructive
key sequence, including release gaps, transport acceptance and clock wrap.
