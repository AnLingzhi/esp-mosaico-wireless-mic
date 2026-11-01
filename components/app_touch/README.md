# app_touch

`app_touch` owns the wireless microphone's physical 480 x 480 touchscreen UI. It starts the existing ESP-Mosaico BSP display/LVGL adapter, presents four large icon keys in a two-by-two arrangement inside the round panel's safe boundary, provides a Mac Keyboard Setup Assistant page, and registers the ESP-Iris screenshot backend used by `mosaico.py`. It does not modify Recovery, configure the host computer, or provide a simulator.

## API and lifecycle

Main-page layout: Talk top-left, Enter top-right, Clear bottom-left, and Setup
bottom-right. Positions do not change the logical callback indices or colors.

Call `app_touch_start(callback)` once after board services required by the BSP are available. The optional callback runs in the BSP LVGL task. Talk uses index `0` and reports `pressed=true` immediately, then `pressed=false` on release, slide-off, lost press, or page exit. Enter (`1`), Clear (`3`), setup Z (`4`), and setup slash (`5`) report only `(index, true)` after `LV_EVENT_CLICKED`. The settings key (`2`) opens the setup page and never emits a host-key callback. Keep the callback non-blocking and hand work to the owning application task when needed.

Each effective press requests one short haptic pulse through `app_controls_feedback()`. Talk does not repeat. Click keys do not activate from a slide or drag because they use completed-click events only. The setup page sends the ANSI keys beside left Shift (`Z`) and right Shift (`/`) when prompted; it does not claim to configure the Mac.

Startup returns an error if the display cannot start, touch input is unavailable, or the LVGL lock cannot be acquired. No application/Recovery state is changed on failure. The component is a process-lifetime singleton and does not provide a stop operation.

ESP-Iris screenshots are captured on demand with LVGL snapshot support. `begin` allocates one full-screen RGB565 snapshot, `read` serves immutable chunks, and `end` frees it. A static mutex serializes non-UI ESP-Iris callers; concurrent captures are rejected, while LVGL snapshot creation and destruction run under the BSP display lock. Pixel bytes remain LVGL's native-endian RGB565 representation expected by the ESP-Iris protocol.

```c
#include "app_touch.h"

static void touch_key_changed(unsigned index, bool pressed)
{
    /* Queue the state change for the application owner. */
}

ESP_ERROR_CHECK(app_touch_start(touch_key_changed));
```

## Battery indicator

The main page includes a non-interactive top-right battery symbol and measured
percentage, within the circular panel boundary. A low-priority task reads BQ27220
every five seconds, adapting the system demo's checked standard-word reads and
BATTPRES/DSG validation. BSP initialization reuses its default battery profile.
I2C reads happen outside the display lock; only label updates acquire that lock.
Read failures or absent/invalid samples show `--%`, never a fabricated full/empty
percentage. Positive measured current with DSG cleared shows a charging symbol;
a full battery at zero current is displayed as full without claiming active charging.
Low battery (20 percent or less, not charging) uses muted coral. A missing gauge
does not block microphone startup. The task and label have application lifetime;
the settings page hides the main-page indicator without deleting it.

## Dependencies and required configuration

The component uses public APIs from `esp-mosaico-bsp`, `app_controls`, and LVGL. The integrating application must enable:

```text
CONFIG_BSP_DISPLAY_ENABLE=y
CONFIG_BSP_DISPLAY_LVGL_ENABLE=y
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_OS_FREERTOS=y
CONFIG_LV_USE_SNAPSHOT=y
CONFIG_LV_FONT_MONTSERRAT_28=y
CONFIG_LV_FONT_MONTSERRAT_40=y
```

The 28/40 px fonts provide readable headings and large LVGL symbols; the UI falls back to the configured default font if omitted. The BSP's default display configuration enables the CST9220 touch device. No partition change or additional image asset is required.
