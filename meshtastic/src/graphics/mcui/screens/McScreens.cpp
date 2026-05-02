#if HAS_TFT && USE_MCUI

#include "McScreens.h"
#include "McChatView.h"
#include "../McKeyboard.h"
#include "../McTheme.h"
#include "../McUI.h"
#include "../data/McMessages.h"

#include "configuration.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/generated/meshtastic/channel.pb.h"
#include "mesh/wifi/WiFiAPClient.h"

#if HAS_WIFI && !defined(ARCH_PORTDUINO)
#include <WiFi.h>
#endif

#include <Arduino.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <cstdio>
#include <cstring>

namespace mcui {

// ---- Shared helpers --------------------------------------------------------

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, SCR_W, PAGE_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

// Avatar color palette — hashed from (kind, value, title) so each
// conversation gets a stable colored circle.
static const uint32_t kAvatarPalette[TH_AVATAR_PALETTE_COUNT] = { TH_AVATAR_PALETTE_LIST };

static uint32_t avatar_color_for(const McConvId &id, const char *title)
{
    uint32_t h = 2166136261u ^ (uint32_t)id.kind ^ id.value;
    if (title) {
        for (const char *p = title; *p; p++) {
            h ^= (uint8_t)(unsigned char)*p;
            h *= 16777619u;
        }
    }
    return kAvatarPalette[h % TH_AVATAR_PALETTE_COUNT];
}

// ---- Channel create / delete ----------------------------------------------
// Bridge to Meshtastic core's `channels` singleton + persistence path.
// `psk_len == 0` (with psk == nullptr) creates a new channel with a fresh
// random 16-byte key. A non-null psk of length 1/16/32 joins an existing
// channel by writing that PSK into a free secondary slot.
static bool channel_create(const char *name, const uint8_t *psk = nullptr, size_t psk_len = 0,
                           bool mqtt_uplink = true, bool mqtt_downlink = true)
{
    if (!name || !*name) return false;
    if (psk_len != 0 && psk_len != 1 && psk_len != 16 && psk_len != 32) {
        LOG_WARN("mcui: channel_create: psk_len=%u invalid (must be 0/1/16/32)", (unsigned)psk_len);
        return false;
    }

    int target = -1;
    for (uint8_t i = 1; i < channels.getNumChannels(); i++) {
        meshtastic_Channel &c = channels.getByIndex(i);
        if (c.role == meshtastic_Channel_Role_DISABLED) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        LOG_WARN("mcui: channel_create: no free secondary slot");
        return false;
    }

    meshtastic_Channel ch = channels.getByIndex(target);
    ch.index           = target;
    ch.role            = meshtastic_Channel_Role_SECONDARY;
    ch.has_settings    = true;
    memset(&ch.settings, 0, sizeof(ch.settings));
    strncpy(ch.settings.name, name, sizeof(ch.settings.name) - 1);
    ch.settings.name[sizeof(ch.settings.name) - 1] = '\0';
    ch.settings.uplink_enabled = mqtt_uplink;
    ch.settings.downlink_enabled = mqtt_downlink;

    if (psk && psk_len > 0) {
        memcpy(ch.settings.psk.bytes, psk, psk_len);
        ch.settings.psk.size = psk_len;
    } else {
        ch.settings.psk.size = 16;
        esp_fill_random(ch.settings.psk.bytes, 16);
    }

    channels.setChannel(ch);
    channels.onConfigChanged();
    if (service)
        service->reloadConfig(SEGMENT_CHANNELS);
    else if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CHANNELS);

    LOG_INFO("mcui: %s channel '%s' in slot %d (psk_len=%u)",
             (psk && psk_len) ? "joined" : "created", name, target,
             (unsigned)ch.settings.psk.size);
    return true;
}

static bool channel_delete(uint8_t idx)
{
    if (idx == 0 || idx >= channels.getNumChannels()) return false;
    meshtastic_Channel ch = channels.getByIndex(idx);
    if (ch.role == meshtastic_Channel_Role_DISABLED) return false;

    LOG_INFO("mcui: deleting channel slot %d (name='%s')", idx, ch.settings.name);
    ch.role = meshtastic_Channel_Role_DISABLED;
    ch.has_settings = true;
    memset(&ch.settings, 0, sizeof(ch.settings));
    channels.setChannel(ch);
    channels.onConfigChanged();
    if (service)
        service->reloadConfig(SEGMENT_CHANNELS);
    else if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CHANNELS);
    return true;
}

