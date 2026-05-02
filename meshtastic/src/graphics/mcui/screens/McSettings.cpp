#if HAS_TFT && USE_MCUI

#include "McScreens.h"
#include "../McKeyboard.h"
#include "../McTheme.h"
#include "../McUI.h"
#include "../data/McNodeActions.h"

#include "configuration.h"
#include "crowpanel_backlight.h"
#include "main.h"
#include "memGet.h"
#include "concurrency/OSThread.h"
#include "mesh/NodeDB.h"
#include "mesh/Channels.h"
#include "mesh/CryptoEngine.h"
#include "mesh/MeshService.h"
#include "mesh/RadioInterface.h"
#include "mesh/Router.h"
#include "mesh/generated/meshtastic/config.pb.h"
#include "mesh/generated/meshtastic/module_config.pb.h"
#include "mesh/Default.h"

#include "PowerStatus.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cstdio>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace mcui {

using namespace meshtastic;

// ============================================================================
//  Deferred config-apply model
// ============================================================================
//
// UI handlers run on the mcui task (core 0). `nodeDB->saveToDisk()` and
// `service->reloadConfig()` must run on the main Arduino loop (core 1), so
// settings changes are debounced and applied by McConfigApplyThread:
//
//  1. The UI handler directly writes to `config.X.Y` (all primitive-field
//     writes; atomic on ESP32 for 8/16/32-bit aligned values). It then marks
//     either a reboot-required change or a save-only change.
//  2. `McConfigApplyThread` runs on the main loop. Once `APPLY_DEBOUNCE_MS`
//     has elapsed since the last mark-dirty call with no further changes,
//     it saves SEGMENT_CONFIG. Reboot-required changes also reload config and
//     schedule a reboot; save-only changes just persist to flash.
//
// The debounce lets users flip several switches in a row without the
// device rebooting between each one.
static volatile bool     s_config_dirty           = false;
static volatile bool     s_config_save_only_dirty = false;
static volatile bool     s_module_config_dirty    = false;
static volatile bool     s_orientation_dirty      = false;
static volatile bool     s_orientation_force_apply = false;
static volatile bool     s_factory_reset_req      = false;
static volatile bool     s_pending_landscape      = false;
static volatile uint32_t s_last_change_ms         = 0;
static constexpr uint32_t APPLY_DEBOUNCE_MS       = 5000;

static lv_obj_t *s_lbl_pending = nullptr; // "Saving in N s..." status label

