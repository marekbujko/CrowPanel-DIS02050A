// ============================================================
// wifi_ntp.cpp - WiFi connection, NTP time sync, credential storage
// ============================================================

#include "wifi_ntp.h"
#include "app_globals.h"
#include "utils.h"
#include "settings_cb.h"

#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <target.h>
#include <RTClib.h>

// ---- State ----
bool g_wifi_enabled = false;
bool g_wifi_connected = false;
bool g_wifi_has_saved_network = false;
char g_wifi_ssid[33] = {};

static char s_wifi_pass[65] = {};
static Preferences s_wifi_prefs;
static uint32_t s_last_ntp_sync_ms = 0;
static uint32_t s_reconnect_ms = 0;
static bool s_ntp_synced_once = false;
static const uint32_t NTP_SYNC_INTERVAL_MS = 3600000UL;  // re-sync every hour
static const uint32_t RECONNECT_INTERVAL_MS = 30000UL;
static bool s_wifi_driver_ok = true;
static const size_t WIFI_INIT_MIN_FREE_INTERNAL = 52000;
static const size_t WIFI_INIT_MIN_LARGEST_INTERNAL = 22000;

// Deferred scan state
static volatile bool s_scan_requested = false;
static bool s_scan_results_ready = false;
static int  s_scan_result_count = 0;

// ---- Credential management ----

void wifi_load_credentials() {
  s_wifi_prefs.begin("wifi", true);
  String ssid = s_wifi_prefs.getString("ssid", "");
  String pass = s_wifi_prefs.getString("pass", "");
  g_wifi_enabled = s_wifi_prefs.getUChar("enabled", 0) != 0;
  s_wifi_prefs.end();

  if (ssid.length() > 0) {
    strncpy(g_wifi_ssid, ssid.c_str(), sizeof(g_wifi_ssid) - 1);
    g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';
    strncpy(s_wifi_pass, pass.c_str(), sizeof(s_wifi_pass) - 1);
    s_wifi_pass[sizeof(s_wifi_pass) - 1] = '\0';
    g_wifi_has_saved_network = true;
  } else {
    g_wifi_ssid[0] = '\0';
    s_wifi_pass[0] = '\0';
    g_wifi_has_saved_network = false;
  }
}

void wifi_save_credentials(const char* ssid, const char* password) {
  strncpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid) - 1);
  g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';
  strncpy(s_wifi_pass, password, sizeof(s_wifi_pass) - 1);
  s_wifi_pass[sizeof(s_wifi_pass) - 1] = '\0';
  g_wifi_has_saved_network = true;

  s_wifi_prefs.begin("wifi", false);
  s_wifi_prefs.putString("ssid", g_wifi_ssid);
  s_wifi_prefs.putString("pass", s_wifi_pass);
  s_wifi_prefs.end();
}

void wifi_forget_credentials() {
  g_wifi_ssid[0] = '\0';
  s_wifi_pass[0] = '\0';
  g_wifi_has_saved_network = false;

  s_wifi_prefs.begin("wifi", false);
  s_wifi_prefs.remove("ssid");
  s_wifi_prefs.remove("pass");
  s_wifi_prefs.end();

  wifi_disconnect();
}

bool wifi_has_credentials() {
  return g_wifi_has_saved_network && g_wifi_ssid[0] != '\0';
}

static void wifi_save_enabled(bool en) {
  s_wifi_prefs.begin("wifi", false);
  s_wifi_prefs.putUChar("enabled", en ? 1 : 0);
  s_wifi_prefs.end();
}

static bool wifi_ensure_sta() {
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_STA || mode == WIFI_AP_STA) return true;
  if (!WiFi.mode(WIFI_STA)) {
    serialmon_append("WiFi: STA mode init failed (esp_wifi_init no mem)");
    s_wifi_driver_ok = false;
    return false;
  }
  return true;
}

static bool wifi_memory_ok_for_driver() {
  size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (free_internal < WIFI_INIT_MIN_FREE_INTERNAL || largest_internal < WIFI_INIT_MIN_LARGEST_INTERNAL) {
    char msg[112];
    snprintf(msg, sizeof(msg),
             "WiFi: init skipped (low mem free=%u largest=%u)",
             (unsigned)free_internal, (unsigned)largest_internal);
    serialmon_append(msg);
    return false;
  }
  return true;
}

// ---- RTC hardware time persistence ----

static void wifi_write_time_to_rtc(uint32_t utc_epoch) {
  if (!g_rtc_ok) return;
  g_rtc.adjust(DateTime(utc_epoch));  // store UTC directly - no offset
}

// ---- NTP sync ----

void wifi_ntp_sync() {
  if (!g_wifi_connected) return;

  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    time_t now;
    time(&now);
    uint32_t epoch = (uint32_t)now;
    mesh_set_time_epoch(epoch);
    tz_update_offset_now();   // recalculate DST with accurate UTC time
    wifi_write_time_to_rtc(epoch);
    g_deferred_timelabel_dirty = true;
    s_last_ntp_sync_ms = millis();
    s_ntp_synced_once = true;

    char buf[48];
    snprintf(buf, sizeof(buf), "NTP sync OK: %04d-%02d-%02d %02d:%02d UTC",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min);
    serialmon_append(buf);
  } else {
    serialmon_append("NTP sync failed");
  }
}

// ---- Connection control ----

void wifi_init() {
  wifi_load_credentials();
  s_wifi_driver_ok = true;

  if (g_wifi_enabled) {
    if (!wifi_memory_ok_for_driver()) {
      g_wifi_enabled = false;
      wifi_save_enabled(false);
      s_wifi_driver_ok = false;
      return;
    }
    if (!wifi_ensure_sta()) {
      g_wifi_enabled = false;
      wifi_save_enabled(false);
      return;
    }
    WiFi.setAutoReconnect(false);
    if (wifi_has_credentials()) {
      wifi_connect_saved();
    }
  }
}

