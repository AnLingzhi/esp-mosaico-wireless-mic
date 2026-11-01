# HFP microphone transport

Call `app_hfp_start()` once from the composition task after `app_audio_start()`.
It waits at most ten seconds for asynchronous profile registration. Dependencies:
ESP-IDF Bluetooth, FreeRTOS and `app_audio`. Resources live until reboot; errors
leave USB Recovery available. Never erase NVS or pairing records automatically.

The device advertises **Mosaico Mic**, hands-free Audio/Video class, SSP Just Works
pairing (NoInputNoOutput), discoverable while idle. Mac initiates the profile and
audio connection. No calls or voice-assistant activation are synthesized.

The fixed IDF's internal codec accepts mono signed 16-bit PCM through
`esp_hf_client_legacy_api.h`. This deprecated-but-supported interface avoids adding
an application SBC encoder. Keep `CONFIG_BT_HFP_USE_EXTERNAL_CODEC` off. Future SDK
migration must replace this API before changing the pin.

SCO events choose 16 kHz for mSBC, 8 kHz for CVSD. Incoming packets supply the
transport clock; their audio is discarded. Outgoing callbacks read real microphone
PCM without blocking. No speaker or loopback is enabled. Codec reporting comes
from actual HFP audio-state events, not Mac file metadata. Bluedroid owns profile
callbacks; `app_audio` synchronizes its capture and FIFO across tasks.

System-input recognition, activation without a call, audio quality and reconnect
behavior require physical Mac interoperability tests. Pairing is not acceptance.