static void cfg_show_pending_banner()
{
    if (s_lbl_pending) {
        lv_label_set_text(s_lbl_pending, "Changes pending — saving in 5 s...");
        lv_obj_remove_flag(s_lbl_pending, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cfg_mark_dirty()
{
    s_config_dirty   = true;
    s_last_change_ms = millis();
    cfg_show_pending_banner();
}

static void cfg_mark_save_only()
{
    s_config_save_only_dirty = true;
    s_last_change_ms = millis();
    cfg_show_pending_banner();
}

static void cfg_mark_module_dirty()
{
    s_module_config_dirty = true;
    s_last_change_ms = millis();
    cfg_show_pending_banner();
}

static void cfg_mark_orientation(bool landscape)
{
    if (!s_orientation_dirty && landscape == landscape_active())
        return;
    s_pending_landscape = landscape;
    s_orientation_dirty = (landscape != landscape_active());
    s_last_change_ms = millis();
    if (s_orientation_dirty)
        cfg_show_pending_banner();
}

static bool cfg_has_pending()
{
    return s_config_dirty || s_config_save_only_dirty || s_module_config_dirty || s_orientation_dirty;
}

class McConfigApplyThread : public concurrency::OSThread
{
  public:
    McConfigApplyThread() : concurrency::OSThread("McCfg") {}

  protected:
    int32_t runOnce() override
    {
        if (s_factory_reset_req) {
            s_factory_reset_req = false;
            LOG_WARN("mcui: applying factory reset on main loop");
            if (nodeDB) nodeDB->factoryReset();
            rebootAtMsec = millis() + 1500;
            return 500;
        }
        bool force_apply = s_orientation_force_apply;
        if (!cfg_has_pending()) {
            s_orientation_force_apply = false;
            return 500;
        }
        uint32_t now = millis();
        if (!force_apply && now - s_last_change_ms < APPLY_DEBOUNCE_MS) return 500;

        bool reboot_required = s_config_dirty;
        bool save_only = s_config_save_only_dirty;
        bool module_dirty = s_module_config_dirty;
        bool orientation_change = s_orientation_dirty;
        bool target_landscape = s_pending_landscape;
        s_orientation_force_apply = false;
        s_config_dirty = false;
        s_config_save_only_dirty = false;
        s_module_config_dirty = false;
        s_orientation_dirty = false;
        LOG_INFO("mcui: applying pending config (debounce elapsed, cfg_reboot=%d orientation=%d)",
                 reboot_required ? 1 : 0, orientation_change ? 1 : 0);
        if ((reboot_required || save_only) && nodeDB)
            nodeDB->saveToDisk(SEGMENT_CONFIG);
        if (module_dirty && nodeDB)
            nodeDB->saveToDisk(SEGMENT_MODULECONFIG);
        if (orientation_change && !orientation_save(target_landscape)) {
            LOG_WARN("mcui: orientation save failed, retrying later");
            s_pending_landscape = target_landscape;
            s_orientation_dirty = true;
            s_last_change_ms = millis();
            return 1000;
        }
        if (reboot_required) {
            if (service) service->reloadConfig(SEGMENT_CONFIG);
        }
        if (module_dirty) {
            if (service) service->reloadConfig(SEGMENT_MODULECONFIG);
        }
        if (reboot_required || orientation_change) {
            rebootAtMsec = millis() + 1500;
        }
        return 500;
    }
};

static McConfigApplyThread *s_cfg_thread = nullptr;
static void ensure_cfg_thread()
{
    if (!s_cfg_thread) s_cfg_thread = new McConfigApplyThread();
}

// ============================================================================
//  Deferred owner edit + WiFi scan (main-loop-only work)
// ============================================================================
//
// Owner edits (long_name / short_name) must be persisted via
// nodeDB->saveToDisk(SEGMENT_DEVICESTATE|SEGMENT_NODEDATABASE) and broadcast
// via service->reloadOwner() — both of which must run on the main Arduino
// loop (core 1), not on the mcui task (core 0).
//
// WiFi.scanNetworks() is a blocking call. Running it on the UI task would
// freeze the display for several seconds. Instead we queue a scan request
// and let an OSThread (McAuxThread) run it on the main loop.

struct PendingOwner {
    bool long_set;
    bool short_set;
    char long_name[40];
    char short_name[5];
};

static volatile bool s_owner_req        = false;
static volatile bool s_regen_keys_req   = false;
static PendingOwner  s_pending_owner    = {};
static SemaphoreHandle_t s_aux_lock     = nullptr;

// ---- WiFi scan result sharing ----
struct ScannedNetwork {
    char     ssid[33];
    int8_t   rssi;
    bool     encrypted;
};
static constexpr int SCAN_MAX = 20;
enum ScanState : uint8_t { SCAN_IDLE = 0, SCAN_REQUESTED, SCAN_RUNNING, SCAN_DONE, SCAN_FAIL };
static volatile ScanState s_scan_state  = SCAN_IDLE;
static ScannedNetwork s_scan_results[SCAN_MAX];
static int s_scan_count = 0;

static void aux_lock_init()
{
    if (!s_aux_lock) s_aux_lock = xSemaphoreCreateMutex();
}

// Queue an owner-edit apply; runs on the main loop. reloadOwner broadcasts
// the new nodeinfo to the mesh and persists devicestate.
static void queue_owner_edit(const char *longn, const char *shortn)
{
    aux_lock_init();
    xSemaphoreTake(s_aux_lock, portMAX_DELAY);
    if (longn) {
        s_pending_owner.long_set = true;
        strncpy(s_pending_owner.long_name, longn, sizeof(s_pending_owner.long_name) - 1);
        s_pending_owner.long_name[sizeof(s_pending_owner.long_name) - 1] = '\0';
    }
    if (shortn) {
        s_pending_owner.short_set = true;
        strncpy(s_pending_owner.short_name, shortn, sizeof(s_pending_owner.short_name) - 1);
        s_pending_owner.short_name[sizeof(s_pending_owner.short_name) - 1] = '\0';
    }
    s_owner_req = true;
    xSemaphoreGive(s_aux_lock);
}

static void queue_regenerate_private_keys()
{
    aux_lock_init();
    xSemaphoreTake(s_aux_lock, portMAX_DELAY);
    s_regen_keys_req = true;
    xSemaphoreGive(s_aux_lock);
}

class McAuxThread : public concurrency::OSThread
{
  public:
    McAuxThread() : concurrency::OSThread("McAux") {}

  protected:
    int32_t runOnce() override
    {
        // --- Owner edit ---
        if (s_owner_req) {
            aux_lock_init();
            PendingOwner po;
            xSemaphoreTake(s_aux_lock, portMAX_DELAY);
            po = s_pending_owner;
            s_pending_owner = {};
            s_owner_req = false;
            xSemaphoreGive(s_aux_lock);

            bool changed = false;
            if (po.long_set && po.long_name[0]) {
                if (strcmp(owner.long_name, po.long_name) != 0) {
                    strncpy(owner.long_name, po.long_name, sizeof(owner.long_name) - 1);
                    owner.long_name[sizeof(owner.long_name) - 1] = '\0';
                    changed = true;
                }
            }
            if (po.short_set && po.short_name[0]) {
                if (strcmp(owner.short_name, po.short_name) != 0) {
                    strncpy(owner.short_name, po.short_name, sizeof(owner.short_name) - 1);
                    owner.short_name[sizeof(owner.short_name) - 1] = '\0';
                    changed = true;
                }
            }
            if (changed) {
                LOG_INFO("mcui: applying owner edit (long=%s short=%s)",
                         owner.long_name, owner.short_name);
                if (service) service->reloadOwner(true);
                if (nodeDB) nodeDB->saveToDisk(SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE);
            }
        }

        // --- Private key regeneration ---
        bool regen_keys = false;
        aux_lock_init();
        xSemaphoreTake(s_aux_lock, portMAX_DELAY);
        if (s_regen_keys_req) {
            s_regen_keys_req = false;
            regen_keys = true;
        }
        xSemaphoreGive(s_aux_lock);

        if (regen_keys) {
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
            if (owner.is_licensed) {
                LOG_WARN("mcui: private key regeneration skipped in licensed/ham mode");
            } else if (!crypto) {
                LOG_WARN("mcui: private key regeneration skipped, crypto unavailable");
            } else {
                LOG_WARN("mcui: regenerating private keys");
                if (!config.has_security) {
                    config.has_security = true;
                    config.security = meshtastic_Config_SecurityConfig_init_default;
                    config.security.serial_enabled = config.device.serial_enabled;
                    config.security.is_managed = config.device.is_managed;
                }
                crypto->generateKeyPair(config.security.public_key.bytes,
                                        config.security.private_key.bytes);
                config.security.public_key.size = 32;
                config.security.private_key.size = 32;
                owner.public_key.size = 32;
                memcpy(owner.public_key.bytes, config.security.public_key.bytes, 32);
                crypto->setDHPrivateKey(config.security.private_key.bytes);
                if (service) service->reloadOwner(true);
                if (nodeDB)
                    nodeDB->saveToDisk(SEGMENT_CONFIG | SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE);
            }
#else
            LOG_WARN("mcui: private key regeneration unavailable in this build");
#endif
        }

        // --- WiFi scan ---
        if (s_scan_state == SCAN_REQUESTED) {
            s_scan_state = SCAN_RUNNING;
            LOG_INFO("mcui: starting WiFi scan");
            // Ensure station mode is available so scanNetworks returns
            // results even if WiFi is currently disabled in config.
            WiFi.mode(WIFI_STA);
            int n = WiFi.scanNetworks(/*async*/ false, /*hidden*/ false);
            if (n < 0) {
                LOG_WARN("mcui: WiFi scan failed (%d)", n);
                s_scan_count = 0;
                s_scan_state = SCAN_FAIL;
                return 500;
            }
            if (n > SCAN_MAX) n = SCAN_MAX;
            s_scan_count = 0;
            for (int i = 0; i < n; i++) {
                ScannedNetwork &r = s_scan_results[s_scan_count];
                String ssid = WiFi.SSID(i);
                if (ssid.length() == 0) continue;
                strncpy(r.ssid, ssid.c_str(), sizeof(r.ssid) - 1);
                r.ssid[sizeof(r.ssid) - 1] = '\0';
                r.rssi = (int8_t)WiFi.RSSI(i);
                r.encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
                s_scan_count++;
            }
            WiFi.scanDelete();
            LOG_INFO("mcui: WiFi scan found %d networks", s_scan_count);
            s_scan_state = SCAN_DONE;
        }
        return 300;
    }
};

static McAuxThread *s_aux_thread = nullptr;
static void ensure_aux_thread()
{
    if (!s_aux_thread) s_aux_thread = new McAuxThread();
}

// ============================================================================
//  Enum → label tables
// ============================================================================

struct EnumEntry {
    int         code;
    const char *label;
};

static const EnumEntry REGIONS[] = {
    {meshtastic_Config_LoRaConfig_RegionCode_UNSET,    "UNSET"},
    {meshtastic_Config_LoRaConfig_RegionCode_US,       "US"},
    {meshtastic_Config_LoRaConfig_RegionCode_EU_433,   "EU_433"},
    {meshtastic_Config_LoRaConfig_RegionCode_EU_868,   "EU_868"},
    {meshtastic_Config_LoRaConfig_RegionCode_CN,       "CN"},
    {meshtastic_Config_LoRaConfig_RegionCode_JP,       "JP"},
    {meshtastic_Config_LoRaConfig_RegionCode_ANZ,      "ANZ"},
    {meshtastic_Config_LoRaConfig_RegionCode_KR,       "KR"},
    {meshtastic_Config_LoRaConfig_RegionCode_TW,       "TW"},
    {meshtastic_Config_LoRaConfig_RegionCode_RU,       "RU"},
    {meshtastic_Config_LoRaConfig_RegionCode_IN,       "IN"},
    {meshtastic_Config_LoRaConfig_RegionCode_NZ_865,   "NZ_865"},
    {meshtastic_Config_LoRaConfig_RegionCode_TH,       "TH"},
    {meshtastic_Config_LoRaConfig_RegionCode_LORA_24,  "LORA_24"},
    {meshtastic_Config_LoRaConfig_RegionCode_UA_433,   "UA_433"},
    {meshtastic_Config_LoRaConfig_RegionCode_UA_868,   "UA_868"},
    {meshtastic_Config_LoRaConfig_RegionCode_MY_433,   "MY_433"},
    {meshtastic_Config_LoRaConfig_RegionCode_MY_919,   "MY_919"},
    {meshtastic_Config_LoRaConfig_RegionCode_SG_923,   "SG_923"},
};
static constexpr int REGION_COUNT = sizeof(REGIONS) / sizeof(REGIONS[0]);

static const EnumEntry PRESETS[] = {
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST,        "Long Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW,        "Long Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW,   "Very Long Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE,    "Long Moderate"},
    {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW,      "Medium Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST,      "Medium Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW,       "Short Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST,       "Short Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO,      "Short Turbo"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO,       "Long Turbo"},
};
static constexpr int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

static const EnumEntry ROLES[] = {
    {meshtastic_Config_DeviceConfig_Role_CLIENT,          "Client"},
    {meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE,     "Client Mute"},
    {meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN,   "Client Hidden"},
    {meshtastic_Config_DeviceConfig_Role_ROUTER,          "Router"},
    {meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT,   "Router Client"},
    {meshtastic_Config_DeviceConfig_Role_REPEATER,        "Repeater"},
    {meshtastic_Config_DeviceConfig_Role_TRACKER,         "Tracker"},
    {meshtastic_Config_DeviceConfig_Role_SENSOR,          "Sensor"},
    {meshtastic_Config_DeviceConfig_Role_TAK,             "TAK"},
    {meshtastic_Config_DeviceConfig_Role_TAK_TRACKER,     "TAK Tracker"},
    {meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND,  "Lost & Found"},
};
static constexpr int ROLE_COUNT = sizeof(ROLES) / sizeof(ROLES[0]);

static const EnumEntry REBROADCAST_MODES[] = {
    {meshtastic_Config_DeviceConfig_RebroadcastMode_ALL,                "All"},
    {meshtastic_Config_DeviceConfig_RebroadcastMode_ALL_SKIP_DECODING,  "All (skip decoding)"},
    {meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY,         "Local Only"},
    {meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY,         "Known Only"},
    {meshtastic_Config_DeviceConfig_RebroadcastMode_NONE,               "None"},
    {meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY, "Core Portnums"},
};
static constexpr int REBROADCAST_COUNT = sizeof(REBROADCAST_MODES) / sizeof(REBROADCAST_MODES[0]);

static const EnumEntry UNITS[] = {
    {meshtastic_Config_DisplayConfig_DisplayUnits_METRIC,   "Metric"},
    {meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL, "Imperial"},
};
static constexpr int UNITS_COUNT = sizeof(UNITS) / sizeof(UNITS[0]);

// LoRa hop limit. Meshtastic valid range is 1..7, with 3 being the default.
static const EnumEntry HOP_LIMITS[] = {
    {1, "1"},
    {2, "2"},
    {3, "3 (default)"},
    {4, "4"},
    {5, "5"},
    {6, "6"},
    {7, "7"},
};
static constexpr int HOP_LIMIT_COUNT = sizeof(HOP_LIMITS) / sizeof(HOP_LIMITS[0]);

// TX power in dBm. 0 selects the region/radio default (which is also the
// safest bet — using anything over the region's legal max can burn the
// radio). Entries beyond 22 dBm are only legal in a few regions and may
// be clamped by the radio driver at init time.
static const EnumEntry TX_POWERS[] = {
    {0,  "Region default"},
    {5,  "5 dBm"},
    {10, "10 dBm"},
    {14, "14 dBm"},
    {17, "17 dBm"},
    {20, "20 dBm (default)"},
    {22, "22 dBm"},
};
static constexpr int TX_POWER_COUNT = sizeof(TX_POWERS) / sizeof(TX_POWERS[0]);

// Interval choices (in seconds) for broadcast periods.
struct IntervalEntry {
    uint32_t    secs;
    const char *label;
};
static const IntervalEntry INTERVAL_SHORT[] = {
    {60,     "1 min"},
    {300,    "5 min"},
    {900,    "15 min"},
    {1800,   "30 min"},
    {3600,   "1 hour"},
    {10800,  "3 hours"},
    {21600,  "6 hours"},
    {43200,  "12 hours"},
    {86400,  "24 hours"},
};
static constexpr int INTERVAL_SHORT_COUNT = sizeof(INTERVAL_SHORT) / sizeof(INTERVAL_SHORT[0]);

// Screen on timeout choices (0 == default, UINT32_MAX == always).
static const IntervalEntry SCREEN_TIMEOUT[] = {
    {0,          "Default"},
    {15,         "15 s"},
    {30,         "30 s"},
    {60,         "1 min"},
    {300,        "5 min"},
    {900,        "15 min"},
    {3600,       "1 hour"},
    {0xFFFFFFFF, "Always"},
};
static constexpr int SCREEN_TIMEOUT_COUNT = sizeof(SCREEN_TIMEOUT) / sizeof(SCREEN_TIMEOUT[0]);

// Helpers — look up a label for a code, and find the index that matches a
// code or secs value. Returns 0 if not found.
static const char *enum_label(const EnumEntry *table, int n, int code)
{
    for (int i = 0; i < n; i++) if (table[i].code == code) return table[i].label;
    return "?";
}
static int enum_index(const EnumEntry *table, int n, int code)
{
    for (int i = 0; i < n; i++) if (table[i].code == code) return i;
    return 0;
}
static int interval_index(const IntervalEntry *table, int n, uint32_t secs)
{
    // Exact match first
    for (int i = 0; i < n; i++) if (table[i].secs == secs) return i;
    // Nearest-higher fallback
    for (int i = 0; i < n; i++) if (table[i].secs >= secs) return i;
    return n - 1;
}

static void build_dropdown_options(char *out, size_t out_sz,
                                   const char *const *labels, int n)
{
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(labels[i]);
        if (used + len + 2 > out_sz) break;
        if (i) out[used++] = '\n';
        memcpy(out + used, labels[i], len);
        used += len;
        out[used] = '\0';
    }
}

// ============================================================================
//  Row builder helpers (factor out the repetitive "label + control" layout)
// ============================================================================

static lv_obj_t *s_page = nullptr;
static lv_obj_t *s_list = nullptr;

// About-section live labels
static lv_obj_t *s_lbl_firmware    = nullptr;
static lv_obj_t *s_lbl_owner       = nullptr;
static lv_obj_t *s_lbl_nodeid      = nullptr;
static lv_obj_t *s_lbl_region_now  = nullptr;
static lv_obj_t *s_lbl_preset_now  = nullptr;
static lv_obj_t *s_lbl_uptime      = nullptr;
static lv_obj_t *s_lbl_heap        = nullptr;
static lv_obj_t *s_lbl_battery     = nullptr;
static lv_obj_t *s_lbl_noise       = nullptr;
static lv_obj_t *s_lbl_wifi_ssid   = nullptr;
static lv_obj_t *s_lbl_orientation = nullptr;
static lv_obj_t *s_btn_orientation = nullptr;
static lv_obj_t *s_btn_orientation_label = nullptr;
static lv_obj_t *s_orientation_overlay = nullptr;
static lv_obj_t *s_orientation_overlay_label = nullptr;
// Tappable owner rows — refreshed whenever owner.long_name/short_name change.
static lv_obj_t *s_lbl_owner_long  = nullptr;
static lv_obj_t *s_lbl_owner_short = nullptr;

// Hop/tx value labels on the slider rows
static uint32_t s_last_refresh_ms = 0;
static uint32_t s_orientation_reboot_at_ms = 0;
static uint32_t s_orientation_last_countdown = UINT32_MAX;

static void refresh_values();

static void orientation_overlay_close()
{
    if (s_orientation_overlay) {
        lv_obj_delete(s_orientation_overlay);
        s_orientation_overlay = nullptr;
    }
    s_orientation_overlay_label = nullptr;
    s_orientation_reboot_at_ms = 0;
    s_orientation_last_countdown = UINT32_MAX;
}

static void orientation_overlay_update()
{
    if (!s_orientation_overlay || !s_orientation_overlay_label || !s_orientation_reboot_at_ms)
        return;

    uint32_t now = millis();
    uint32_t secs = 0;
    if (now < s_orientation_reboot_at_ms)
        secs = (s_orientation_reboot_at_ms - now + 999) / 1000;

    if (secs != s_orientation_last_countdown) {
        char buf[32];
        if (secs > 0)
            snprintf(buf, sizeof(buf), "Reboot in %u...", (unsigned)secs);
        else
            snprintf(buf, sizeof(buf), "Rebooting...");
        lv_label_set_text(s_orientation_overlay_label, buf);
        s_orientation_last_countdown = secs;
    }

    if (now >= s_orientation_reboot_at_ms) {
        s_orientation_force_apply = true;
        orientation_overlay_close();
    }
}

static void orientation_overlay_open()
{
    orientation_overlay_close();

    lv_obj_t *scr = lv_screen_active();
    s_orientation_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_orientation_overlay);
    lv_obj_set_size(s_orientation_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_orientation_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_orientation_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_orientation_overlay, LV_OPA_70, 0);
    lv_obj_remove_flag(s_orientation_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_orientation_overlay);

    lv_obj_t *card = lv_obj_create(s_orientation_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W > 460 ? 420 : SCR_W - 40, 150);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Changing orientation");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    s_orientation_overlay_label = lv_label_create(card);
    lv_label_set_text(s_orientation_overlay_label, "Reboot in 3...");
    lv_obj_set_style_text_color(s_orientation_overlay_label, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(s_orientation_overlay_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_orientation_overlay_label, LV_ALIGN_CENTER, 0, 20);

    s_orientation_reboot_at_ms = millis() + 3000;
    s_orientation_last_countdown = UINT32_MAX;
    orientation_overlay_update();
}

static void add_section_header(const char *text)
{
    lv_obj_t *h = lv_label_create(s_list);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_color(h, lv_color_hex(TH_ACCENT_LIGHT), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(h, 10, 0);
    lv_obj_set_style_pad_left(h, 6, 0);
}

static lv_obj_t *add_card()
{
    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

// Info row: "Label: value" (read-only)
static lv_obj_t *add_info_row(lv_obj_t *card, const char *label, const char *initial)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 24);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, initial);
    lv_obj_set_style_text_color(val, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
    return val;
}

// Toggle (lv_switch) row
static lv_obj_t *add_switch_row(lv_obj_t *card, const char *label, bool initial,
                                lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 56, 28);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (initial) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return sw;
}

// Dropdown row — builds options from an EnumEntry table.
static lv_obj_t *add_enum_dropdown_row(lv_obj_t *card, const char *label,
                                       const EnumEntry *table, int n,
                                       int current_code, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 52);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *dd = lv_dropdown_create(row);
    const char *labels[32];
    int m = (n > 32) ? 32 : n;
    for (int i = 0; i < m; i++) labels[i] = table[i].label;
    char opts[512];
    build_dropdown_options(opts, sizeof(opts), labels, m);
    lv_dropdown_set_options(dd, opts);
    lv_obj_set_width(dd, 190);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_dropdown_set_selected(dd, (uint16_t)enum_index(table, n, current_code));
    if (cb) lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return dd;
}

