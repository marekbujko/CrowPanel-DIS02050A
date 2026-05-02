// =============================================================================
// crowpanel_backlight.h — public API for the CrowPanel backlight controller
//
// The .cpp owns a FreeRTOS task on core 1 that:
//   - Runs the soft-wake ramp (~1.5 s quadratic fade) when wake is requested
//   - Monitors an idle timer and switches the backlight off after it expires
//
// Clients (LVGL touch callback, incoming-message observer, settings UI) poke
// the backlight via the calls below. All are lock-free: the task reads
// `volatile` flags on each iteration.
//
// DESIGN NOTE — "why no GT911 polling here":
// The original implementation polled the GT911 directly from this task *and*
// cleared the touch-status register after reading it. LVGL's touch callback
// in McDisplay also reads the same register, and the race meant the first
// wake-tap was nearly always swallowed here before LVGL could see it — users
// had to tap 4-5 times before anything happened. The fix is to make LVGL the
// single reader and have it call backlight_notify_activity() on every press.
// =============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Record direct user activity such as touch input. Resets the idle timer.
// If the screen is currently asleep, schedules a soft wake on the backlight
// task. Safe to call from any thread / ISR-free context.
void backlight_notify_activity(void);

// Request a wake for passive external events such as a newly received
// message. Unlike backlight_notify_activity(), this does not refresh the
// idle timer while the screen is already on, so background traffic cannot
// keep the display awake indefinitely.
void backlight_wake_if_off(void);

// True when the backlight is currently on. Used by LVGL's touch_cb to
// decide whether a tap should also be delivered as a UI event or swallowed
// as a wake-only gesture.
bool backlight_is_screen_on(void);

// Configure the idle timeout in seconds. 0 selects the built-in default
// (30 s). UINT32_MAX disables auto-sleep. Typically called from the
// settings screen when the user changes the value, and from display_init()
// at boot to apply the persisted config.display.screen_on_secs.
void backlight_set_timeout_secs(uint32_t secs);

// Shared I2C bus mutex. Both the backlight controller (0x30, on core 1
// inside the backlight task) and the GT911 touch controller (0x5D, on
// core 0 inside LVGL's touch_cb via LovyanGFX) live on the same Wire
// bus. Without a common mutex, concurrent access from two cores corrupts
// the bus and can panic the device. Every Wire user that isn't already
// bracketed by this lock must call these around its transactions.
void backlight_i2c_lock(void);
void backlight_i2c_unlock(void);

#ifdef __cplusplus
}
#endif