// ============================================================================
//  Chats screen
// ============================================================================

// Static state for the chats screen ------------------------------------------
static lv_obj_t *s_chats_page = nullptr;  // the "page" container we own
static lv_obj_t *s_chats_list = nullptr;  // scrollable list container
static lv_obj_t *s_chat_delete_overlay = nullptr;
static uint32_t s_chats_last_tick = 0xFFFFFFFFu;
static bool s_chats_rebuild_pending = false;
static McConvId s_pending_delete_id = McConvId::none();
static McConvId s_suppress_click_id = McConvId::none();
static uint32_t s_suppress_click_ms = 0;

// Backing array of McConvIds, one per list card. Rebuilt on every refresh,
// so each card's user_data pointer remains valid for its lifetime.
struct ChatEntry {
    McConvId id;
    char title[40];
};
static ChatEntry s_entries[MC_MAX_CONVERSATIONS + 8];
static int s_num_entries = 0;

static void rebuild_chats_list();

// Called when a conversation card is tapped.
static void chat_card_clicked(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    ChatEntry *ent = (ChatEntry *)lv_obj_get_user_data(obj);
    if (!ent) return;
    if (s_suppress_click_id.is_valid() && ent->id == s_suppress_click_id) {
        if ((uint32_t)(lv_tick_get() - s_suppress_click_ms) < 1200) {
            s_suppress_click_id = McConvId::none();
            return;
        }
        s_suppress_click_id = McConvId::none();
    }
    chatview_open(ent->id, ent->title);
}

static void chat_delete_overlay_close()
{
    if (s_chat_delete_overlay) {
        lv_obj_delete(s_chat_delete_overlay);
        s_chat_delete_overlay = nullptr;
    }
    s_pending_delete_id = McConvId::none();
}

static void chat_delete_confirm_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
    bool do_delete = (btn == (lv_obj_t *)lv_event_get_user_data(e));
    McConvId id = s_pending_delete_id;
    chat_delete_overlay_close();
    if (!do_delete) return;

    // Always clear the message history. For secondary channels, also free
    // the slot. Primary channel (slot 0) only clears history — the channel
    // itself stays.
    messages_delete_conv(id);

    if (id.kind == McConvId::CHANNEL && id.value > 0) {
        channel_delete((uint8_t)id.value);
    }

    rebuild_chats_list();
}

static void chat_card_long_pressed(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    ChatEntry *ent = (ChatEntry *)lv_obj_get_user_data(obj);
    if (!ent) return;

    // Channels and DMs are both long-pressable; everything else (dividers,
    // section headers) is not.
    if (ent->id.kind != McConvId::DIRECT && ent->id.kind != McConvId::CHANNEL) return;

    s_suppress_click_id = ent->id;
    s_suppress_click_ms = lv_tick_get();

    lv_indev_t *indev = lv_indev_active();
    if (indev)
        lv_indev_wait_release(indev);

    chat_delete_overlay_close();
    s_pending_delete_id = ent->id;

    const bool is_channel   = (ent->id.kind == McConvId::CHANNEL);
    const bool is_primary   = (is_channel && ent->id.value == 0);
    const bool is_secondary = (is_channel && ent->id.value > 0);

    char title[96];
    const char *body_text;
    const char *delete_label;
    if (is_primary) {
        snprintf(title, sizeof(title), "Clear chat history?\n%s", ent->title);
        body_text = "Deletes the message history for this primary channel. "
                    "The channel itself stays — you keep receiving messages on it.";
        delete_label = "Clear history";
    } else if (is_secondary) {
        snprintf(title, sizeof(title), "Delete channel?\n%s", ent->title);
        body_text = "Permanently removes this channel and all its messages. "
                    "You will stop sending and receiving on this channel.";
        delete_label = "Delete channel";
    } else {
        snprintf(title, sizeof(title), "Delete private chat?\n%s", ent->title);
        body_text = "This deletes only the message history. The node stays in your node list.";
        delete_label = "Delete chat";
    }

    lv_obj_t *scr = lv_screen_active();
    s_chat_delete_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chat_delete_overlay);
    lv_obj_set_size(s_chat_delete_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_chat_delete_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_chat_delete_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_chat_delete_overlay, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_chat_delete_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_chat_delete_overlay);

    lv_obj_t *card = lv_obj_create(s_chat_delete_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 48, 250);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, title);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tl, lv_pct(100));
    lv_obj_set_style_text_color(tl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, body_text);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 64);

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

    lv_obj_t *delete_btn = lv_button_create(card);
    lv_obj_set_size(delete_btn, 150, 42);
    lv_obj_align(delete_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0xB83232), 0);
    lv_obj_set_style_radius(delete_btn, 0, 0);
    lv_obj_t *dl = lv_label_create(delete_btn);
    lv_label_set_text(dl, delete_label);
    lv_obj_set_style_text_color(dl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_16, 0);
    lv_obj_center(dl);

    lv_obj_add_event_cb(cancel, chat_delete_confirm_cb, LV_EVENT_CLICKED, delete_btn);
    lv_obj_add_event_cb(delete_btn, chat_delete_confirm_cb, LV_EVENT_CLICKED, delete_btn);
}