// Dropdown row — builds options from an IntervalEntry table.
static lv_obj_t *add_interval_dropdown_row(lv_obj_t *card, const char *label,
                                           const IntervalEntry *table, int n,
                                           uint32_t current_secs, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 52);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *dd = lv_dropdown_create(row);
    const char *labels[16];
    int m = (n > 16) ? 16 : n;
    for (int i = 0; i < m; i++) labels[i] = table[i].label;
    char opts[256];
    build_dropdown_options(opts, sizeof(opts), labels, m);
    lv_dropdown_set_options(dd, opts);
    lv_obj_set_width(dd, 190);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_dropdown_set_selected(dd, (uint16_t)interval_index(table, n, current_secs));
    if (cb) lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return dd;
}

static void add_card_hint(lv_obj_t *card, const char *text)
{
    lv_obj_t *h = lv_label_create(card);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_color(h, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, lv_pct(100));
}

static int modal_card_height(int desired, int vertical_margin = 40)
{
    int max_h = SCR_H - vertical_margin;
    return (desired < max_h) ? desired : max_h;
}

static int modal_card_y(int card_h, int top_margin = 20)
{
    int y = (SCR_H - card_h) / 2;
    return (y < top_margin) ? top_margin : y;
}

// ============================================================================
//  Reusable text-edit modal
// ============================================================================
//
// A centred card with a single textarea, OK/Cancel buttons, and the shared
// McKeyboard docked at the bottom. When the user taps OK the registered
// callback is invoked with the new text, then the modal dismisses; Cancel
// just dismisses. Only one modal may be open at a time.

typedef void (*TextEditCb)(const char *new_text);

static lv_obj_t *s_tem_overlay = nullptr;
static lv_obj_t *s_tem_ta      = nullptr;
static TextEditCb s_tem_cb     = nullptr;

static void tem_close()
{
    keyboard_hide();
    if (s_tem_overlay) {
        lv_obj_delete(s_tem_overlay);
        s_tem_overlay = nullptr;
        s_tem_ta      = nullptr;
    }
    s_tem_cb = nullptr;
}

