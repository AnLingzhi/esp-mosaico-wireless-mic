#include "app_touch.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_controls.h"
#include "app_hfp.h"
#include "app_hid.h"
#include "connection_status.h"
#include "battery_display.h"
#include "bsp/display.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#if !LV_USE_SNAPSHOT
#error "app_touch requires CONFIG_LV_USE_SNAPSHOT=y"
#endif

#define APP_TOUCH_MAIN_KEY_COUNT       4U
#define APP_TOUCH_VOICE_KEY            0U
#define APP_TOUCH_SETUP_Z_KEY          4U
#define APP_TOUCH_SETUP_SLASH_KEY      5U
#define APP_TOUCH_NO_ACTIVE_KEY        6U
#define APP_TOUCH_DISPLAY_LOCK_MS      1000
#define APP_TOUCH_SCREENSHOT_LOCK_MS   1000
#define APP_TOUCH_BUTTON_WIDTH         172
#define APP_TOUCH_BUTTON_HEIGHT        158
#define APP_TOUCH_BUTTON_RADIUS        38
#define APP_TOUCH_BUTTON_BORDER_WIDTH  2

#define APP_TOUCH_COLOR_BACKGROUND     lv_color_hex(0xF4EFE6)
#define APP_TOUCH_COLOR_PANEL          lv_color_hex(0xFFFDF8)
#define APP_TOUCH_COLOR_TEXT           lv_color_hex(0x34495A)
#define APP_TOUCH_COLOR_MUTED          lv_color_hex(0x71808C)
#define APP_TOUCH_COLOR_VOICE          lv_color_hex(0x789BB6)
#define APP_TOUCH_COLOR_VOICE_HELD     lv_color_hex(0x557D9D)
#define APP_TOUCH_COLOR_CORAL          lv_color_hex(0xD98273)
#define APP_TOUCH_COLOR_CORAL_HELD     lv_color_hex(0xBF685B)
#define APP_TOUCH_COLOR_PRESS          lv_color_hex(0xDDE5E8)
#define APP_TOUCH_COLOR_BORDER         lv_color_hex(0xD6D7D2)

static struct {
    void (*callback)(unsigned index, bool pressed);
    lv_obj_t *screen;
    lv_obj_t *main_page;
    lv_obj_t *shift_page;
    lv_obj_t *battery_label;
    lv_obj_t *audio_link;
    lv_obj_t *keyboard_link;
    lv_timer_t *link_timer;
    lv_draw_buf_t *snapshot;
    SemaphoreHandle_t screenshot_lock;
    StaticSemaphore_t screenshot_lock_storage;
    unsigned active_key;
    bool screenshot_active;
    bool gesture_latched;
    bool started;
} s_touch = {
    .active_key = APP_TOUCH_NO_ACTIVE_KEY,
};

static const lv_font_t *icon_font(void)
{
#if LV_FONT_MONTSERRAT_40
    return &lv_font_montserrat_40;
#elif LV_FONT_MONTSERRAT_28
    return &lv_font_montserrat_28;
#else
    return LV_FONT_DEFAULT;
#endif
}

static const lv_font_t *heading_font(void)
{
#if LV_FONT_MONTSERRAT_28
    return &lv_font_montserrat_28;
#else
    return LV_FONT_DEFAULT;
#endif
}