// ---- New / join channel modal ---------------------------------------------
// Single combined modal: empty PSK -> create with random key, non-empty
// PSK (base64) -> join an existing channel by writing that key into a
// free secondary slot.

static lv_obj_t *s_chcreate_overlay = nullptr;
static lv_obj_t *s_chcreate_name    = nullptr;
static lv_obj_t *s_chcreate_psk     = nullptr;
static lv_obj_t *s_chcreate_status  = nullptr;
static lv_obj_t *s_chcreate_uplink  = nullptr;
static lv_obj_t *s_chcreate_downlink = nullptr;

static void chcreate_close()
{
    keyboard_hide();
    if (s_chcreate_overlay) {
        lv_obj_delete(s_chcreate_overlay);
        s_chcreate_overlay = nullptr;
    }
    s_chcreate_name   = nullptr;
    s_chcreate_psk    = nullptr;
    s_chcreate_status = nullptr;
    s_chcreate_uplink = nullptr;
    s_chcreate_downlink = nullptr;
}

static void chcreate_cancel_cb(lv_event_t *) { chcreate_close(); }

static void chcreate_status(const char *msg, bool ok)
{
    if (!s_chcreate_status) return;
    lv_label_set_text(s_chcreate_status, msg);
    lv_obj_set_style_text_color(s_chcreate_status,
                                lv_color_hex(ok ? 0x45D483 : 0xE05050), 0);
}