static void tem_ok_cb(lv_event_t *)
{
    if (s_tem_ta && s_tem_cb) {
        const char *t = lv_textarea_get_text(s_tem_ta);
        TextEditCb cb = s_tem_cb;
        // Clear before firing so the callback can open a *new* modal
        // (e.g. SSID picked → password prompt) without us tearing it down.
        s_tem_cb = nullptr;
        char copy[129];
        strncpy(copy, t ? t : "", sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        tem_close();
        cb(copy);
        return;
    }
    tem_close();
}

static void tem_cancel_cb(lv_event_t *) { tem_close(); }

// Build the text edit modal. Called from an LVGL event handler — safe because
// lv_obj_delete is only called inside other LVGL event handlers of ours, not
// mid-press on a child of the object being deleted.
static void text_edit_modal(const char *title, const char *current,
                            int max_len, bool password, TextEditCb cb)
{
    if (s_tem_overlay) tem_close();
    s_tem_cb = cb;

    lv_obj_t *scr = lv_screen_active();
    s_tem_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_tem_overlay);
    lv_obj_set_size(s_tem_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_tem_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_tem_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tem_overlay, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_tem_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_tem_overlay);

    // Card sits above the keyboard. Keyboard covers
    // [SCR_H-keyboard_height(), SCR_H] when
    // visible, so anchor the card a little higher than that.
    lv_obj_t *card = lv_obj_create(s_tem_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 40, 220);
    lv_obj_set_pos(card, 20, SCR_H - keyboard_height() - 240);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    s_tem_ta = lv_textarea_create(card);
    lv_obj_set_size(s_tem_ta, lv_pct(100), 52);
    lv_obj_align(s_tem_ta, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_textarea_set_one_line(s_tem_ta, true);
    lv_textarea_set_text(s_tem_ta, current ? current : "");
    lv_textarea_set_max_length(s_tem_ta, max_len);
    lv_obj_set_style_bg_color(s_tem_ta, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(s_tem_ta, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(s_tem_ta, 0, 0);
    lv_obj_set_style_radius(s_tem_ta, 0, 0);
    lv_obj_set_style_anim_duration(s_tem_ta, 0, LV_PART_CURSOR);
    if (password) lv_textarea_set_password_mode(s_tem_ta, true);

    // Cancel / OK
    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 130, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, tem_cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);

    lv_obj_t *ok = lv_button_create(card);
    lv_obj_set_size(ok, 130, 42);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(ok, 0, 0);
    lv_obj_add_event_cb(ok, tem_ok_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, "OK");
    lv_obj_set_style_text_color(ol, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(ol, &lv_font_montserrat_16, 0);
    lv_obj_center(ol);

    // Attach the shared keyboard to this textarea and show it.
    keyboard_attach(s_tem_ta);
    keyboard_show();
    lv_textarea_set_cursor_pos(s_tem_ta, LV_TEXTAREA_CURSOR_LAST);
}

// ============================================================================
//  First-boot setup modal
// ============================================================================

static lv_obj_t *s_onboarding_overlay = nullptr;
static lv_obj_t *s_onboarding_long    = nullptr;
static lv_obj_t *s_onboarding_short   = nullptr;
static lv_obj_t *s_onboarding_region  = nullptr;
static lv_obj_t *s_onboarding_status  = nullptr;
static bool      s_onboarding_shown   = false;

static bool onboarding_needed()
{
    return config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET ||
           owner.long_name[0] == '\0' ||
           owner.short_name[0] == '\0';
}

static void onboarding_close()
{
    keyboard_hide();
    if (s_onboarding_overlay) {
        lv_obj_delete(s_onboarding_overlay);
        s_onboarding_overlay = nullptr;
        s_onboarding_long    = nullptr;
        s_onboarding_short   = nullptr;
        s_onboarding_region  = nullptr;
        s_onboarding_status  = nullptr;
    }
}

static void onboarding_later_cb(lv_event_t *)
{
    s_onboarding_shown = true;
    onboarding_close();
}

static void onboarding_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    keyboard_attach(ta);
    keyboard_show();
}

static void onboarding_dismiss_kb_cb(lv_event_t *e)
{
    lv_obj_t *tgt = (lv_obj_t *)lv_event_get_target(e);
    if (tgt && lv_obj_check_type(tgt, &lv_textarea_class)) return;
    keyboard_hide();
}

static lv_obj_t *onboarding_label(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, y);
    return label;
}

static lv_obj_t *onboarding_textarea(lv_obj_t *parent, const char *text,
                                     int max_len, int y)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, lv_pct(100), 50);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, y);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, text ? text : "");
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_style_bg_color(ta, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 0, 0);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    lv_obj_add_event_cb(ta, onboarding_focus_cb, LV_EVENT_FOCUSED, nullptr);
    return ta;
}

static void onboarding_save_cb(lv_event_t *e)
{
    const char *longn = s_onboarding_long ? lv_textarea_get_text(s_onboarding_long) : "";
    const char *shortn = s_onboarding_short ? lv_textarea_get_text(s_onboarding_short) : "";
    uint16_t idx = s_onboarding_region ? lv_dropdown_get_selected(s_onboarding_region) : 0;
    if (!longn || !*longn || !shortn || !*shortn) {
        if (s_onboarding_status)
            lv_label_set_text(s_onboarding_status, "Please enter both names.");
        return;
    }
    if (idx >= REGION_COUNT ||
        REGIONS[idx].code == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
        if (s_onboarding_status)
            lv_label_set_text(s_onboarding_status, "Please select your radio region.");
        return;
    }

    ensure_aux_thread();
    queue_owner_edit(longn, shortn);
    config.lora.region = (meshtastic_Config_LoRaConfig_RegionCode)REGIONS[idx].code;
    cfg_mark_dirty();

    lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
    if (btn) lv_obj_add_state(btn, LV_STATE_DISABLED);
    if (s_onboarding_status)
        lv_label_set_text(s_onboarding_status, "Saving setup... rebooting soon.");
    s_onboarding_shown = true;
}

