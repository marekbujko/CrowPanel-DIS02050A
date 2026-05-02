// =============================================================================
// crowpanel_backlight.cpp — CrowPanel DIS05020A v1.1 init for Meshtastic
//
// 1. Optionally clears OTA state so the factory boot selector runs next boot
// 2. Pre-mounts LittleFS on "mtdata" partition
// 3. Wakes the I2C backlight controller + GT911 touch
// 4. Runs a background task that handles the backlight idle timer and the
//    soft-wake ramp. Touch input is NOT read here — LVGL's touch_cb is the
//    single GT911 reader, and it pokes this task via backlight_notify_activity().
//    See crowpanel_backlight.h for the rationale.
// =============================================================================

#include "crowpanel_backlight.h"

#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>
#include <esp_partition.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#if HAS_TFT && USE_MCUI
#include "graphics/mcui/McDisplay.h"
#endif

static constexpr uint8_t  BL_I2C_ADDR = 0x30;
static constexpr uint8_t  TOUCH_I2C_ADDR = 0x5D;
static constexpr uint8_t  BL_V11_OFF = 0x05;
static constexpr uint8_t  BL_V11_MIN = 0x06;
static constexpr uint8_t  BL_V11_MAX = 0x10;
static constexpr uint8_t  BL_V11_TOUCH_WAKE = 0x19;
static constexpr uint8_t  BL_V12_OFF = 245;
static constexpr uint32_t DEFAULT_TIMEOUT_SECS = 30;
static constexpr uint32_t SOFT_WAKE_MS = 1500;
static constexpr uint32_t STARTUP_GRACE_MS = 5000;
static constexpr uint32_t TASK_PERIOD_MS = 100;
static constexpr int      PROBE_ATTEMPTS = 20;
static const char *TAG = "crowpanel";

// ---------------------------------------------------------------------------
// I2C helpers for backlight controller at 0x30
// ---------------------------------------------------------------------------
static bool _i2c_probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return (Wire.endTransmission() == 0);
}

static SemaphoreHandle_t s_wire_lock = nullptr;

static void _bl_cmd(uint8_t cmd) {
    if (s_wire_lock) xSemaphoreTake(s_wire_lock, portMAX_DELAY);
    Wire.beginTransmission(BL_I2C_ADDR);
    Wire.write(cmd);
    Wire.endTransmission();
    if (s_wire_lock) xSemaphoreGive(s_wire_lock);
}

// Elecrow changed the 0x30 backlight protocol between board revisions.
// Older boards use the small v1.1 command set (0x05..0x10), while newer
// boards use a linear brightness scale where 0 is brightest and 245 is off.
// Drive both so the same firmware works across silent hardware batches.
static void _bl_cmd_compat(uint8_t v11_cmd, uint8_t v12_cmd)
{
    _bl_cmd(v11_cmd);
    _bl_cmd(v12_cmd);
}

// ---------------------------------------------------------------------------
// State, shared lock-free between the backlight task (core 1), LVGL touch
// cb (core 0) and the mesh RX observer (core 1). All writes and reads are
// 32-bit aligned primitives so ESP32 word-aligned access is atomic; the
// `volatile` keeps the compiler from caching across task boundaries.
// ---------------------------------------------------------------------------
static volatile uint32_t s_last_activity_ms = 0;
static volatile bool     s_wake_requested   = false;
static volatile bool     s_screen_on        = true;
static volatile uint32_t s_timeout_secs     = DEFAULT_TIMEOUT_SECS;

