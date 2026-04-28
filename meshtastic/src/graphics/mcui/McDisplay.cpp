#if HAS_TFT && USE_MCUI

#include "McDisplay.h"
#include "McUI.h"
#include "configuration.h"
#include "crowpanel_backlight.h"

// LovyanGFX driver for the DIS05020A v1.1 800x480 RGB panel + GT911 touch.
#include "graphics/LGFX/LGFX_ELECROW70.h"

#include "mesh/NodeDB.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

namespace mcui {

// Native panel = 800x480 landscape. We ask LovyanGFX to rotate it 90° via
// setRotation(1) so the whole stack (drawing + touch) sees 480x800 portrait
// natively. LVGL then draws portrait without any rotation layer, and the
// flush just calls pushImageDMA() — LGFX handles the pixel transform into
// the RGB panel's internal framebuffer.
static LGFX_ELECROW70 *gfx = nullptr;
static lv_display_t *disp = nullptr;
static lv_indev_t *indev = nullptr;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

// ---- LVGL flush pipeline ---------------------------------------------------
// Render runs on the UI task (core 0). The actual pushImage (rotated
// SRAM->PSRAM copy via LovyanGFX) runs on a worker task pinned to core 1,
// so per-frame wall time is max(render, copy) instead of render + copy.
//
// flush_cb is the LVGL-facing entry point: it just enqueues the chunk and
// returns. Queue depth 1 matches LVGL's two-buffer flow (only one flush in
// flight at a time) and provides natural backpressure via blocking enqueue
// when the worker is still busy with the previous chunk.
//
// Coordinates are in the logical (rotated) mcui space; LovyanGFX transforms
// them into the physical 800x480 framebuffer automatically.
struct flush_msg_t {
    lv_display_t *d;
    lv_area_t     area;
    uint8_t      *px_map;
};
static QueueHandle_t s_flush_q = nullptr;

static void flush_worker_task(void *)
{
    flush_msg_t m;
    for (;;) {
        if (xQueueReceive(s_flush_q, &m, portMAX_DELAY) != pdTRUE) continue;
        const int32_t w = m.area.x2 - m.area.x1 + 1;
        const int32_t h = m.area.y2 - m.area.y1 + 1;
        gfx->pushImageDMA(m.area.x1, m.area.y1, w, h, reinterpret_cast<uint16_t *>(m.px_map));
        lv_display_flush_ready(m.d);
    }
}

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map)
{
    flush_msg_t m = { d, *area, px_map };
    xQueueSend(s_flush_q, &m, portMAX_DELAY);
}