// Strip whitespace from a base64 string (paste-from-phone friendly).
static size_t b64_clean(const char *in, char *out, size_t out_sz)
{
    size_t n = 0;
    if (!in) return 0;
    for (; *in && n + 1 < out_sz; in++) {
        char c = *in;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

static void chcreate_ok_cb(lv_event_t *)
{
    if (!s_chcreate_name) { chcreate_close(); return; }

    // Trim whitespace from name
    const char *raw_name = lv_textarea_get_text(s_chcreate_name);
    if (!raw_name) { chcreate_close(); return; }
    char name[12] = {};
    int len = (int)strlen(raw_name);
    int s = 0;
    while (s < len && (raw_name[s] == ' ' || raw_name[s] == '\t')) s++;
    int e = len;
    while (e > s && (raw_name[e-1] == ' ' || raw_name[e-1] == '\t')) e--;
    int copy = e - s;
    if (copy > (int)sizeof(name) - 1) copy = sizeof(name) - 1;
    if (copy > 0) memcpy(name, raw_name + s, copy);
    name[copy] = '\0';
    if (!name[0]) {
        chcreate_status("Enter a channel name", false);
        return;
    }

    // PSK: blank means "create with random key"; non-blank must base64-decode
    // to exactly 1, 16, or 32 bytes.
    const uint8_t *psk_ptr = nullptr;
    size_t         psk_len = 0;
    uint8_t        psk_buf[32];

    if (s_chcreate_psk) {
        const char *raw_psk = lv_textarea_get_text(s_chcreate_psk);
        char cleaned[80];
        size_t clen = b64_clean(raw_psk, cleaned, sizeof(cleaned));
        if (clen > 0) {
            size_t olen = 0;
            int rc = mbedtls_base64_decode(psk_buf, sizeof(psk_buf), &olen,
                                           (const unsigned char *)cleaned, clen);
            if (rc != 0) {
                chcreate_status("PSK is not valid base64", false);
                return;
            }
            if (olen != 1 && olen != 16 && olen != 32) {
                chcreate_status("Decoded PSK must be 1, 16, or 32 bytes", false);
                return;
            }
            psk_ptr = psk_buf;
            psk_len = olen;
        }
    }

    bool mqtt_uplink = s_chcreate_uplink && lv_obj_has_state(s_chcreate_uplink, LV_STATE_CHECKED);
    bool mqtt_downlink = s_chcreate_downlink && lv_obj_has_state(s_chcreate_downlink, LV_STATE_CHECKED);

    if (!channel_create(name, psk_ptr, psk_len, mqtt_uplink, mqtt_downlink)) {
        chcreate_status("No free channel slot (delete one first)", false);
        return;
    }
    rebuild_chats_list();
    chcreate_close();
}

static void chcreate_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_current_target(e);
    keyboard_attach(ta);
    keyboard_show();
}

static void chcreate_open()
{
    chcreate_close();

    lv_obj_t *scr = lv_screen_active();
    s_chcreate_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chcreate_overlay);
    lv_obj_set_size(s_chcreate_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_chcreate_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_chcreate_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_chcreate_overlay, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_chcreate_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_chcreate_overlay);

    const int card_h = landscape_active() ? (SCR_H - keyboard_height() - 16) : 430;
    lv_obj_t *card = lv_obj_create(s_chcreate_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 40, card_h);
    // Sit just above the keyboard so both are visible.
    int card_y = SCR_H - keyboard_height() - card_h - 8;
    if (card_y < 8) card_y = 8;
    lv_obj_set_pos(card, 20, card_y);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    // Landscape has limited vertical space with keyboard shown; keep the
    // modal scrollable so all fields/toggles remain reachable.
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    int y = 0;

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "New / join channel");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, y);
    y += 28;

    s_chcreate_name = lv_textarea_create(card);
    lv_obj_set_size(s_chcreate_name, lv_pct(100), 44);
    lv_obj_align(s_chcreate_name, LV_ALIGN_TOP_LEFT, 0, y);
    lv_textarea_set_one_line(s_chcreate_name, true);
    lv_textarea_set_max_length(s_chcreate_name, 11);
    lv_textarea_set_placeholder_text(s_chcreate_name, "Channel name (max 11 chars)");
    lv_obj_set_style_bg_color(s_chcreate_name, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(s_chcreate_name, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(s_chcreate_name, 0, 0);
    lv_obj_set_style_radius(s_chcreate_name, 0, 0);
    lv_obj_set_style_anim_duration(s_chcreate_name, 0, LV_PART_CURSOR);
    lv_obj_add_event_cb(s_chcreate_name, chcreate_focus_cb, LV_EVENT_FOCUSED, nullptr);
    y += 50;

    lv_obj_t *psk_hint = lv_label_create(card);
    lv_label_set_text(psk_hint,
                      "Key (base64). Blank = new random key. "
                      "Type a key to join an existing channel.");
    lv_label_set_long_mode(psk_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(psk_hint, lv_pct(100));
    lv_obj_set_style_text_color(psk_hint, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(psk_hint, &lv_font_montserrat_16, 0);
    lv_obj_align(psk_hint, LV_ALIGN_TOP_LEFT, 0, y);
    y += 56;

    s_chcreate_psk = lv_textarea_create(card);
    lv_obj_set_size(s_chcreate_psk, lv_pct(100), 44);
    lv_obj_align(s_chcreate_psk, LV_ALIGN_TOP_LEFT, 0, y);
    lv_textarea_set_one_line(s_chcreate_psk, true);
    lv_textarea_set_max_length(s_chcreate_psk, 64);
    lv_textarea_set_placeholder_text(s_chcreate_psk, "(optional)  e.g. 1PG7OiApB1nwvP+rz05pAQ==");
    lv_obj_set_style_bg_color(s_chcreate_psk, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(s_chcreate_psk, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(s_chcreate_psk, 0, 0);
    lv_obj_set_style_radius(s_chcreate_psk, 0, 0);
    lv_obj_set_style_anim_duration(s_chcreate_psk, 0, LV_PART_CURSOR);
    lv_obj_add_event_cb(s_chcreate_psk, chcreate_focus_cb, LV_EVENT_FOCUSED, nullptr);
    y += 50;

    lv_obj_t *ul_row = lv_obj_create(card);
    lv_obj_remove_style_all(ul_row);
    lv_obj_set_size(ul_row, lv_pct(100), 34);
    lv_obj_align(ul_row, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_remove_flag(ul_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ul_lbl = lv_label_create(ul_row);
    lv_label_set_text(ul_lbl, "MQTT uplink");
    lv_obj_set_style_text_color(ul_lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(ul_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(ul_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    s_chcreate_uplink = lv_switch_create(ul_row);
    lv_obj_set_size(s_chcreate_uplink, 56, 28);
    lv_obj_align(s_chcreate_uplink, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_state(s_chcreate_uplink, LV_STATE_CHECKED);
    y += 38;

    lv_obj_t *dl_row = lv_obj_create(card);
    lv_obj_remove_style_all(dl_row);
    lv_obj_set_size(dl_row, lv_pct(100), 34);
    lv_obj_align(dl_row, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_remove_flag(dl_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *dl_lbl = lv_label_create(dl_row);
    lv_label_set_text(dl_lbl, "MQTT downlink");
    lv_obj_set_style_text_color(dl_lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(dl_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(dl_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    s_chcreate_downlink = lv_switch_create(dl_row);
    lv_obj_set_size(s_chcreate_downlink, 56, 28);
    lv_obj_align(s_chcreate_downlink, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_state(s_chcreate_downlink, LV_STATE_CHECKED);
    y += 42;

    s_chcreate_status = lv_label_create(card);
    lv_label_set_text(s_chcreate_status, "");
    lv_obj_set_width(s_chcreate_status, lv_pct(100));
    lv_label_set_long_mode(s_chcreate_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_chcreate_status, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(s_chcreate_status, &lv_font_montserrat_16, 0);
    lv_obj_align(s_chcreate_status, LV_ALIGN_TOP_LEFT, 0, y);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 130, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, chcreate_cancel_cb, LV_EVENT_CLICKED, nullptr);
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
    lv_obj_add_event_cb(ok, chcreate_ok_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, "Create / join");
    lv_obj_set_style_text_color(ol, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(ol, &lv_font_montserrat_16, 0);
    lv_obj_center(ol);

    keyboard_attach(s_chcreate_name);
    keyboard_show();
    lv_textarea_set_cursor_pos(s_chcreate_name, LV_TEXTAREA_CURSOR_LAST);
}

// "+" FAB on the Chats tab. Refuses to open the modal when every secondary
// channel slot is in use (the underlying channel_create would fail anyway).
static void chats_fab_clicked_cb(lv_event_t *)
{
    bool has_room = false;
    for (uint8_t i = 1; i < channels.getNumChannels(); i++) {
        if (channels.getByIndex(i).role == meshtastic_Channel_Role_DISABLED) {
            has_room = true;
            break;
        }
    }
    if (!has_room) {
        LOG_WARN("mcui: chats FAB: no free secondary channel slot");
        return;
    }
    chcreate_open();
}

// Add a section header label ("Channels" / "Direct")
static void add_section_header(const char *text)
{
    lv_obj_t *h = lv_label_create(s_chats_list);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_color(h, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(h, 6, 0);
    lv_obj_set_style_pad_left(h, 12, 0);
}

// Add one conversation card (channel or direct).
static void add_chat_card(ChatEntry *ent, const char *subtitle, uint16_t unread,
                          uint32_t accent_color)
{
    (void)accent_color;
    lv_obj_t *card = lv_obj_create(s_chats_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 54);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, ent);
    lv_obj_add_event_cb(card, chat_card_clicked, LV_EVENT_CLICKED, nullptr);
    // Long-press is supported on both DMs and channels (handler distinguishes).
    lv_obj_add_event_cb(card, chat_card_long_pressed, LV_EVENT_LONG_PRESSED, nullptr);

    // Title (+ unread count inline to avoid creating a separate badge object)
    char title_buf[56];
    if (unread > 0)
        snprintf(title_buf, sizeof(title_buf), "%s  (%u)", ent->title, (unsigned)(unread > 99 ? 99 : unread));
    else
        snprintf(title_buf, sizeof(title_buf), "%s", ent->title);

    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, title_buf);
    lv_obj_set_style_text_color(tl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(tl, 8, 2);
    lv_obj_set_width(tl, SCR_W - 20);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);

    // Subtitle / preview
    if (subtitle && subtitle[0]) {
        lv_obj_t *sl = lv_label_create(card);
        lv_label_set_text(sl, subtitle);
        lv_obj_set_style_text_color(sl, lv_color_hex(TH_TEXT2), 0);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
        lv_obj_set_pos(sl, 8, 24);
        lv_obj_set_width(sl, SCR_W - 20);
        lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
    }
}

// Pull a short preview string for a conversation into `out`.
static void fill_preview(const McConvId &id, char *out, size_t out_sz)
{
    McMessage last;
    if (!messages_last(id, last)) {
        snprintf(out, out_sz, "No messages yet");
        return;
    }
    const char *prefix = last.outgoing ? "You: " : "";
    snprintf(out, out_sz, "%s%s", prefix, last.text);
}

struct ConvGather {
    McConvId ids[MC_MAX_CONVERSATIONS];
    int n;
};
static void conv_gather_cb(const McConvId &id, void *ctx)
{
    ConvGather *g = (ConvGather *)ctx;
    if (g->n < (int)(sizeof(g->ids) / sizeof(g->ids[0]))) {
        g->ids[g->n++] = id;
    }
}

static void rebuild_chats_list()
{
    if (!s_chats_list) return;
    lv_obj_clean(s_chats_list);
    s_num_entries = 0;

    // ---- Section 1: Channels (pinned) --------------------------------------
    add_section_header("Channels");

    uint8_t nch = channels.getNumChannels();
    for (uint8_t i = 0; i < nch && s_num_entries < (int)(sizeof(s_entries)/sizeof(s_entries[0])); i++) {
        meshtastic_Channel &ch = channels.getByIndex(i);
        if (ch.role == meshtastic_Channel_Role_DISABLED) continue;

        ChatEntry *ent = &s_entries[s_num_entries++];
        ent->id = McConvId::channel(i);
        const char *name = channels.getName(i);
        if (!name || !name[0]) name = (i == 0) ? "Primary" : "Channel";
        strncpy(ent->title, name, sizeof(ent->title) - 1);
        ent->title[sizeof(ent->title) - 1] = '\0';

        char preview[80];
        fill_preview(ent->id, preview, sizeof(preview));
        uint16_t u = messages_unread(ent->id);
        add_chat_card(ent, preview, u, avatar_color_for(ent->id, ent->title));
    }

    // ---- Section 2: Direct conversations -----------------------------------
    ConvGather g;
    g.n = 0;
    messages_for_each_conv(conv_gather_cb, &g);

    bool any_direct = false;
    for (int i = 0; i < g.n; i++) {
        if (g.ids[i].kind != McConvId::DIRECT) continue;
        any_direct = true;
        break;
    }
    if (any_direct) {
        add_section_header("Direct");
    }

    for (int i = 0; i < g.n && s_num_entries < (int)(sizeof(s_entries)/sizeof(s_entries[0])); i++) {
        if (g.ids[i].kind != McConvId::DIRECT) continue;

        ChatEntry *ent = &s_entries[s_num_entries++];
        ent->id = g.ids[i];

        // Resolve display name from NodeDB
        const char *title = nullptr;
        char fallback[16];
        if (nodeDB) {
            auto *n = nodeDB->getMeshNode((NodeNum)g.ids[i].value);
            if (n && n->has_user) {
                if (n->user.long_name[0])
                    title = n->user.long_name;
                else if (n->user.short_name[0])
                    title = n->user.short_name;
            }
        }
        if (!title) {
            snprintf(fallback, sizeof(fallback), "!%08x", (unsigned)g.ids[i].value);
            title = fallback;
        }
        strncpy(ent->title, title, sizeof(ent->title) - 1);
        ent->title[sizeof(ent->title) - 1] = '\0';

        char preview[80];
        fill_preview(ent->id, preview, sizeof(preview));
        uint16_t u = messages_unread(ent->id);
        add_chat_card(ent, preview, u, avatar_color_for(ent->id, ent->title));
    }

    s_chats_last_tick = messages_change_tick();
}

lv_obj_t *chats_screen_create(lv_obj_t *parent)
{
    s_chats_page = make_page(parent);

    // Scrollable list container fills the whole page
    s_chats_list = lv_obj_create(s_chats_page);
    lv_obj_remove_style_all(s_chats_list);
    lv_obj_set_size(s_chats_list, SCR_W, PAGE_H);
    lv_obj_set_pos(s_chats_list, 0, 0);
    lv_obj_set_style_bg_color(s_chats_list, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_chats_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_chats_list, 8, 0);
    lv_obj_set_style_pad_row(s_chats_list, 6, 0);
    lv_obj_set_flex_flow(s_chats_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_chats_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chats_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_chats_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(s_chats_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    // Floating action button: opens the "New / join channel" modal. Lives on
    // the page (not the scroll list) so it stays put while the list scrolls.
    lv_obj_t *fab = lv_button_create(s_chats_page);
    lv_obj_set_size(fab, 56, 56);
    lv_obj_align(fab, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_set_style_bg_color(fab, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(fab, 0, 0);
    lv_obj_set_style_shadow_width(fab, 0, 0);
    lv_obj_set_style_shadow_color(fab, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(fab, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(fab, chats_fab_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_move_foreground(fab);

    lv_obj_t *fab_lbl = lv_label_create(fab);
    lv_label_set_text(fab_lbl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(fab_lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(fab_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(fab_lbl);

    // Note: the bubble chat view is created by McUI at root-screen level so
    // it can cover the tab bar area when the keyboard is up.

    // Force first build
    s_chats_last_tick = 0xFFFFFFFFu;
    rebuild_chats_list();
    return s_chats_page;
}

void chats_screen_tick()
{
    // Let the chat view process its own refresh (bubble updates)
    chatview_tick();

    if (!s_chats_list) return;
    if (chatview_is_open()) return;
    uint32_t t = messages_change_tick();
    if (t != s_chats_last_tick) {
        s_chats_rebuild_pending = true;
    }
    if (s_chats_rebuild_pending && !lv_obj_is_scrolling(s_chats_list)) {
        rebuild_chats_list();
        s_chats_rebuild_pending = false;
    }
}

// ============================================================================
//  Other (placeholder) screens
// ============================================================================

// nodes_screen_create() lives in McNodes.cpp

static lv_obj_t *s_maps_page = nullptr;
static lv_obj_t *s_maps_advert_switch = nullptr;
static lv_obj_t *s_maps_advert_note = nullptr;
static lv_obj_t *s_maps_intro = nullptr;
static lv_obj_t *s_maps_http = nullptr;
static lv_obj_t *s_maps_https = nullptr;
static lv_obj_t *s_maps_note = nullptr;
static uint32_t s_maps_last_refresh_ms = 0;

static bool maps_get_wifi_ip(char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return false;

    out[0] = '\0';

#if HAS_WIFI && !defined(ARCH_PORTDUINO)
    if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        if ((uint32_t)ip != 0) {
            snprintf(out, out_sz, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            return true;
        }
    }
#endif

    return false;
}

static lv_obj_t *add_maps_switch_row(lv_obj_t *parent, const char *label, bool initial, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
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
    if (initial)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb)
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return sw;
}

static void maps_update_advert_note()
{
    if (!s_maps_advert_note)
        return;

    if (position_advert_enabled()) {
        lv_label_set_text(s_maps_advert_note, "GPS or fixed position will be advertised to the mesh.");
    } else {
        lv_label_set_text(s_maps_advert_note, "Node info adverts continue, but position packets are not sent.");
    }
}

static void maps_position_advert_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_current_target(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (!position_advert_save(enabled)) {
        if (enabled)
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
        else
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        return;
    }
    maps_update_advert_note();
}

static void maps_refresh(bool force = false)
{
    if (!s_maps_page || !s_maps_intro || !s_maps_http || !s_maps_https || !s_maps_note)
        return;

    uint32_t now = millis();
    if (!force && (uint32_t)(now - s_maps_last_refresh_ms) < 1000)
        return;
    s_maps_last_refresh_ms = now;

    char ip[24];
    char http_url[64];
    char https_url[64];

    if (maps_get_wifi_ip(ip, sizeof(ip))) {
        snprintf(http_url, sizeof(http_url), "HTTP:  http://%s/position", ip);
        snprintf(https_url, sizeof(https_url), "HTTPS: https://%s/position", ip);
        lv_label_set_text(s_maps_intro, "Open this address on your phone to set the device position:");
        lv_label_set_text(s_maps_http, http_url);
        lv_label_set_text(s_maps_https, https_url);
        lv_label_set_text(s_maps_note, "If Chrome blocks phone location on HTTP, use the HTTPS address instead.");
    } else {
        lv_label_set_text(s_maps_intro, "Connect this device to WiFi to show the phone position page address.");
        lv_label_set_text(s_maps_http, "HTTP:  unavailable");
        lv_label_set_text(s_maps_https, "HTTPS: unavailable");
        lv_label_set_text(s_maps_note, "Once connected, open the shown /position address on your phone.");
    }
}

lv_obj_t *maps_screen_create(lv_obj_t *parent)
{
    s_maps_page = make_page(parent);

    lv_obj_t *title = lv_label_create(s_maps_page);
    lv_label_set_text(title, "Maps");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *card = lv_obj_create(s_maps_page);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 40, landscape_active() ? 320 : 380);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    s_maps_advert_switch = add_maps_switch_row(card, "Advertise position", position_advert_enabled(),
                                               maps_position_advert_changed_cb);

    s_maps_advert_note = lv_label_create(card);
    lv_obj_set_width(s_maps_advert_note, lv_pct(100));
    lv_label_set_long_mode(s_maps_advert_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_maps_advert_note, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_align(s_maps_advert_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_maps_advert_note, &lv_font_montserrat_16, 0);

    s_maps_intro = lv_label_create(card);
    lv_obj_set_width(s_maps_intro, lv_pct(100));
    lv_label_set_long_mode(s_maps_intro, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_maps_intro, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_align(s_maps_intro, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_maps_intro, &lv_font_montserrat_16, 0);

    s_maps_http = lv_label_create(card);
    lv_obj_set_width(s_maps_http, lv_pct(100));
    lv_label_set_long_mode(s_maps_http, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_maps_http, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_text_align(s_maps_http, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_maps_http, &lv_font_montserrat_16, 0);

    s_maps_https = lv_label_create(card);
    lv_obj_set_width(s_maps_https, lv_pct(100));
    lv_label_set_long_mode(s_maps_https, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_maps_https, lv_color_hex(TH_TAB_ACTIVE), 0);
    lv_obj_set_style_text_align(s_maps_https, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_maps_https, &lv_font_montserrat_16, 0);

    s_maps_note = lv_label_create(card);
    lv_obj_set_width(s_maps_note, lv_pct(100));
    lv_label_set_long_mode(s_maps_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_maps_note, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_align(s_maps_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_maps_note, &lv_font_montserrat_16, 0);

    s_maps_last_refresh_ms = 0;
    maps_update_advert_note();
    maps_refresh(true);
    return s_maps_page;
}

void maps_screen_tick()
{
    maps_refresh();
}

// settings_screen_create() lives in McSettings.cpp

} // namespace mcui

#endif