/* ESP-Iris requires this exact mutable-context callback signature. */
// cppcheck-suppress constParameterCallback
static esp_err_t screenshot_begin(const esp_iris_media_desc_t *requested,
                                  esp_iris_media_desc_t *actual,
                                  // cppcheck-suppress constParameterCallback
                                  uint32_t *total_size, void *user_ctx)
{
    (void)requested;
    if (user_ctx != &s_touch || actual == NULL || total_size == NULL ||
            s_touch.screenshot_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_touch.screenshot_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_touch.screenshot_active || s_touch.screen == NULL) {
        xSemaphoreGive(s_touch.screenshot_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_touch.screenshot_active = true;
    if (!bsp_display_lock(APP_TOUCH_SCREENSHOT_LOCK_MS)) {
        s_touch.screenshot_active = false;
        xSemaphoreGive(s_touch.screenshot_lock);
        return ESP_ERR_TIMEOUT;
    }
    s_touch.snapshot = lv_snapshot_take(s_touch.screen, LV_COLOR_FORMAT_RGB565);
    bsp_display_unlock();

    if (s_touch.snapshot == NULL) {
        s_touch.screenshot_active = false;
        xSemaphoreGive(s_touch.screenshot_lock);
        return ESP_ERR_NO_MEM;
    }

    *actual = (esp_iris_media_desc_t) {
        .x = 0,
        .y = 0,
        .width = s_touch.snapshot->header.w,
        .height = s_touch.snapshot->header.h,
        .stride = s_touch.snapshot->header.stride,
        .format = ESP_IRIS_PIXEL_FORMAT_RGB565,
        .quality = 0,
    };
    *total_size = s_touch.snapshot->data_size;
    xSemaphoreGive(s_touch.screenshot_lock);
    return ESP_OK;
}

/* ESP-Iris requires this exact mutable-context callback signature. */
// cppcheck-suppress constParameterCallback
static esp_err_t screenshot_read(uint32_t offset, uint8_t *out, size_t capacity,
                                 // cppcheck-suppress constParameterCallback
                                 size_t *out_size, void *user_ctx)
{
    if (user_ctx != &s_touch || out == NULL || out_size == NULL || capacity == 0 ||
            s_touch.screenshot_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_touch.screenshot_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_touch.screenshot_active || s_touch.snapshot == NULL) {
        xSemaphoreGive(s_touch.screenshot_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (offset >= s_touch.snapshot->data_size) {
        xSemaphoreGive(s_touch.screenshot_lock);
        return ESP_ERR_INVALID_ARG;
    }

    size_t size = s_touch.snapshot->data_size - offset;
    if (size > capacity) {
        size = capacity;
    }
    memcpy(out, s_touch.snapshot->data + offset, size);
    *out_size = size;
    xSemaphoreGive(s_touch.screenshot_lock);
    return ESP_OK;
}

/* ESP-Iris requires this exact mutable-context callback signature. */
// cppcheck-suppress constParameterCallback
static void screenshot_end(void *user_ctx)
{
    if (user_ctx != &s_touch || s_touch.screenshot_lock == NULL ||
            xSemaphoreTake(s_touch.screenshot_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (s_touch.snapshot != NULL) {
        if (!bsp_display_lock(APP_TOUCH_SCREENSHOT_LOCK_MS)) {
            xSemaphoreGive(s_touch.screenshot_lock);
            return;
        }
        lv_draw_buf_destroy(s_touch.snapshot);
        bsp_display_unlock();
        s_touch.snapshot = NULL;
    }
    s_touch.screenshot_active = false;
    xSemaphoreGive(s_touch.screenshot_lock);
}

static esp_err_t register_screenshot_backend(void)
{
    const esp_iris_screen_backend_t backend = {
        .begin = screenshot_begin,
        .read = screenshot_read,
        .end = screenshot_end,
        .user_ctx = &s_touch,
    };
    return esp_iris_screen_register(&backend);
}

static void release_active_key(void)
{
    if (s_touch.active_key != APP_TOUCH_VOICE_KEY) {
        return;
    }

    const unsigned released_key = s_touch.active_key;
    s_touch.active_key = APP_TOUCH_NO_ACTIVE_KEY;
    if (s_touch.callback) {
        s_touch.callback(released_key, false);
    }
}

static void hold_key_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    const unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);

    if (code == LV_EVENT_PRESSED) {
        if (s_touch.gesture_latched || s_touch.active_key != APP_TOUCH_NO_ACTIVE_KEY) {
            return;
        }
        s_touch.gesture_latched = true;
        s_touch.active_key = index;
        app_controls_feedback();
        if (s_touch.callback) {
            s_touch.callback(index, true);
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) &&
               s_touch.active_key == index) {
        release_active_key();
    }
}

static void click_key_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    app_controls_feedback();
    if (s_touch.callback) {
        s_touch.callback(index, true);
    }
}

static void input_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        release_active_key();
        s_touch.gesture_latched = false;
    }
}

static void show_page(lv_obj_t *page)
{
    release_active_key();
    lv_obj_add_flag(s_touch.main_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_touch.shift_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
}

static void settings_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        app_controls_feedback();
        show_page(s_touch.shift_page);
    }
}

