# Device composition

`app_main()` starts ESP-Iris Recovery support, initializes application NVS without
erasing existing records, starts microphone capture, then registers HFP HF.
It marks the firmware healthy only after microphone and profile initialization
succeed. Failure is logged and leaves the Recovery RPC available.

Called once by ESP-IDF in task context. No public API or mutable shared context.
Private dependencies are `app_audio`, `app_hfp`, ESP-Iris, retained app Recovery,
NVS and logging. Central BSP/Iris path overrides live in `idf_component.yml`.
All audio buffers and Bluetooth/capture tasks have application lifetime.