void settings_maybe_show_onboarding()
{
    if (s_onboarding_shown || s_onboarding_overlay || !onboarding_needed())
        return;

    s_onboarding_shown = true;
    lv_obj_t *scr = lv_screen_active();
    s_onboarding_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_onboarding_overlay);
    lv_obj_set_size(s_onboarding_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_onboarding_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_onboarding_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_onboarding_overlay, LV_OPA_70, 0);
    lv_obj_remove_flag(s_onboarding_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_onboarding_overlay);
    lv_obj_add_flag(s_onboarding_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_onboarding_overlay, onboarding_dismiss_kb_cb,
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_onboarding_overlay, onboarding_dismiss_kb_cb,
                        LV_EVENT_PRESSED, nullptr);

    int onboarding_card_h = modal_card_height(470);
    lv_obj_t *card = lv_obj_create(s_onboarding_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 40, onboarding_card_h);
    lv_obj_set_pos(card, 20, modal_card_y(onboarding_card_h));
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, onboarding_dismiss_kb_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(card, onboarding_dismiss_kb_cb, LV_EVENT_PRESSED, nullptr);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "First setup");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Choose names and a legal LoRa region before using the mesh.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_color(hint, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 32);

    onboarding_label(card, "Long name", 84);
    s_onboarding_long = onboarding_textarea(card, owner.long_name, 39, 110);

    onboarding_label(card, "Short name", 172);
    s_onboarding_short = onboarding_textarea(card, owner.short_name, 4, 198);

    onboarding_label(card, "Region", 260);
    s_onboarding_region = lv_dropdown_create(card);
    const char *labels[32];
    int m = (REGION_COUNT > 32) ? 32 : REGION_COUNT;
    for (int i = 0; i < m; i++) labels[i] = REGIONS[i].label;
    char opts[512];
    build_dropdown_options(opts, sizeof(opts), labels, m);
    lv_dropdown_set_options(s_onboarding_region, opts);
    lv_dropdown_set_selected(s_onboarding_region,
                             (uint16_t)enum_index(REGIONS, REGION_COUNT,
                                                  config.lora.region));
    lv_obj_set_size(s_onboarding_region, lv_pct(100), 50);
    lv_obj_align(s_onboarding_region, LV_ALIGN_TOP_LEFT, 0, 286);
    lv_obj_set_style_bg_color(s_onboarding_region, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(s_onboarding_region, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(s_onboarding_region, 0, 0);
    lv_obj_set_style_radius(s_onboarding_region, 0, 0);

    s_onboarding_status = lv_label_create(card);
    lv_label_set_text(s_onboarding_status, "");
    lv_label_set_long_mode(s_onboarding_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_onboarding_status, lv_pct(100));
    lv_obj_set_style_text_color(s_onboarding_status, lv_color_hex(0xE0A030), 0);
    lv_obj_set_style_text_font(s_onboarding_status, &lv_font_montserrat_16, 0);
    lv_obj_align(s_onboarding_status, LV_ALIGN_TOP_LEFT, 0, 348);

    lv_obj_t *later = lv_button_create(card);
    lv_obj_set_size(later, 130, 42);
    lv_obj_align(later, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(later, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(later, 0, 0);
    lv_obj_add_event_cb(later, onboarding_later_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ll = lv_label_create(later);
    lv_label_set_text(ll, "Later");
    lv_obj_set_style_text_color(ll, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(ll, &lv_font_montserrat_16, 0);
    lv_obj_center(ll);

    lv_obj_t *save = lv_button_create(card);
    lv_obj_set_size(save, 160, 42);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(save, 0, 0);
    lv_obj_add_event_cb(save, onboarding_save_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "Save & reboot");
    lv_obj_set_style_text_color(sl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
    lv_obj_center(sl);

    keyboard_attach(s_onboarding_long);
    keyboard_show();
    lv_textarea_set_cursor_pos(s_onboarding_long, LV_TEXTAREA_CURSOR_LAST);
}

// ============================================================================
//  WiFi scan / connect modal
// ============================================================================

static lv_obj_t *s_wifi_overlay = nullptr;
static lv_obj_t *s_wifi_list    = nullptr;
static lv_obj_t *s_wifi_status  = nullptr;
static uint32_t  s_wifi_poll_ms = 0;
// SSID held between "pick network" and "enter password" steps
static char      s_wifi_pending_ssid[33] = {0};
static bool      s_wifi_pending_encrypted = false;

static void wifi_modal_close()
{
    if (s_wifi_overlay) {
        lv_obj_delete(s_wifi_overlay);
        s_wifi_overlay = nullptr;
        s_wifi_list    = nullptr;
        s_wifi_status  = nullptr;
    }
}

static void wifi_modal_cancel_cb(lv_event_t *) { wifi_modal_close(); }

// Apply the chosen SSID + PSK and schedule a save/reboot.
static void wifi_apply(const char *ssid, const char *psk)
{
    strncpy(config.network.wifi_ssid, ssid,
            sizeof(config.network.wifi_ssid) - 1);
    config.network.wifi_ssid[sizeof(config.network.wifi_ssid) - 1] = '\0';
    if (psk) {
        strncpy(config.network.wifi_psk, psk,
                sizeof(config.network.wifi_psk) - 1);
        config.network.wifi_psk[sizeof(config.network.wifi_psk) - 1] = '\0';
    } else {
        config.network.wifi_psk[0] = '\0';
    }
    config.network.wifi_enabled = true;
    LOG_INFO("mcui: WiFi set to SSID=%s (reboot pending)", ssid);
    cfg_mark_dirty();
}

// User finished typing the password — apply and close.
static void wifi_password_entered(const char *pw)
{
    wifi_apply(s_wifi_pending_ssid, pw);
    wifi_modal_close();
}

// User picked an SSID from the list: open the password editor if encrypted,
// otherwise apply immediately with an empty PSK.
static void wifi_pick_btn_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_scan_count) return;

    const ScannedNetwork &n = s_scan_results[idx];
    strncpy(s_wifi_pending_ssid, n.ssid, sizeof(s_wifi_pending_ssid) - 1);
    s_wifi_pending_ssid[sizeof(s_wifi_pending_ssid) - 1] = '\0';
    s_wifi_pending_encrypted = n.encrypted;

    wifi_modal_close();

    if (n.encrypted) {
        char title[64];
        snprintf(title, sizeof(title), "Password for %s", n.ssid);
        // wifi_psk is up to 64 chars in protobuf.
        text_edit_modal(title, "", 64, /*password*/ true, wifi_password_entered);
    } else {
        wifi_apply(s_wifi_pending_ssid, "");
    }
}

static void wifi_populate_list()
{
    if (!s_wifi_list) return;
    // Clear existing rows (except nothing — list is a fresh obj each open).
    lv_obj_clean(s_wifi_list);

    if (s_scan_count == 0) {
        lv_obj_t *l = lv_label_create(s_wifi_list);
        lv_label_set_text(l, "No networks found.");
        lv_obj_set_style_text_color(l, lv_color_hex(TH_TEXT3), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        return;
    }

    for (int i = 0; i < s_scan_count; i++) {
        const ScannedNetwork &n = s_scan_results[i];
        lv_obj_t *row = lv_button_create(s_wifi_list);
        lv_obj_set_size(row, lv_pct(100), 46);
        lv_obj_set_style_bg_color(row, lv_color_hex(TH_INPUT), 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 8, 0);

        lv_obj_t *lbl = lv_label_create(row);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%s  %d dBm",
                 n.ssid, n.encrypted ? " " LV_SYMBOL_SETTINGS : "",
                 (int)n.rssi);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_add_event_cb(row, wifi_pick_btn_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
}

// Called from settings_screen_tick while the modal is up — watches the scan
// state and populates the list when the scan completes.
static void wifi_modal_tick()
{
    if (!s_wifi_overlay) return;
    uint32_t now = millis();
    if (now - s_wifi_poll_ms < 200) return;
    s_wifi_poll_ms = now;

    if (s_scan_state == SCAN_RUNNING || s_scan_state == SCAN_REQUESTED) {
        if (s_wifi_status)
            lv_label_set_text(s_wifi_status, "Scanning...");
    } else if (s_scan_state == SCAN_FAIL) {
        if (s_wifi_status)
            lv_label_set_text(s_wifi_status, "Scan failed.");
        s_scan_state = SCAN_IDLE;
    } else if (s_scan_state == SCAN_DONE) {
        if (s_wifi_status) {
            char buf[48];
            snprintf(buf, sizeof(buf), "Found %d networks.", s_scan_count);
            lv_label_set_text(s_wifi_status, buf);
        }
        wifi_populate_list();
        s_scan_state = SCAN_IDLE;
    }
}

static void wifi_scan_clicked_cb(lv_event_t *)
{
    ensure_aux_thread();
    if (s_wifi_overlay) return; // already open

    lv_obj_t *scr = lv_screen_active();
    s_wifi_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_wifi_overlay);
    lv_obj_set_size(s_wifi_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_wifi_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wifi_overlay, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_wifi_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_wifi_overlay);

    int wifi_card_h = modal_card_height(600);
    lv_obj_t *card = lv_obj_create(s_wifi_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 40, wifi_card_h);
    lv_obj_set_pos(card, 20, modal_card_y(wifi_card_h));
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "WiFi networks");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_wifi_status = lv_label_create(card);
    lv_label_set_text(s_wifi_status, "Scanning...");
    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(s_wifi_status, &lv_font_montserrat_16, 0);
    lv_obj_align(s_wifi_status, LV_ALIGN_TOP_LEFT, 0, 28);

    s_wifi_list = lv_obj_create(card);
    lv_obj_remove_style_all(s_wifi_list);
    int wifi_list_h = wifi_card_h - 56 - 42 - 28;
    if (wifi_list_h < 160) wifi_list_h = 160;
    lv_obj_set_size(s_wifi_list, lv_pct(100), wifi_list_h);
    lv_obj_align(s_wifi_list, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(s_wifi_list, 4, 0);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(s_wifi_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    // Cancel button at bottom
    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 130, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, wifi_modal_cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);

    // Kick off the scan
    s_scan_state = SCAN_REQUESTED;
    s_wifi_poll_ms = 0;
}

// ---- MQTT modal ----
static lv_obj_t *s_mqtt_overlay = nullptr;
static lv_obj_t *s_mqtt_address = nullptr;
static lv_obj_t *s_mqtt_username = nullptr;
static lv_obj_t *s_mqtt_password = nullptr;
static lv_obj_t *s_mqtt_root = nullptr;
static lv_obj_t *s_mqtt_enabled = nullptr;
static lv_obj_t *s_mqtt_encrypt = nullptr;
static lv_obj_t *s_mqtt_json = nullptr;
static lv_obj_t *s_mqtt_tls = nullptr;
static lv_obj_t *s_mqtt_ch0_uplink = nullptr;
static lv_obj_t *s_mqtt_ch0_downlink = nullptr;

static void mqtt_dismiss_kb_cb(lv_event_t *e)
{
    lv_obj_t *tgt = (lv_obj_t *)lv_event_get_target(e);
    if (tgt && lv_obj_check_type(tgt, &lv_textarea_class)) return;
    keyboard_hide();
}

static void mqtt_modal_close()
{
    keyboard_hide();
    if (s_mqtt_overlay) {
        lv_obj_delete(s_mqtt_overlay);
        s_mqtt_overlay = nullptr;
    }
    s_mqtt_address = nullptr;
    s_mqtt_username = nullptr;
    s_mqtt_password = nullptr;
    s_mqtt_root = nullptr;
    s_mqtt_enabled = nullptr;
    s_mqtt_encrypt = nullptr;
    s_mqtt_json = nullptr;
    s_mqtt_tls = nullptr;
    s_mqtt_ch0_uplink = nullptr;
    s_mqtt_ch0_downlink = nullptr;
}

static void mqtt_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_current_target(e);
    keyboard_attach(ta);
    keyboard_show();
}

static lv_obj_t *mqtt_add_switch_row(lv_obj_t *parent, const char *label, bool initial)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 38);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 56, 28);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (initial) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

static lv_obj_t *mqtt_add_text_field(lv_obj_t *parent, const char *label, const char *value, int max_len, bool password)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);

    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, lv_pct(100), 44);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, value ? value : "");
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_style_bg_color(ta, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 0, 0);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    if (password) lv_textarea_set_password_mode(ta, true);
    lv_obj_add_event_cb(ta, mqtt_focus_cb, LV_EVENT_FOCUSED, nullptr);
    return ta;
}

static void mqtt_modal_save_cb(lv_event_t *)
{
    const char *addr = s_mqtt_address ? lv_textarea_get_text(s_mqtt_address) : "";
    const char *user = s_mqtt_username ? lv_textarea_get_text(s_mqtt_username) : "";
    const char *pass = s_mqtt_password ? lv_textarea_get_text(s_mqtt_password) : "";
    const char *root = s_mqtt_root ? lv_textarea_get_text(s_mqtt_root) : "";

    moduleConfig.mqtt.enabled = s_mqtt_enabled && lv_obj_has_state(s_mqtt_enabled, LV_STATE_CHECKED);
    moduleConfig.mqtt.encryption_enabled = s_mqtt_encrypt && lv_obj_has_state(s_mqtt_encrypt, LV_STATE_CHECKED);
    moduleConfig.mqtt.json_enabled = s_mqtt_json && lv_obj_has_state(s_mqtt_json, LV_STATE_CHECKED);
    moduleConfig.mqtt.tls_enabled = s_mqtt_tls && lv_obj_has_state(s_mqtt_tls, LV_STATE_CHECKED);

    strncpy(moduleConfig.mqtt.address, addr && addr[0] ? addr : default_mqtt_address,
            sizeof(moduleConfig.mqtt.address) - 1);
    moduleConfig.mqtt.address[sizeof(moduleConfig.mqtt.address) - 1] = '\0';
    strncpy(moduleConfig.mqtt.username, user && user[0] ? user : default_mqtt_username,
            sizeof(moduleConfig.mqtt.username) - 1);
    moduleConfig.mqtt.username[sizeof(moduleConfig.mqtt.username) - 1] = '\0';
    strncpy(moduleConfig.mqtt.password, pass && pass[0] ? pass : default_mqtt_password,
            sizeof(moduleConfig.mqtt.password) - 1);
    moduleConfig.mqtt.password[sizeof(moduleConfig.mqtt.password) - 1] = '\0';
    strncpy(moduleConfig.mqtt.root, root ? root : "",
            sizeof(moduleConfig.mqtt.root) - 1);
    moduleConfig.mqtt.root[sizeof(moduleConfig.mqtt.root) - 1] = '\0';

    // Channel 0 (primary/LongFast) MQTT bridge flags.
    uint8_t primary = channels.getPrimaryIndex();
    meshtastic_Channel ch0 = channels.getByIndex(primary);
    ch0.settings.uplink_enabled = s_mqtt_ch0_uplink && lv_obj_has_state(s_mqtt_ch0_uplink, LV_STATE_CHECKED);
    ch0.settings.downlink_enabled = s_mqtt_ch0_downlink && lv_obj_has_state(s_mqtt_ch0_downlink, LV_STATE_CHECKED);
    channels.setChannel(ch0);
    channels.onConfigChanged();
    if (service)
        service->reloadConfig(SEGMENT_CHANNELS);
    else if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CHANNELS);

    cfg_mark_module_dirty();
    mqtt_modal_close();
}

