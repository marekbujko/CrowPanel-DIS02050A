#if HAS_TFT && USE_MCUI

#include "McUI.h"
#include "McDisplay.h"
#include "McTheme.h"
#include "McStatusBar.h"
#include "McTabBar.h"
#include "McKeyboard.h"
#include "screens/McScreens.h"
#include "screens/McChatView.h"
#include "data/McClock.h"
#include "data/McMessages.h"
#include "data/McNodeActions.h"
#include "data/McObserver.h"

#include "configuration.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

namespace mcui {

static TaskHandle_t s_task = nullptr;
static lv_obj_t *s_root = nullptr;
static lv_obj_t *s_page_host = nullptr;
static lv_obj_t *s_pages[4] = {nullptr, nullptr, nullptr, nullptr};
static int s_active_tab = -1;
static bool s_landscape = false;
static bool s_position_advert = true;
static bool s_prefs_loaded = false;

static constexpr const char *MCUI_PREF_NS = "meshtastic";
static constexpr const char *MCUI_PREF_KEY_LANDSCAPE = "mcuiLandscape";
static constexpr const char *MCUI_PREF_KEY_POSITION_ADVERT = "mcuiPosAdvert";

static void prefs_load_once()
{
    if (s_prefs_loaded)
        return;

    Preferences prefs;
    if (prefs.begin(MCUI_PREF_NS, true)) {
        s_landscape = prefs.getBool(MCUI_PREF_KEY_LANDSCAPE, false);
        s_position_advert = prefs.getBool(MCUI_PREF_KEY_POSITION_ADVERT, true);
        prefs.end();
    }
    s_prefs_loaded = true;
}

int screen_width()
{
    return s_landscape ? LANDSCAPE_SCR_W : PORTRAIT_SCR_W;
}

int screen_height()
{
    return s_landscape ? LANDSCAPE_SCR_H : PORTRAIT_SCR_H;
}

int page_height()
{
    return screen_height() - STATUS_H - TAB_H;
}

int keyboard_height()
{
    return s_landscape ? 180 : 280;
}

bool landscape_active()
{
    prefs_load_once();
    return s_landscape;
}

bool orientation_save(bool landscape)
{
    prefs_load_once();
    Preferences prefs;
    if (!prefs.begin(MCUI_PREF_NS, false)) {
        LOG_ERROR("mcui: failed to open preferences for orientation save");
        return false;
    }
    bool ok = prefs.putBool(MCUI_PREF_KEY_LANDSCAPE, landscape);
    prefs.end();
    if (ok)
        s_landscape = landscape;
    return ok;
}

bool position_advert_enabled()
{
    prefs_load_once();
    return s_position_advert;
}

bool position_advert_save(bool enabled)
{
    prefs_load_once();
    Preferences prefs;
    if (!prefs.begin(MCUI_PREF_NS, false)) {
        LOG_ERROR("mcui: failed to open preferences for position advert save");
        return false;
    }
    bool ok = prefs.putBool(MCUI_PREF_KEY_POSITION_ADVERT, enabled);
    prefs.end();
    if (ok)
        s_position_advert = enabled;
    return ok;
}

void switchTab(int idx)
{
    if (idx < 0 || idx >= 4) return;
    if (chatview_is_open())
        chatview_hide();
    if (s_active_tab == idx) return;
    for (int i = 0; i < 4; i++) {
        if (!s_pages[i]) continue;
        if (i == idx) {
            lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_active_tab = idx;
    tabbar_set_active(idx);
}

static void build_ui()
{
    // Root screen
    s_root = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // Top status bar
    statusbar_create(s_root);

    // Page host (fills the area between status bar and tab bar)
    s_page_host = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_page_host);
    lv_obj_set_size(s_page_host, SCR_W, PAGE_H);
    lv_obj_set_pos(s_page_host, 0, PAGE_Y);
    lv_obj_set_style_bg_color(s_page_host, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_page_host, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_page_host, LV_OBJ_FLAG_SCROLLABLE);

    s_pages[TAB_CHATS] = chats_screen_create(s_page_host);
    s_pages[TAB_NODES] = nodes_screen_create(s_page_host);
    s_pages[TAB_MAPS] = maps_screen_create(s_page_host);
    s_pages[TAB_SETTINGS] = settings_screen_create(s_page_host);

    // Hide all but Chats initially
    for (int i = 1; i < 4; i++) {
        if (s_pages[i]) lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Bottom tab bar
    tabbar_create(s_root);

    // Bubble chat view — lives at root level so its input row can float above
    // the keyboard (which covers the tab bar while visible). Starts hidden.
    chatview_create(s_root);

    // Keyboard (created hidden, flush with the bottom edge)
    keyboard_create(s_root);

    // Load the screen and mark Chats active
    lv_screen_load(s_root);
    s_active_tab = -1;
    switchTab(TAB_CHATS);
    settings_maybe_show_onboarding();
}

static void ui_task(void *)
{
    LOG_INFO("mcui: UI task started on core %d", xPortGetCoreID());

    // Bring up display + LVGL
    display_init();

    // Bring up data layer (message store + incoming text observer).
    messages_init();
    node_actions_init();
    observer_init();

    // Build the initial UI tree
    build_ui();

    LOG_INFO("mcui: UI built, entering main loop");

    uint32_t next_status_refresh = 0;
    uint32_t next_observer_attach_try = 0;
    uint32_t next_screen_tick = 0;
    uint32_t next_save_tick = 0;
    while (true) {
        lv_timer_handler();

        // observer_init() is idempotent after it successfully attaches.
        // The first few calls usually no-op because textMessageModule hasn't
        // been constructed yet (tftSetup runs before setupModules). We retry
        // every tick until attached.
        uint32_t now = millis();
        if ((int32_t)(now - next_observer_attach_try) >= 0) {
            observer_init();
            next_observer_attach_try = now + 250;
        }

        // Only tick the visible screen. Hidden screen rebuilds are surprisingly
        // expensive on RGB panels and do not help the radio recover after IRQs.
        if ((int32_t)(now - next_screen_tick) >= 0) {
            if (s_active_tab == TAB_CHATS) {
                chats_screen_tick();
            } else {
                if (chatview_is_open())
                    chatview_tick();
                if (s_active_tab == TAB_NODES)
                    nodes_screen_tick();
                else if (s_active_tab == TAB_MAPS)
                    maps_screen_tick();
                else if (s_active_tab == TAB_SETTINGS)
                    settings_screen_tick();
            }
            next_screen_tick = now + 16;
        }
        // Throttled flush of chat history to flash when dirty.
        if ((int32_t)(now - next_save_tick) >= 0) {
            messages_save_tick();
            next_save_tick = now + 200;
        }

        if ((int32_t)(now - next_status_refresh) >= 0) {
            statusbar_refresh();
            next_status_refresh = now + 1000;
        }

        vTaskDelay(pdMS_TO_TICKS(6));
    }
}

void setup()
{
    if (s_task) return;
    prefs_load_once();

    // Restore wall-clock time from the PCF8563 RTC at I2C 0x51 BEFORE we
    // spawn the UI task and BEFORE the mesh router / modules start getting
    // constructed in main.cpp — so the very first packet that gets
    // timestamped sees a sane UTC epoch. mcclock_init is synchronous and
    // cheap (one I2C probe + a few register reads).
    mcclock_init();

    LOG_INFO("mcui: spawning UI task");
    BaseType_t ok = xTaskCreatePinnedToCore(
        ui_task, "mcui", 12288, nullptr, 1, &s_task, 0);
    if (ok != pdPASS) {
        LOG_ERROR("mcui: failed to spawn UI task");
        s_task = nullptr;
    }
}

} // namespace mcui

#endif // HAS_TFT && USE_MCUI