static void back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        app_controls_feedback();
        show_page(s_touch.main_page);
    }
}

static void style_page(lv_obj_t *page)
{
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_bg_color(page, APP_TOUCH_COLOR_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
}

static lv_obj_t *create_button(lv_obj_t *parent, int32_t width, int32_t height,
                               lv_color_t color, lv_color_t pressed_color)
{
    lv_obj_t *button = lv_button_create(parent);
    if (button == NULL) {
        return NULL;
    }

    lv_obj_set_size(button, width, height);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_radius(button, APP_TOUCH_BUTTON_RADIUS, 0);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_bg_color(button, pressed_color, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, APP_TOUCH_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(button, APP_TOUCH_BUTTON_BORDER_WIDTH, 0);
    lv_obj_set_style_shadow_color(button, APP_TOUCH_COLOR_TEXT, 0);
    lv_obj_set_style_shadow_width(button, 12, 0);
    lv_obj_set_style_shadow_opa(button, LV_OPA_10, 0);
    lv_obj_set_style_shadow_offset_y(button, 4, 0);
    lv_obj_set_style_transform_width(button, -6, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(button, -6, LV_STATE_PRESSED);
    return button;
}

static bool add_icon_label(lv_obj_t *button, const char *icon, const char *caption,
                           lv_color_t color)
{
    lv_obj_t *icon_label = lv_label_create(button);
    lv_obj_t *caption_label = lv_label_create(button);
    if (icon_label == NULL || caption_label == NULL) {
        return false;
    }

    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, icon_font(), 0);
    lv_obj_set_style_text_color(icon_label, color, 0);
    lv_obj_align(icon_label, LV_ALIGN_CENTER, 0, -13);

    lv_label_set_text(caption_label, caption);
    lv_obj_set_style_text_color(caption_label, color, 0);
    lv_obj_align(caption_label, LV_ALIGN_BOTTOM_MID, 0, -15);
    return true;
}

static bool add_mic_icon(lv_obj_t *button, lv_color_t color)
{
    lv_obj_t *body = lv_obj_create(button);
    lv_obj_t *stem = lv_obj_create(button);
    lv_obj_t *base = lv_obj_create(button);
    lv_obj_t *caption = lv_label_create(button);
    if (body == NULL || stem == NULL || base == NULL || caption == NULL) {
        return false;
    }

    lv_obj_remove_style_all(body);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(body, 30, 54);
    lv_obj_set_style_radius(body, 15, 0);
    lv_obj_set_style_bg_color(body, color, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, -23);

    lv_obj_remove_style_all(stem);
    lv_obj_remove_flag(stem, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(stem, 5, 18);
    lv_obj_set_style_bg_color(stem, color, 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_align(stem, LV_ALIGN_CENTER, 0, 13);

    lv_obj_remove_style_all(base);
    lv_obj_remove_flag(base, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(base, 34, 5);
    lv_obj_set_style_radius(base, 3, 0);
    lv_obj_set_style_bg_color(base, color, 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_align(base, LV_ALIGN_CENTER, 0, 22);

    lv_label_set_text(caption, "TALK");
    lv_obj_set_style_text_color(caption, color, 0);
    lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -15);
    return true;
}

static lv_obj_t *create_main_button(lv_obj_t *page, unsigned index, int32_t x,
                                    int32_t y, const char *icon, const char *caption,
                                    lv_color_t fill, lv_color_t pressed,
                                    lv_color_t foreground)
{
    lv_obj_t *button = create_button(page, APP_TOUCH_BUTTON_WIDTH,
                                     APP_TOUCH_BUTTON_HEIGHT, fill, pressed);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_align(button, LV_ALIGN_CENTER, x, y);
    const bool label_created = index == 0 ? add_mic_icon(button, foreground) :
                               add_icon_label(button, icon, caption, foreground);
    if (!label_created) {
        lv_obj_delete(button);
        return NULL;
    }

    if (index == 0) {
        lv_obj_add_event_cb(button, hold_key_event_cb, LV_EVENT_ALL,
                            (void *)(uintptr_t)index);
    } else if (index == 2) {
        lv_obj_add_event_cb(button, settings_event_cb, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_add_event_cb(button, click_key_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)index);
    }
    return button;
}

static esp_err_t create_main_page(lv_obj_t *screen, lv_obj_t **out_page)
{
    lv_obj_t *page = lv_obj_create(screen);
    if (page == NULL) {
        return ESP_ERR_NO_MEM;
    }
    style_page(page);

    static const struct {
        int16_t x;
        int16_t y;
        const char *icon;
        const char *caption;
    } keys[APP_TOUCH_MAIN_KEY_COUNT] = {
        {-92, -84, "", "TALK"},
        { 92, -84, LV_SYMBOL_NEW_LINE, "ENTER"},
        { 92,  84, LV_SYMBOL_SETTINGS, "SETUP"},
        {-92,  84, LV_SYMBOL_BACKSPACE, "CLEAR"},
    };

    for (unsigned index = 0; index < APP_TOUCH_MAIN_KEY_COUNT; ++index) {
        lv_color_t fill = APP_TOUCH_COLOR_PANEL;
        lv_color_t pressed = APP_TOUCH_COLOR_PRESS;
        lv_color_t foreground = APP_TOUCH_COLOR_TEXT;
        if (index == 0) {
            fill = APP_TOUCH_COLOR_VOICE;
            pressed = APP_TOUCH_COLOR_VOICE_HELD;
            foreground = APP_TOUCH_COLOR_PANEL;
        } else if (index == 3) {
            fill = APP_TOUCH_COLOR_CORAL;
            pressed = APP_TOUCH_COLOR_CORAL_HELD;
            foreground = APP_TOUCH_COLOR_PANEL;
        }
        if (create_main_button(page, index, keys[index].x, keys[index].y,
                               keys[index].icon, keys[index].caption, fill,
                               pressed, foreground) == NULL) {
            lv_obj_delete(page);
            return ESP_ERR_NO_MEM;
        }
    }

    s_touch.battery_label = lv_label_create(page);
    if (s_touch.battery_label == NULL) {
        lv_obj_delete(page);
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(s_touch.battery_label, LV_SYMBOL_BATTERY_EMPTY " --%");
    lv_obj_remove_flag(s_touch.battery_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_touch.battery_label, APP_TOUCH_COLOR_TEXT, 0);
    lv_obj_set_width(s_touch.battery_label, 110);
    lv_obj_set_style_text_align(s_touch.battery_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_touch.battery_label, LV_ALIGN_TOP_RIGHT, -110, 42);

    s_touch.audio_link = lv_label_create(page);
    s_touch.keyboard_link = lv_label_create(page);
    if (!s_touch.audio_link || !s_touch.keyboard_link) {
        lv_obj_delete(page);
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(s_touch.audio_link, LV_SYMBOL_AUDIO " " LV_SYMBOL_CLOSE);
    lv_label_set_text(s_touch.keyboard_link, LV_SYMBOL_KEYBOARD " " LV_SYMBOL_CLOSE);
    lv_obj_remove_flag(s_touch.audio_link, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_touch.keyboard_link, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_touch.audio_link, APP_TOUCH_COLOR_MUTED, 0);
    lv_obj_set_style_text_color(s_touch.keyboard_link, APP_TOUCH_COLOR_MUTED, 0);
    lv_obj_align(s_touch.audio_link, LV_ALIGN_TOP_LEFT, 110, 42);
    lv_obj_align(s_touch.keyboard_link, LV_ALIGN_TOP_LEFT, 181, 42);

    *out_page = page;
    return ESP_OK;
}

static void update_connection_icons(lv_timer_t *timer)
{
    (void)timer;
    const app_hfp_status_t hfp = app_hfp_status();
    const app_link_state_t audio = app_audio_link_state(hfp.connected, hfp.streaming);
    uint32_t generation;
    const bool keyboard = app_hid_connected(&generation);
    lv_label_set_text(s_touch.audio_link, audio == APP_LINK_ACTIVE ?
                      LV_SYMBOL_AUDIO " " LV_SYMBOL_PLAY : audio == APP_LINK_READY ?
                      LV_SYMBOL_AUDIO " " LV_SYMBOL_OK : LV_SYMBOL_AUDIO " " LV_SYMBOL_CLOSE);
    lv_label_set_text(s_touch.keyboard_link, keyboard ?
                      LV_SYMBOL_KEYBOARD " " LV_SYMBOL_OK : LV_SYMBOL_KEYBOARD " " LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(s_touch.audio_link, audio == APP_LINK_ACTIVE ?
                                APP_TOUCH_COLOR_CORAL : audio == APP_LINK_READY ?
                                APP_TOUCH_COLOR_VOICE_HELD : APP_TOUCH_COLOR_MUTED, 0);
    lv_obj_set_style_text_color(s_touch.keyboard_link, keyboard ?
                                APP_TOUCH_COLOR_VOICE_HELD : APP_TOUCH_COLOR_MUTED, 0);
}

static esp_err_t create_shift_page(lv_obj_t *screen, lv_obj_t **out_page)
{
    lv_obj_t *page = lv_obj_create(screen);
    if (page == NULL) {
        return ESP_ERR_NO_MEM;
    }
    style_page(page);

    lv_obj_t *title = lv_label_create(page);
    lv_obj_t *help = lv_label_create(page);
    lv_obj_t *left_key = create_button(page, 168, 132, APP_TOUCH_COLOR_VOICE,
                                       APP_TOUCH_COLOR_VOICE_HELD);
    lv_obj_t *right_key = create_button(page, 168, 132, APP_TOUCH_COLOR_PANEL,
                                        APP_TOUCH_COLOR_PRESS);
    lv_obj_t *back = create_button(page, 150, 62, APP_TOUCH_COLOR_PANEL,
                                   APP_TOUCH_COLOR_PRESS);
    if (title == NULL || help == NULL || left_key == NULL || right_key == NULL ||
            back == NULL) {
        lv_obj_delete(page);
        return ESP_ERR_NO_MEM;
    }

    lv_label_set_text(title, "Keyboard Setup");
    lv_obj_set_style_text_font(title, heading_font(), 0);
    lv_obj_set_style_text_color(title, APP_TOUCH_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 42);

    lv_label_set_text(help, "Follow Mac prompts. Choose ANSI if asked.");
    lv_obj_set_width(help, 370);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(help, APP_TOUCH_COLOR_MUTED, 0);
    lv_obj_align(help, LV_ALIGN_TOP_MID, 0, 91);

    lv_obj_align(left_key, LV_ALIGN_CENTER, -90, 34);
    lv_obj_t *left_title = lv_label_create(left_key);
    lv_obj_t *left_help = lv_label_create(left_key);
    lv_obj_align(right_key, LV_ALIGN_CENTER, 90, 34);
    lv_obj_t *right_title = lv_label_create(right_key);
    lv_obj_t *right_help = lv_label_create(right_key);
    if (left_title == NULL || left_help == NULL || right_title == NULL ||
            right_help == NULL) {
        lv_obj_delete(page);
        return ESP_ERR_NO_MEM;
    }

    lv_label_set_text(left_title, "1  Z");
    lv_obj_set_style_text_font(left_title, heading_font(), 0);
    lv_obj_set_style_text_color(left_title, APP_TOUCH_COLOR_PANEL, 0);
    lv_obj_align(left_title, LV_ALIGN_CENTER, 0, -18);
    lv_label_set_text(left_help, "Beside left Shift");
    lv_obj_set_style_text_color(left_help, APP_TOUCH_COLOR_PANEL, 0);
    lv_obj_align(left_help, LV_ALIGN_CENTER, 0, 22);
    lv_obj_add_event_cb(left_key, click_key_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)APP_TOUCH_SETUP_Z_KEY);

    lv_label_set_text(right_title, "2  /");
    lv_obj_set_style_text_font(right_title, heading_font(), 0);
    lv_obj_set_style_text_color(right_title, APP_TOUCH_COLOR_TEXT, 0);
    lv_obj_align(right_title, LV_ALIGN_CENTER, 0, -18);
    lv_label_set_text(right_help, "Beside right Shift");
    lv_obj_set_style_text_color(right_help, APP_TOUCH_COLOR_MUTED, 0);
    lv_obj_align(right_help, LV_ALIGN_CENTER, 0, 22);
    lv_obj_add_event_cb(right_key, click_key_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)APP_TOUCH_SETUP_SLASH_KEY);

    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_t *back_label = lv_label_create(back);
    if (back_label == NULL) {
        lv_obj_delete(page);
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(back_label, LV_SYMBOL_LEFT "  BACK");
    lv_obj_set_style_text_color(back_label, APP_TOUCH_COLOR_TEXT, 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back, back_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    *out_page = page;
    return ESP_OK;
}

static esp_err_t create_ui(lv_display_t *display, lv_obj_t **out_screen)
{
    if (out_screen == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lv_display_set_default(display);
    lv_obj_t *screen = lv_obj_create(NULL);
    if (screen == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_bg_color(screen, APP_TOUCH_COLOR_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    esp_err_t result = create_main_page(screen, &s_touch.main_page);
    if (result == ESP_OK) {
        result = create_shift_page(screen, &s_touch.shift_page);
    }
    if (result != ESP_OK) {
        s_touch.main_page = NULL;
        s_touch.shift_page = NULL;
        lv_obj_delete(screen);
        return result;
    }

    *out_screen = screen;
    return ESP_OK;
}

esp_err_t app_touch_start(void (*callback)(unsigned index, bool pressed))
{
    if (s_touch.started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_touch.screenshot_lock == NULL) {
        s_touch.screenshot_lock =
            xSemaphoreCreateMutexStatic(&s_touch.screenshot_lock_storage);
        if (s_touch.screenshot_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        return ESP_FAIL;
    }
    if (!bsp_display_lock(APP_TOUCH_DISPLAY_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_indev_t *input = bsp_display_get_input_dev();
    lv_obj_t *screen = NULL;
    esp_err_t result = input ? create_ui(display, &screen) : ESP_ERR_INVALID_STATE;
    if (result == ESP_OK) {
        s_touch.callback = callback;
        s_touch.screen = screen;
        s_touch.active_key = APP_TOUCH_NO_ACTIVE_KEY;
        s_touch.gesture_latched = false;
        result = register_screenshot_backend();
        if (result == ESP_OK) {
            s_touch.link_timer = lv_timer_create(update_connection_icons, 250, NULL);
            if (!s_touch.link_timer) {
                result = ESP_ERR_NO_MEM;
            }
        }
        if (result == ESP_OK) {
            lv_screen_load(screen);
            lv_indev_add_event_cb(input, input_event_cb, LV_EVENT_RELEASED, NULL);
            s_touch.started = true;
        } else {
            s_touch.screen = NULL;
            s_touch.main_page = NULL;
            s_touch.shift_page = NULL;
            s_touch.callback = NULL;
            lv_obj_delete(screen);
        }
    }

    bsp_display_unlock();
    if (result == ESP_OK) {
        const esp_err_t battery_error = app_battery_display_start(s_touch.battery_label);
        if (battery_error != ESP_OK) {
            ESP_LOGW("app_touch", "Battery indicator unavailable: %s", esp_err_to_name(battery_error));
        }
    }
    return result;
}