// ---------------------------------------------------------------------------
// Soft wake: ramp brightness from ~10% to 100% over ~1.5 seconds
// Uses quadratic ease-in so lower levels (where the eye is most sensitive)
// get more dwell time, making the ramp feel smoother despite only 10 hw steps.
// Runs exclusively on the backlight task, so it can safely call delay().
// ---------------------------------------------------------------------------
static void _bl_soft_wake() {
    const uint8_t start = BL_V11_MIN;
    const uint8_t end   = BL_V11_MAX;
    const uint8_t steps = end - start;

    for (uint8_t i = 0; i <= steps; i++) {
        _bl_cmd(start + i);
        if (i < steps) {
            uint32_t dt = SOFT_WAKE_MS * (2 * i + 1) / (steps * steps);
            delay(dt);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
extern "C" void backlight_notify_activity(void) {
    s_last_activity_ms = millis();
    if (!s_screen_on) s_wake_requested = true;
}

extern "C" void backlight_wake_if_off(void) {
    if (!s_screen_on) {
        s_last_activity_ms = millis();
        s_wake_requested = true;
    }
}

extern "C" bool backlight_is_screen_on(void) {
    return s_screen_on;
}

extern "C" void backlight_set_timeout_secs(uint32_t secs) {
    // 0 means "use default" in Meshtastic config.display.screen_on_secs
    if (secs == 0) secs = DEFAULT_TIMEOUT_SECS;
    s_timeout_secs = secs;
}

extern "C" void backlight_i2c_lock(void) {
    if (s_wire_lock) xSemaphoreTake(s_wire_lock, portMAX_DELAY);
}
extern "C" void backlight_i2c_unlock(void) {
    if (s_wire_lock) xSemaphoreGive(s_wire_lock);
}

// ---------------------------------------------------------------------------
// Background task: idle timer + wake ramp. Does NOT poll GT911 — LVGL does.
// ---------------------------------------------------------------------------
static void backlight_task(void *) {
    // Wait for display init to complete before we start evaluating timeout
    vTaskDelay(pdMS_TO_TICKS(STARTUP_GRACE_MS));
    s_last_activity_ms = millis();

    while (true) {
        // Service a pending wake request from the UI thread or mesh observer
        if (s_wake_requested) {
            s_wake_requested = false;
            if (!s_screen_on) {
#if HAS_TFT && USE_MCUI
                mcui::display_wake_panel();
#endif
                _bl_soft_wake();
                s_screen_on = true;
            }
            s_last_activity_ms = millis();
        }

        // UINT32_MAX disables auto-sleep (matches Meshtastic's "always on").
        uint32_t to = s_timeout_secs;
        if (to != UINT32_MAX && s_screen_on) {
            uint32_t elapsed = millis() - s_last_activity_ms;
            if (elapsed >= to * 1000U) {
                _bl_cmd_compat(BL_V11_OFF, BL_V12_OFF);
#if HAS_TFT && USE_MCUI
                mcui::display_sleep_panel();
#endif
                s_screen_on = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
    }
}

// ---------------------------------------------------------------------------
// initVariant() — called by Arduino core before setup()
// ---------------------------------------------------------------------------
extern "C" void initVariant() {

    // ---- 1. Clear OTA state so the factory boot selector runs next boot ----
    //
    // The CrowPanel dual-boot layout is:
    //   factory = selector, ota_0 = MeshCore, ota_1 = Meshtastic.
    // Clearing otadata makes the ROM/IDF boot path fall back to the factory
    // selector after Meshtastic has run. Guard this by both a build flag and
    // a runtime factory-partition check so app0/app1-only layouts are safe.
#if CROWPANEL_RETURN_TO_BOOT_SELECTOR
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    const esp_partition_t *otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (factory && otadata) {
        esp_err_t err = esp_partition_erase_range(otadata, 0, otadata->size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "cleared otadata; factory boot selector will run on next reboot");
        } else {
            ESP_LOGW(TAG, "failed to clear otadata (%d)", (int)err);
        }
    } else {
        ESP_LOGW(TAG, "boot-selector return requested but factory/otadata partition is missing");
    }
#endif

    // ---- 2. Pre-mount LittleFS on Meshtastic's own "mtdata" partition ----
    LittleFS.begin(true, "/littlefs", 10, "mtdata");

    // ---- 3. I2C backlight + GT911 wake sequence (from MeshCore) ----
    Wire.begin(15, 16);
    Wire.setClock(400000);
    delay(50);

    s_wire_lock = xSemaphoreCreateMutex();

    for (int i = 0; i < PROBE_ATTEMPTS; i++) {
        if (_i2c_probe(BL_I2C_ADDR) && _i2c_probe(TOUCH_I2C_ADDR)) break;
        _bl_cmd(BL_V11_TOUCH_WAKE);
        pinMode(1, OUTPUT);
        digitalWrite(1, LOW);
        delay(120);
        pinMode(1, INPUT);
        delay(100);
    }

    // Backlight ON (soft ramp to reduce inrush current on battery)
    _bl_soft_wake();
    s_screen_on = true;
    s_last_activity_ms = millis();

    // ---- 4. Start backlight monitor task ----
    xTaskCreatePinnedToCore(
        backlight_task,
        "blTask",
        2048,
        nullptr,
        1,
        nullptr,
        1  // Core 1
    );
}