static void mqtt_modal_open_cb(lv_event_t *)
{
    if (s_mqtt_overlay) return;

    lv_obj_t *scr = lv_screen_active();
    s_mqtt_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_mqtt_overlay);
    lv_obj_set_size(s_mqtt_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_mqtt_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_mqtt_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_mqtt_overlay, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_mqtt_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_mqtt_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mqtt_overlay, mqtt_dismiss_kb_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_mqtt_overlay, mqtt_dismiss_kb_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_move_foreground(s_mqtt_overlay);

    int card_h = modal_card_height(650);
    lv_obj_t *card = lv_obj_create(s_mqtt_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 40, card_h);
    lv_obj_set_pos(card, 20, modal_card_y(card_h));
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, mqtt_dismiss_kb_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(card, mqtt_dismiss_kb_cb, LV_EVENT_PRESSED, nullptr);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "MQTT");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *list = lv_obj_create(card);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), card_h - 120);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(list, mqtt_dismiss_kb_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(list, mqtt_dismiss_kb_cb, LV_EVENT_PRESSED, nullptr);

    const char *addr_default = moduleConfig.mqtt.address[0] ? moduleConfig.mqtt.address : default_mqtt_address;
    const char *user_default = moduleConfig.mqtt.username[0] ? moduleConfig.mqtt.username : default_mqtt_username;
    const char *pass_default = moduleConfig.mqtt.password[0] ? moduleConfig.mqtt.password : default_mqtt_password;
    const char *root_default = moduleConfig.mqtt.root[0] ? moduleConfig.mqtt.root : "msh";
    const meshtastic_Channel &primary_ch = channels.getByIndex(channels.getPrimaryIndex());

    s_mqtt_enabled = mqtt_add_switch_row(list, "MQTT enabled", moduleConfig.mqtt.enabled);
    s_mqtt_ch0_uplink = mqtt_add_switch_row(list, "Primary channel uplink", primary_ch.settings.uplink_enabled);
    s_mqtt_ch0_downlink = mqtt_add_switch_row(list, "Primary channel downlink", primary_ch.settings.downlink_enabled);
    s_mqtt_address = mqtt_add_text_field(list, "Address", addr_default, (int)sizeof(moduleConfig.mqtt.address) - 1, false);
    s_mqtt_username = mqtt_add_text_field(list, "Username", user_default, (int)sizeof(moduleConfig.mqtt.username) - 1, false);
    s_mqtt_password = mqtt_add_text_field(list, "Password", pass_default, (int)sizeof(moduleConfig.mqtt.password) - 1, true);
    s_mqtt_root = mqtt_add_text_field(list, "Root topic", root_default, (int)sizeof(moduleConfig.mqtt.root) - 1, false);
    s_mqtt_encrypt = mqtt_add_switch_row(list, "Encryption enabled", moduleConfig.mqtt.encryption_enabled);
    s_mqtt_json = mqtt_add_switch_row(list, "JSON output enabled", moduleConfig.mqtt.json_enabled);
    s_mqtt_tls = mqtt_add_switch_row(list, "TLS enabled", moduleConfig.mqtt.tls_enabled);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 130, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, [](lv_event_t *) { mqtt_modal_close(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);

    lv_obj_t *save = lv_button_create(card);
    lv_obj_set_size(save, 130, 42);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(save, 0, 0);
    lv_obj_add_event_cb(save, mqtt_modal_save_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "Save");
    lv_obj_set_style_text_color(sl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
    lv_obj_center(sl);
}

// ============================================================================
//  Event handlers — all write to config.* and mark dirty
// ============================================================================

// ---- LoRa ----
static void region_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= REGION_COUNT) return;
    auto sel = (meshtastic_Config_LoRaConfig_RegionCode)REGIONS[idx].code;
    if (sel == config.lora.region) return;
    config.lora.region = sel;
    cfg_mark_dirty();
}
static void preset_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= PRESET_COUNT) return;
    auto sel = (meshtastic_Config_LoRaConfig_ModemPreset)PRESETS[idx].code;
    if (sel == config.lora.modem_preset && config.lora.use_preset) return;
    config.lora.use_preset   = true;
    config.lora.modem_preset = sel;
    cfg_mark_dirty();
}
static void hop_limit_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= HOP_LIMIT_COUNT) return;
    uint32_t v = (uint32_t)HOP_LIMITS[idx].code;
    if (v == config.lora.hop_limit) return;
    config.lora.hop_limit = v;
    cfg_mark_dirty();
}
static void tx_power_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= TX_POWER_COUNT) return;
    int8_t v = (int8_t)TX_POWERS[idx].code;
    if (v == config.lora.tx_power) return;
    config.lora.tx_power = v;
    cfg_mark_dirty();
}
static void tx_enabled_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    config.lora.tx_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    cfg_mark_dirty();
}
// RX Boosted Gain: sx126x_rx_boosted_gain in the protobuf. The firmware
// reads this at radio init time (SX126xInterface::init calls
// `lora.setRxBoostedGainMode(config.lora.sx126x_rx_boosted_gain)`) so a
// reboot is required for the change to take effect — which our debounced
// apply model delivers automatically.
static void rx_boost_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    config.lora.sx126x_rx_boosted_gain = lv_obj_has_state(sw, LV_STATE_CHECKED);
    cfg_mark_dirty();
}
// Override Duty Cycle: lets the user ignore the regional regulatory
// duty-cycle cap (1% in EU868 etc). Only legal to enable if you actually
// have permission to transmit more — e.g. amateur licence. The mesh
// stack reads this when computing airtime budgets.
static void override_duty_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    config.lora.override_duty_cycle = lv_obj_has_state(sw, LV_STATE_CHECKED);
    cfg_mark_dirty();
}

// ---- Device ----
static void role_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= ROLE_COUNT) return;
    auto sel = (meshtastic_Config_DeviceConfig_Role)ROLES[idx].code;
    if (sel == config.device.role) return;
    config.device.role = sel;
    cfg_mark_dirty();
}
static void rebroadcast_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= REBROADCAST_COUNT) return;
    auto sel = (meshtastic_Config_DeviceConfig_RebroadcastMode)REBROADCAST_MODES[idx].code;
    if (sel == config.device.rebroadcast_mode) return;
    config.device.rebroadcast_mode = sel;
    cfg_mark_dirty();
}
static void node_info_interval_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= INTERVAL_SHORT_COUNT) return;
    uint32_t s = INTERVAL_SHORT[idx].secs;
    if (s == config.device.node_info_broadcast_secs) return;
    config.device.node_info_broadcast_secs = s;
    cfg_mark_dirty();
}

// ---- Display ----
static void screen_timeout_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= SCREEN_TIMEOUT_COUNT) return;
    uint32_t s = SCREEN_TIMEOUT[idx].secs;
    if (s == config.display.screen_on_secs) return;
    config.display.screen_on_secs = s;
    // Backlight timeout is lock-free and can update live; only persistence is
    // deferred to the main loop.
    backlight_set_timeout_secs(s);
    cfg_mark_save_only();
}
static void units_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= UNITS_COUNT) return;
    auto sel = (meshtastic_Config_DisplayConfig_DisplayUnits)UNITS[idx].code;
    if (sel == config.display.units) return;
    config.display.units = sel;
    cfg_mark_dirty();
}

static bool orientation_target_landscape()
{
    return s_orientation_dirty ? s_pending_landscape : landscape_active();
}

static void orientation_toggle_clicked_cb(lv_event_t *)
{
    bool target_landscape = !orientation_target_landscape();
    if (s_orientation_dirty && s_pending_landscape == target_landscape && s_orientation_overlay)
        return;

    cfg_mark_orientation(target_landscape);
    if (s_orientation_dirty)
        orientation_overlay_open();
    refresh_values();
}

// ---- Network ----
static void wifi_enabled_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (enabled && config.network.wifi_ssid[0] == '\0') {
        // First-time WiFi setup should complete SSID + password before the
        // debounced save/reboot starts.
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        wifi_scan_clicked_cb(nullptr);
        return;
    }
    if (config.network.wifi_enabled == enabled) return;
    config.network.wifi_enabled = enabled;
    cfg_mark_dirty();
}

// ---- Owner ----
// The callbacks fire from the text-edit modal's OK button. They queue the
// rename to the aux thread (which runs on the main loop) so persistence
// happens in the right context, then refresh the About row immediately so
// the user sees the change without waiting for the next tick.
static void owner_long_entered(const char *text)
{
    if (!text || !*text) return;
    ensure_aux_thread();
    queue_owner_edit(text, nullptr);
}
static void owner_short_entered(const char *text)
{
    if (!text || !*text) return;
    ensure_aux_thread();
    queue_owner_edit(nullptr, text);
}
static void owner_long_tap_cb(lv_event_t *)
{
    text_edit_modal("Edit long name", owner.long_name, 39,
                    /*password*/ false, owner_long_entered);
}
static void owner_short_tap_cb(lv_event_t *)
{
    text_edit_modal("Edit short name (max 4)", owner.short_name, 4,
                    /*password*/ false, owner_short_entered);
}

// ---- Actions ----
static void reboot_clicked_cb(lv_event_t *)
{
    LOG_INFO("mcui: user-requested reboot in 1 s");
    rebootAtMsec = millis() + 1000;
}
static void shutdown_clicked_cb(lv_event_t *)
{
    LOG_INFO("mcui: user-requested shutdown in 1 s");
    shutdownAtMsec = millis() + 1000;
}

static void clear_nodes_confirm_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
    lv_obj_t *mbox = lv_obj_get_parent(lv_obj_get_parent(btn));
    bool do_clear = (btn == (lv_obj_t *)lv_event_get_user_data(e));
    if (do_clear) {
        LOG_WARN("mcui: clear all nodes queued");
        node_actions_clear_all();
    }
    if (mbox)
        lv_obj_delete(mbox);
}