// ---- LVGL indev (touch) callback ------------------------------------------
// LovyanGFX's getTouch() returns coordinates in the rotated (logical) space
// when setRotation() is applied, so we can feed them straight to LVGL.
//
// This callback is ALSO the single reader of the GT911 touch register — the
// backlight task used to poll it independently and would sometimes race us
// to clearing the touch-status flag, swallowing wake-up taps. See
// crowpanel_backlight.h for the full story.
//
// Wake-tap semantics: if the screen is currently asleep, a tap is consumed
// purely to wake the screen — we do NOT forward it as a press to LVGL,
// otherwise the first touch would accidentally activate whatever widget
// was sitting under the user's finger before the screen went dark.
static void touch_cb(lv_indev_t *drv, lv_indev_data_t *data)
{
    int32_t x = 0, y = 0;
    // GT911 touch lives on the same Wire bus as the 0x30 backlight
    // controller that the backlight task writes to from core 1.
    // Concurrent Arduino-Wire access from two cores corrupts the bus,
    // so we serialize against the backlight task through the shared
    // mutex it exposes. Lock held for a single short I2C transaction.
    backlight_i2c_lock();
    bool pressed = gfx->getTouch(&x, &y);
    backlight_i2c_unlock();
    if (pressed) {
        // Always note activity so the idle timer resets and the screen
        // wakes even if we end up swallowing this particular press event.
        backlight_notify_activity();

        if (!backlight_is_screen_on()) {
            // Screen was off — this tap's only job is to wake it.
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }

        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= SCR_W) x = SCR_W - 1;
        if (y >= SCR_H) y = SCR_H - 1;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void display_init()
{
    LOG_INFO("mcui: display_init() starting");

    // ---- LovyanGFX bringup ----
    gfx = new LGFX_ELECROW70();
    gfx->init();
    gfx->setRotation(landscape_active() ? 0 : 1);
    gfx->setSwapBytes(true);    // LVGL outputs native-endian RGB565; panel wants swapped
    gfx->setBrightness(255);
    gfx->fillScreen(TFT_BLACK); // clear the whole panel framebuffer

    LOG_INFO("mcui: LovyanGFX ready, logical %dx%d (%s)",
             (int)gfx->width(), (int)gfx->height(),
             landscape_active() ? "landscape" : "portrait");

    // ---- Flush worker on core 1 ----
    // The UI task (this one) runs on core 0 and does LVGL render. The worker
    // on core 1 does the rotated SRAM->PSRAM copy in parallel, so per-frame
    // wall time becomes max(render, copy) instead of render + copy.
    // Queue depth 1 matches LVGL's two-buffer flow (only one flush in flight).
    s_flush_q = xQueueCreate(1, sizeof(flush_msg_t));
    xTaskCreatePinnedToCore(flush_worker_task, "lcdflush", 4096,
                            nullptr, 2, nullptr, 1);

    // ---- LVGL core ----
    lv_init();
    lv_tick_set_cb(reinterpret_cast<lv_tick_get_cb_t>(millis));

    // ---- Draw buffers ----
    // Prefer internal SRAM (DMA-capable) over PSRAM. The RGB panel streams
    // its 768 KB framebuffer from PSRAM continuously at the pixel clock, so
    // PSRAM-resident draw buffers stall LVGL render and the flush copy on
    // every read and write. SRAM buffers eliminate that contention.
    //
    // 80 rows × SCR_W × 2 ≈ 77 KB per buffer, ~154 KB total — fits comfortably
    // in the S3's free internal heap. Falls back to bigger PSRAM buffers if
    // SRAM alloc fails (better than failing the boot).
    constexpr uint32_t BUF_LINES_SRAM  = 80;
    constexpr uint32_t BUF_LINES_PSRAM = 200;
    size_t bufBytes = (size_t)SCR_W * BUF_LINES_SRAM * sizeof(lv_color_t);

    buf1 = static_cast<lv_color_t *>(heap_caps_aligned_alloc(32, bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    buf2 = static_cast<lv_color_t *>(heap_caps_aligned_alloc(32, bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (!buf1 || !buf2) {
        LOG_WARN("mcui: internal SRAM draw buffer alloc failed (%u bytes each), falling back to PSRAM",
                 (unsigned)bufBytes);
        if (buf1) { heap_caps_free(buf1); buf1 = nullptr; }
        if (buf2) { heap_caps_free(buf2); buf2 = nullptr; }
        bufBytes = (size_t)SCR_W * BUF_LINES_PSRAM * sizeof(lv_color_t);
        buf1 = static_cast<lv_color_t *>(heap_caps_aligned_alloc(32, bufBytes, MALLOC_CAP_SPIRAM));
        buf2 = static_cast<lv_color_t *>(heap_caps_aligned_alloc(32, bufBytes, MALLOC_CAP_SPIRAM));
        if (!buf1 || !buf2) {
            LOG_ERROR("mcui: PSRAM draw buffer alloc also failed (%u bytes each)",
                      (unsigned)bufBytes);
            if (buf1) { heap_caps_free(buf1); buf1 = nullptr; }
            if (buf2) { heap_caps_free(buf2); buf2 = nullptr; }
            return;
        }
    }

    // ---- Display object ----
    // Create with the current logical dimensions.
    disp = lv_display_create(SCR_W, SCR_H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    // No lv_display_set_rotation() — the panel is already rotated at the
    // LovyanGFX level, so LVGL renders directly into the current logical space.

    // ---- Touch input device ----
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);
    lv_indev_set_display(indev, disp);
    // Long-press threshold is short enough for node context menus to feel
    // responsive, but still comfortably above normal tap/typing timing.
    // Keeps quick taps from ever entering long-press state — important for keyboards,
    // where LVGL's default 400 ms would fire LONG_PRESSED on any held key
    // and interact badly with auto-repeat.
    lv_indev_set_long_press_time(indev, 700);

    // Tune the display refresh timer to 20 ms period (50 Hz). LVGL's default
    // LV_DEF_REFR_PERIOD is 33 ms (~30 Hz) which feels sluggish on this
    // panel for typing-speed edits. Avoid pushing faster than the RGB panel
    // scanout; that causes visible tearing/ghosting on this hardware.
    lv_timer_t *refr_timer = lv_display_get_refr_timer(disp);
    if (refr_timer) {
        lv_timer_set_period(refr_timer, 20);
    }

    // Apply the persisted screen-sleep timeout from Meshtastic config to the
    // backlight task now that config is loaded. The backlight task started
    // earlier in initVariant() with the default 60 s timeout.
    {
        uint32_t secs = config.display.screen_on_secs;
        // Meshtastic semantics: 0 = default (60 s handled inside backlight),
        // UINT32_MAX = always on. Pass straight through.
        backlight_set_timeout_secs(secs);
    }

    LOG_INFO("mcui: LVGL ready, logical %dx%d %s",
             SCR_W, SCR_H, landscape_active() ? "landscape" : "portrait");
}

} // namespace mcui

#endif // HAS_TFT && USE_MCUI
