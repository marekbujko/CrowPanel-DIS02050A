// Display + touch bringup for mcui (LovyanGFX + LVGL 9 in portrait)
#pragma once
#if HAS_TFT && USE_MCUI

namespace mcui {
// Initialize LovyanGFX panel, LVGL core, draw buffers, and touch indev.
// Safe to call once from the mcui FreeRTOS task before building the UI.
void display_init();

// Put the CrowPanel display hardware into sleep and wake it back up.
// These are no-ops before display_init() completes.
void display_sleep_panel();
void display_wake_panel();
} // namespace mcui

#endif