void wifi_connect_saved() {
  if (!wifi_has_credentials() || !s_wifi_driver_ok) return;
  if (!wifi_ensure_sta()) return;

  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(g_wifi_ssid, s_wifi_pass);

  char msg[64];
  snprintf(msg, sizeof(msg), "WiFi: connecting to %s...", g_wifi_ssid);
  serialmon_append(msg);
  s_reconnect_ms = millis() + RECONNECT_INTERVAL_MS;
}

void wifi_disconnect() {
  WiFi.disconnect(true);
  g_wifi_connected = false;
  s_reconnect_ms = 0;
}

void wifi_toggle(bool enable) {
  g_wifi_enabled = enable;
  wifi_save_enabled(enable);

  if (enable) {
    s_wifi_driver_ok = true;
    if (!wifi_memory_ok_for_driver()) {
      g_wifi_enabled = false;
      wifi_save_enabled(false);
      s_wifi_driver_ok = false;
      s_reconnect_ms = 0;
      g_deferred_wifi_status_dirty = true;
      return;
    }
    if (!wifi_ensure_sta()) {
      g_wifi_enabled = false;
      wifi_save_enabled(false);
      s_reconnect_ms = 0;
      g_deferred_wifi_status_dirty = true;
      return;
    }
    WiFi.setAutoReconnect(false);
    if (wifi_has_credentials()) wifi_connect_saved();
  } else {
    wifi_disconnect();
    WiFi.mode(WIFI_OFF);
  }
}

// ---- Scanning (deferred synchronous) ----

void wifi_request_scan() {
  s_scan_results_ready = false;
  s_scan_result_count = 0;
  s_scan_requested = true;
  serialmon_append("WiFi: scan requested...");
}

bool wifi_scan_results_ready() {
  return s_scan_results_ready;
}

int wifi_scan_result_count() {
  return s_scan_result_count;
}

void wifi_scan_results_consumed() {
  s_scan_results_ready = false;
  s_scan_result_count = 0;
}

// Accessors for scan results (valid after scan_results_ready)
String wifi_scan_ssid(int idx) {
  return WiFi.SSID(idx);
}

int wifi_scan_rssi(int idx) {
  return WiFi.RSSI(idx);
}

static void wifi_do_sync_scan() {
  // Ensure STA mode
  wifi_mode_t mode = WiFi.getMode();
  if (mode != WIFI_STA && mode != WIFI_AP_STA) {
    WiFi.mode(WIFI_STA);
    delay(200);
  }

  // Guard against known ESP32-S3 crash path:
  // WiFi scan may abort when internal heap is too fragmented/low.
  size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (free_internal < 50000 || largest_internal < 24000) {
    char msg[96];
    snprintf(msg, sizeof(msg), "WiFi: scan skipped (low mem free=%u largest=%u)",
             (unsigned)free_internal, (unsigned)largest_internal);
    serialmon_append(msg);
    s_scan_result_count = 0;
    s_scan_results_ready = true;
    return;
  }

  WiFi.scanDelete();

  // Synchronous scan - blocks ~2-4 seconds but reliably returns results
  int n = WiFi.scanNetworks(false, false, false, 300);

  if (n > 0) {
    s_scan_result_count = n;
    char msg[48];
    snprintf(msg, sizeof(msg), "WiFi: scan found %d network(s)", n);
    serialmon_append(msg);
  } else {
    s_scan_result_count = 0;
    serialmon_append("WiFi: scan found 0 networks");
  }
  s_scan_results_ready = true;
}

// ---- Loop ----

void wifi_loop() {
  // Handle deferred scan (runs even if WiFi is "disabled" - scan temporarily enables STA)
  if (s_scan_requested) {
    s_scan_requested = false;
    wifi_do_sync_scan();
  }

  if (!g_wifi_enabled || !s_wifi_driver_ok) return;

  bool was_connected = g_wifi_connected;
  g_wifi_connected = (WiFi.status() == WL_CONNECTED);

  // Just connected
  if (g_wifi_connected && !was_connected) {
    char msg[64];
    snprintf(msg, sizeof(msg), "WiFi: connected to %s", g_wifi_ssid);
    serialmon_append(msg);
    g_deferred_wifi_status_dirty = true;
    wifi_ntp_sync();
  }

  // Just disconnected
  if (!g_wifi_connected && was_connected) {
    serialmon_append("WiFi: disconnected");
    g_deferred_wifi_status_dirty = true;
  }

  // Reconnect if we have credentials but aren't connected
  if (!g_wifi_connected && wifi_has_credentials() && s_reconnect_ms > 0) {
    if ((int32_t)(millis() - s_reconnect_ms) > 0) {
      if (!wifi_memory_ok_for_driver()) {
        g_wifi_enabled = false;
        wifi_save_enabled(false);
        s_wifi_driver_ok = false;
        s_reconnect_ms = 0;
        g_deferred_wifi_status_dirty = true;
        return;
      }
      if (!wifi_ensure_sta()) {
        g_wifi_enabled = false;
        wifi_save_enabled(false);
        s_reconnect_ms = 0;
        g_deferred_wifi_status_dirty = true;
        return;
      }
      wifi_connect_saved();
    }
  }

  // Periodic NTP re-sync
  if (g_wifi_connected && s_ntp_synced_once &&
      (millis() - s_last_ntp_sync_ms) > NTP_SYNC_INTERVAL_MS) {
    wifi_ntp_sync();
  }
}
