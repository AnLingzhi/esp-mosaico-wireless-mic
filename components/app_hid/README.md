# Classic Bluetooth keyboard

Registers HID on the existing Bluedroid Classic controller started by `app_hfp`.
The microphone and keyboard share the same Bluetooth address. HFP SLC events
provide the authenticated peer; the control task retries HID connection only to
that peer. HFP audio callbacks never wait for HID reports.

Report 1 is a standard eight-byte keyboard report. The physical voice key uses
F18 (usage 0x6d), mapped to Fn by the device-specific native Mac helper under
`tools/macos`. Enter uses usage 0x28. No Apple vendor identity is advertised.
Mac input applications must already use a hold-Fn voice binding.

`app_hid_set_keys` is called only by the control task. It queues a copied report;
success means queued, not observed by macOS. Asynchronous send failures invalidate
the generation and disconnect HID to release keys. Connection changes reset the
cached report and invalidate all pending gestures. The module lives until reboot.

Native Fn recognition and actual input-method behavior require hardware testing;
mapping readback alone does not establish dictation success.