static void clear_nodes_clicked_cb(lv_event_t *)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *dim = lv_obj_create(scr);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, SCR_W, SCR_H);
    lv_obj_set_pos(dim, 0, 0);
    lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(dim);

    lv_obj_t *card = lv_obj_create(dim);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 420, 220);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Delete all nodes?");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, "This clears the node database, including favourites.\nYour owner/config/channels stay intact.");
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 34);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 150, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);

    lv_obj_t *erase = lv_button_create(card);
    lv_obj_set_size(erase, 150, 42);
    lv_obj_align(erase, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(erase, lv_color_hex(0xB83232), 0);
    lv_obj_set_style_radius(erase, 0, 0);
    lv_obj_t *el = lv_label_create(erase);
    lv_label_set_text(el, "Delete all");
    lv_obj_set_style_text_color(el, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(el, &lv_font_montserrat_16, 0);
    lv_obj_center(el);

    lv_obj_add_event_cb(cancel, clear_nodes_confirm_cb, LV_EVENT_CLICKED, erase);
    lv_obj_add_event_cb(erase, clear_nodes_confirm_cb, LV_EVENT_CLICKED, erase);
}

static void regenerate_keys_confirm_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
    lv_obj_t *mbox = lv_obj_get_parent(lv_obj_get_parent(btn));

    bool do_regen = (btn == (lv_obj_t *)lv_event_get_user_data(e));
    if (do_regen) {
        LOG_WARN("mcui: private key regeneration queued");
        ensure_aux_thread();
        queue_regenerate_private_keys();
    }
    if (mbox)
        lv_obj_delete(mbox);
}

static void regenerate_keys_clicked_cb(lv_event_t *)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *dim = lv_obj_create(scr);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, SCR_W, SCR_H);
    lv_obj_set_pos(dim, 0, 0);
    lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(dim);

    lv_obj_t *card = lv_obj_create(dim);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 440, 240);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Regenerate private keys?");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body,
        "This creates a new public/private key pair.\n"
        "Existing encrypted direct-message trust may need to be re-established.");
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 34);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 150, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);

    lv_obj_t *regen = lv_button_create(card);
    lv_obj_set_size(regen, 170, 42);
    lv_obj_align(regen, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(regen, lv_color_hex(0xB87532), 0);
    lv_obj_set_style_radius(regen, 0, 0);
    lv_obj_t *rl = lv_label_create(regen);
    lv_label_set_text(rl, "Regenerate");
    lv_obj_set_style_text_color(rl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_16, 0);
    lv_obj_center(rl);

    lv_obj_add_event_cb(cancel, regenerate_keys_confirm_cb, LV_EVENT_CLICKED, regen);
    lv_obj_add_event_cb(regen, regenerate_keys_confirm_cb, LV_EVENT_CLICKED, regen);
}

// Factory reset uses a confirmation modal to prevent fat-finger accidents.
// A single tap on Factory Reset opens a confirmation msgbox; only the
// explicit "Erase" button inside wipes and reboots.
static void factory_reset_confirm_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *mbox = lv_obj_get_parent(lv_obj_get_parent(btn));

    bool do_erase = (btn == (lv_obj_t *)lv_event_get_user_data(e));
    if (do_erase) {
        LOG_WARN("mcui: factory reset queued — will run on main loop");
        ensure_cfg_thread();
        s_factory_reset_req = true;
    }
    if (mbox) lv_obj_delete(mbox);
}

static void factory_reset_clicked_cb(lv_event_t *)
{
    // Simple built modal: a centred dark card with text and two buttons.
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *dim = lv_obj_create(scr);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, SCR_W, SCR_H);
    lv_obj_set_pos(dim, 0, 0);
    lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(dim);

    lv_obj_t *card = lv_obj_create(dim);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 380, 220);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Factory reset?");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body,
        "This erases all config, channels, owner info and the node DB.\n"
        "The device will reboot.");
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 32);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 140, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);

    lv_obj_t *erase = lv_button_create(card);
    lv_obj_set_size(erase, 140, 42);
    lv_obj_align(erase, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(erase, lv_color_hex(0xB83232), 0); // red
    lv_obj_set_style_radius(erase, 0, 0);
    lv_obj_t *el = lv_label_create(erase);
    lv_label_set_text(el, "Erase");
    lv_obj_set_style_text_color(el, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(el, &lv_font_montserrat_16, 0);
    lv_obj_center(el);

    // Both buttons share the same confirm cb. user_data points to the
    // "erase" button so the cb can distinguish which one was tapped.
    lv_obj_add_event_cb(cancel, factory_reset_confirm_cb, LV_EVENT_CLICKED, erase);
    lv_obj_add_event_cb(erase,  factory_reset_confirm_cb, LV_EVENT_CLICKED, erase);
}

// ============================================================================
//  Refresh live read-only values (called every ~1 s from settings_screen_tick)
// ============================================================================

static void refresh_values()
{
    if (s_lbl_firmware) lv_label_set_text(s_lbl_firmware, optstr(APP_VERSION));

    char buf[96];

    if (s_lbl_owner) {
        const char *ln = owner.long_name[0]  ? owner.long_name  : "(unset)";
        const char *sn = owner.short_name[0] ? owner.short_name : "--";
        snprintf(buf, sizeof(buf), "%s (%s)", ln, sn);
        lv_label_set_text(s_lbl_owner, buf);
    }
    if (s_lbl_nodeid && nodeDB) {
        snprintf(buf, sizeof(buf), "!%08x", (unsigned)nodeDB->getNodeNum());
        lv_label_set_text(s_lbl_nodeid, buf);
    }
    if (s_lbl_region_now)
        lv_label_set_text(s_lbl_region_now,
                          enum_label(REGIONS, REGION_COUNT, config.lora.region));
    if (s_lbl_preset_now)
        lv_label_set_text(s_lbl_preset_now,
                          enum_label(PRESETS, PRESET_COUNT, config.lora.modem_preset));
    if (s_lbl_uptime) {
        uint32_t s = (uint32_t)(millis() / 1000);
        uint32_t h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
        snprintf(buf, sizeof(buf), "%uh %02um %02us",
                 (unsigned)h, (unsigned)m, (unsigned)sec);
        lv_label_set_text(s_lbl_uptime, buf);
    }
    if (s_lbl_heap) {
        uint32_t free_heap = memGet.getFreeHeap();
        snprintf(buf, sizeof(buf), "%u KB", (unsigned)(free_heap / 1024));
        lv_label_set_text(s_lbl_heap, buf);
    }
    if (s_lbl_battery) {
        if (powerStatus) {
            uint8_t pct = powerStatus->getBatteryChargePercent();
            if (pct > 100) snprintf(buf, sizeof(buf), "ext");
            else           snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
        } else {
            snprintf(buf, sizeof(buf), "—");
        }
        lv_label_set_text(s_lbl_battery, buf);
    }
    if (s_lbl_noise) {
        // Ask the active radio interface for the instantaneous RX RSSI —
        // i.e. the current noise floor. SX126xInterface implements it by
        // calling `lora.getRSSI(false)`; radios that don't override return
        // 0.0 from the RadioInterface base, which we render as a dash.
        float nf = 0.0f;
        if (router && router->getInterface()) nf = router->getInterface()->getNoiseFloor();
        if (nf < -1.0f) {
            // Valid reading (real noise floors on LoRa are deeply negative,
            // typically around -120 to -90 dBm)
            snprintf(buf, sizeof(buf), "%d dBm", (int)lroundf(nf));
        } else {
            snprintf(buf, sizeof(buf), "—");
        }
        lv_label_set_text(s_lbl_noise, buf);
    }
    if (s_lbl_wifi_ssid) {
        const char *s = config.network.wifi_ssid[0] ? config.network.wifi_ssid : "(unset)";
        lv_label_set_text(s_lbl_wifi_ssid, s);
        bool connected = config.network.wifi_enabled && WiFi.status() == WL_CONNECTED;
        lv_obj_set_style_text_color(s_lbl_wifi_ssid,
                                    lv_color_hex(connected ? 0x45D483 : TH_TEXT), 0);
    }
    if (s_lbl_orientation) {
        lv_label_set_text(s_lbl_orientation,
                          s_orientation_dirty
                              ? (s_pending_landscape ? "Landscape (pending reboot)"
                                                     : "Portrait (pending reboot)")
                              : (landscape_active() ? "Landscape" : "Portrait"));
    }
    if (s_btn_orientation_label) {
        lv_label_set_text(s_btn_orientation_label,
                          orientation_target_landscape()
                              ? "Switch to portrait mode"
                              : "Switch to landscape mode");
    }
    if (s_lbl_owner_long)
        lv_label_set_text(s_lbl_owner_long,
                          owner.long_name[0] ? owner.long_name : "(tap to set)");
    if (s_lbl_owner_short)
        lv_label_set_text(s_lbl_owner_short,
                          owner.short_name[0] ? owner.short_name : "(tap to set)");

    // Drive the WiFi scan modal's state transitions
    wifi_modal_tick();

    // "Pending save" countdown
    if (s_lbl_pending) {
        if (cfg_has_pending()) {
            uint32_t elapsed = millis() - s_last_change_ms;
            uint32_t remaining =
                (elapsed >= APPLY_DEBOUNCE_MS) ? 0 : (APPLY_DEBOUNCE_MS - elapsed);
            snprintf(buf, sizeof(buf),
                     "Changes pending — saving in %u s...",
                     (unsigned)((remaining + 999) / 1000));
            lv_label_set_text(s_lbl_pending, buf);
            lv_obj_remove_flag(s_lbl_pending, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_pending, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================================================================
//  Build the full screen
// ============================================================================

static void rebuild_settings()
{
    // -------- Owner (tap to edit) --------
    add_section_header("Owner");
    lv_obj_t *owner_card = add_card();
    {
        // Two tappable rows. We wrap each info_row with a button-like
        // clickable container so the whole row is a tap target.
        lv_obj_t *row_long = lv_obj_create(owner_card);
        lv_obj_remove_style_all(row_long);
        lv_obj_set_width(row_long, lv_pct(100));
        lv_obj_set_height(row_long, 32);
        lv_obj_add_flag(row_long, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row_long, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row_long, lv_color_hex(TH_INPUT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row_long, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row_long, 0, 0);
        lv_obj_add_event_cb(row_long, owner_long_tap_cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *l1 = lv_label_create(row_long);
        lv_label_set_text(l1, "Long name");
        lv_obj_set_style_text_color(l1, lv_color_hex(TH_TEXT2), 0);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_16, 0);
        lv_obj_align(l1, LV_ALIGN_LEFT_MID, 0, 0);

        s_lbl_owner_long = lv_label_create(row_long);
        lv_label_set_text(s_lbl_owner_long, owner.long_name[0] ? owner.long_name : "(tap to set)");
        lv_obj_set_style_text_color(s_lbl_owner_long, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(s_lbl_owner_long, &lv_font_montserrat_16, 0);
        lv_obj_align(s_lbl_owner_long, LV_ALIGN_RIGHT_MID, -4, 0);
    }
    {
        lv_obj_t *row_short = lv_obj_create(owner_card);
        lv_obj_remove_style_all(row_short);
        lv_obj_set_width(row_short, lv_pct(100));
        lv_obj_set_height(row_short, 32);
        lv_obj_add_flag(row_short, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row_short, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row_short, lv_color_hex(TH_INPUT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row_short, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row_short, 0, 0);
        lv_obj_add_event_cb(row_short, owner_short_tap_cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *l2 = lv_label_create(row_short);
        lv_label_set_text(l2, "Short name");
        lv_obj_set_style_text_color(l2, lv_color_hex(TH_TEXT2), 0);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_16, 0);
        lv_obj_align(l2, LV_ALIGN_LEFT_MID, 0, 0);

        s_lbl_owner_short = lv_label_create(row_short);
        lv_label_set_text(s_lbl_owner_short, owner.short_name[0] ? owner.short_name : "(tap to set)");
        lv_obj_set_style_text_color(s_lbl_owner_short, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(s_lbl_owner_short, &lv_font_montserrat_16, 0);
        lv_obj_align(s_lbl_owner_short, LV_ALIGN_RIGHT_MID, -4, 0);
    }
    add_card_hint(owner_card, "Tap a row to edit. Saves and broadcasts immediately.");

    // -------- Pending-save status banner --------
    s_lbl_pending = lv_label_create(s_list);
    lv_label_set_text(s_lbl_pending, "");
    lv_obj_set_style_text_color(s_lbl_pending, lv_color_hex(0xE0A030), 0);
    lv_obj_set_style_text_font(s_lbl_pending, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(s_lbl_pending, 6, 0);
    lv_obj_set_style_pad_left(s_lbl_pending, 6, 0);
    lv_obj_add_flag(s_lbl_pending, LV_OBJ_FLAG_HIDDEN);

    // -------- LoRa --------
    add_section_header("LoRa");
    lv_obj_t *lora = add_card();
    add_enum_dropdown_row(lora, "Region",
                          REGIONS, REGION_COUNT, config.lora.region,
                          region_changed_cb);
    add_enum_dropdown_row(lora, "Preset",
                          PRESETS, PRESET_COUNT, config.lora.modem_preset,
                          preset_changed_cb);
    {
        int hop = (int)config.lora.hop_limit;
        if (hop < 1 || hop > 7) hop = 3;
        add_enum_dropdown_row(lora, "Hop limit",
                              HOP_LIMITS, HOP_LIMIT_COUNT, hop,
                              hop_limit_changed_cb);
    }
    add_enum_dropdown_row(lora, "TX power",
                          TX_POWERS, TX_POWER_COUNT,
                          (int)config.lora.tx_power,
                          tx_power_changed_cb);
    add_switch_row(lora, "TX enabled", config.lora.tx_enabled, tx_enabled_changed_cb);
    add_switch_row(lora, "RX boosted gain",
                   config.lora.sx126x_rx_boosted_gain, rx_boost_changed_cb);
    add_switch_row(lora, "Override duty cycle",
                   config.lora.override_duty_cycle, override_duty_changed_cb);
    add_card_hint(lora, "Changing region, preset, or RX boosted gain reboots the device.");

    // -------- Device --------
    add_section_header("Device");
    lv_obj_t *device = add_card();
    add_enum_dropdown_row(device, "Role",
                          ROLES, ROLE_COUNT, config.device.role,
                          role_changed_cb);
    add_enum_dropdown_row(device, "Rebroadcast",
                          REBROADCAST_MODES, REBROADCAST_COUNT,
                          config.device.rebroadcast_mode,
                          rebroadcast_changed_cb);
    add_interval_dropdown_row(device, "NodeInfo interval",
                              INTERVAL_SHORT, INTERVAL_SHORT_COUNT,
                              config.device.node_info_broadcast_secs,
                              node_info_interval_changed_cb);

    // -------- Display --------
    // Stripped down to just Screen sleep + Units — the other display
    // toggles (12h clock, heading bold, wake-on-tap) target the OLED
    // screen layer which this device doesn't use anyway.
    add_section_header("Display");
    lv_obj_t *display = add_card();
    add_interval_dropdown_row(display, "Screen sleep",
                              SCREEN_TIMEOUT, SCREEN_TIMEOUT_COUNT,
                              config.display.screen_on_secs,
                              screen_timeout_changed_cb);
    add_enum_dropdown_row(display, "Units",
                          UNITS, UNITS_COUNT, config.display.units,
                          units_changed_cb);
    s_lbl_orientation = add_info_row(display, "Orientation",
                                     landscape_active() ? "Landscape" : "Portrait");
    s_btn_orientation = lv_button_create(display);
    lv_obj_set_size(s_btn_orientation, lv_pct(100), 46);
    lv_obj_set_style_bg_color(s_btn_orientation, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(s_btn_orientation, 0, 0);
    lv_obj_add_event_cb(s_btn_orientation, orientation_toggle_clicked_cb, LV_EVENT_CLICKED, nullptr);
    s_btn_orientation_label = lv_label_create(s_btn_orientation);
    lv_label_set_text(s_btn_orientation_label,
                      landscape_active() ? "Switch to portrait mode"
                                         : "Switch to landscape mode");
    lv_obj_set_style_text_color(s_btn_orientation_label, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(s_btn_orientation_label, &lv_font_montserrat_16, 0);
    lv_obj_center(s_btn_orientation_label);
    add_card_hint(display, "Orientation changes after save and reboot.");

    // -------- Network --------
    add_section_header("Network");
    lv_obj_t *net = add_card();
    add_switch_row(net, "WiFi enabled", config.network.wifi_enabled,
                   wifi_enabled_changed_cb);
    s_lbl_wifi_ssid = add_info_row(net, "SSID", "-");
    {
        // Scan & connect button — opens the modal, which kicks the
        // McAuxThread to run WiFi.scanNetworks() on the main loop.
        lv_obj_t *btn = lv_button_create(net);
        lv_obj_set_size(btn, lv_pct(100), 46);
        lv_obj_set_style_bg_color(btn, lv_color_hex(TH_ACCENT), 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_add_event_cb(btn, wifi_scan_clicked_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, LV_SYMBOL_WIFI "  Scan & connect");
        lv_obj_set_style_text_color(bl, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
        lv_obj_center(bl);
    }
    {
        lv_obj_t *btn = lv_button_create(net);
        lv_obj_set_size(btn, lv_pct(100), 46);
        lv_obj_set_style_bg_color(btn, lv_color_hex(TH_ACCENT), 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_add_event_cb(btn, mqtt_modal_open_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, "MQTT");
        lv_obj_set_style_text_color(bl, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
        lv_obj_center(bl);
    }
    add_card_hint(net, "First setup scans first, then saves and reboots after the password.");

    // -------- About --------
    add_section_header("About");
    lv_obj_t *about = add_card();
    s_lbl_firmware   = add_info_row(about, "Firmware",  optstr(APP_VERSION));
    s_lbl_owner      = add_info_row(about, "Owner",     "-");
    s_lbl_nodeid     = add_info_row(about, "Node ID",   "-");
    s_lbl_region_now = add_info_row(about, "Region",    "-");
    s_lbl_preset_now = add_info_row(about, "Preset",    "-");
    s_lbl_uptime     = add_info_row(about, "Uptime",    "-");
    s_lbl_heap       = add_info_row(about, "Free heap", "-");
    s_lbl_battery    = add_info_row(about, "Battery",   "-");
    s_lbl_noise      = add_info_row(about, "Noise floor", "-");

    // -------- Actions --------
    add_section_header("Actions");
    lv_obj_t *actions = add_card();

    auto make_action_btn = [&](const char *label, uint32_t bg_color,
                               lv_event_cb_t cb) {
        lv_obj_t *btn = lv_button_create(actions);
        lv_obj_set_size(btn, lv_pct(100), 48);
        lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, label);
        lv_obj_set_style_text_color(bl, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
        lv_obj_center(bl);
        return btn;
    };
    make_action_btn("Reboot",       TH_BUBBLE_OUT, reboot_clicked_cb);
    make_action_btn("Shutdown",     TH_INPUT,      shutdown_clicked_cb);
    make_action_btn("Delete all nodes", 0xB87532,  clear_nodes_clicked_cb);
    make_action_btn("Regenerate Private Keys", 0xB87532, regenerate_keys_clicked_cb);
    make_action_btn("Factory reset", 0xB83232,     factory_reset_clicked_cb);
}

lv_obj_t *settings_screen_create(lv_obj_t *parent)
{
    ensure_cfg_thread();

    s_page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, SCR_W, PAGE_H);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    s_list = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, SCR_W, PAGE_H);
    lv_obj_set_pos(s_list, 0, 0);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    rebuild_settings();
    refresh_values();
    return s_page;
}

void settings_screen_tick()
{
    if (!s_page) return;
    orientation_overlay_update();
    uint32_t now = millis();
    if (now - s_last_refresh_ms < 1000) return;
    s_last_refresh_ms = now;
    refresh_values();
}

} // namespace mcui

#endif
