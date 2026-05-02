#if HAS_TFT && USE_MCUI

#include "McChatView.h"
#include "../McKeyboard.h"
#include "../McStatusBar.h"
#include "../McTheme.h"
#include "../McUI.h"
#include "../data/McMessages.h"
#include "../data/McSender.h"
#include "configuration.h"

#include "mesh/NodeDB.h"

#include <cstdio>
#include <cstring>
#include <time.h>

namespace mcui {

// ---- Geometry --------------------------------------------------------------
// The chat view is parented to the root screen and occupies the area between
// the status bar and the tab bar: y = STATUS_H .. SCR_H - TAB_H.
// When the keyboard is visible it covers the tab bar and the lower portion of
// this area — the input row is repositioned just above the keyboard then.
static constexpr int HEADER_H = 44;
// Taller input row so the textarea has more comfortable finger room, and
// KB_GAP leaves a clear strip between the input row and the keyboard so the
// textarea isn't visually "sitting inside" the keyboard top edge.
static constexpr int INPUT_ROW_H = 56;
static constexpr int KB_GAP = 8;
static int chat_top_y() { return landscape_active() ? 0 : STATUS_H; }
static int chat_h() { return landscape_active() ? (SCR_H - TAB_H) : PAGE_H; }
static bool merged_landscape_header() { return landscape_active(); }

static lv_obj_t *s_root = nullptr;        // full-page container
static lv_obj_t *s_header = nullptr;
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_back_btn = nullptr;
static lv_obj_t *s_time = nullptr;
static lv_obj_t *s_bubbles = nullptr;     // scrollable bubble container
static lv_obj_t *s_input_row = nullptr;
static lv_obj_t *s_textarea = nullptr;
static lv_obj_t *s_send_btn = nullptr;
static McConvId s_current = McConvId::none();
static uint32_t s_last_tick = 0;
static bool s_rebuild_pending = false;

// Edge-swipe back gesture state. Records where the most recent press began
// so the GESTURE handler can tell whether a left swipe originated from the
// right edge (the only trigger condition for the back-to-main gesture).
static int16_t s_press_start_x = -1;
// How close to the right edge a press must start to arm the back swipe.
static constexpr int16_t EDGE_SWIPE_MARGIN = 40;

static void rebuild_bubbles();
static void refresh_header_time();
static void update_header_layout();

static void update_chat_frame()
{
    if (!s_root || !s_header || !s_bubbles)
        return;

    lv_obj_set_size(s_root, SCR_W, chat_h());
    lv_obj_set_pos(s_root, 0, chat_top_y());
    lv_obj_set_size(s_header, SCR_W, HEADER_H);
    lv_obj_set_width(s_bubbles, SCR_W);
    if (s_input_row)
        lv_obj_set_width(s_input_row, SCR_W);
}

// Recompute input row + bubble container geometry based on keyboard state.
// When the keyboard is visible, the input row sits directly above it and the
// bubble area shrinks accordingly.
static void layout_for_keyboard(bool kb_visible)
{
    if (!s_root || !s_bubbles || !s_input_row) return;

    int input_y_screen;
    if (kb_visible) {
        // Keyboard occupies [SCR_H - keyboard_height(), SCR_H]. Input sits just above it
        // with a small visible gap so they don't touch — otherwise the
        // textarea looks like it's clipped into the keyboard's top edge.
        input_y_screen = SCR_H - keyboard_height() - INPUT_ROW_H - KB_GAP;
    } else {
        // No keyboard: input sits just above the tab bar.
        input_y_screen = SCR_H - TAB_H - INPUT_ROW_H;
    }
    int input_y_local = input_y_screen - chat_top_y();
    lv_obj_set_pos(s_input_row, 0, input_y_local);

    // Bubbles fill the area between header and input row.
    int bubble_h = input_y_local - HEADER_H;
    if (bubble_h < 0) bubble_h = 0;
    lv_obj_set_pos(s_bubbles, 0, HEADER_H);
    lv_obj_set_height(s_bubbles, bubble_h);
}

static void back_cb(lv_event_t *)
{
    chatview_hide();
}

static void refresh_header_time()
{
    if (!s_time)
        return;

    if (!merged_landscape_header()) {
        lv_obj_add_flag(s_time, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char buf[8] = "--:--";
    time_t now = time(nullptr);
    if (now >= 1700000000) {
        struct tm lt;
        localtime_r(&now, &lt);
        snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    }
    lv_label_set_text(s_time, buf);
    lv_obj_remove_flag(s_time, LV_OBJ_FLAG_HIDDEN);
}

static void update_header_layout()
{
    if (!s_header || !s_back_btn || !s_title || !s_time)
        return;

    if (merged_landscape_header()) {
        lv_obj_align(s_back_btn, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 0);
        lv_obj_align(s_time, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_remove_flag(s_time, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_align(s_back_btn, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(s_time, LV_OBJ_FLAG_HIDDEN);
    }
}

// Captures the x coordinate of a press-start inside the chat view so the
// subsequent GESTURE event can tell whether the swipe originated from the
// right edge. Also dismisses the keyboard when a tap lands in the "dismiss
// zone" (bubbles or header) — anywhere that isn't the input row or the
// keyboard itself. This is how the phone keyboard UX works.
//
// LVGL 9 does NOT bubble LV_EVENT_PRESSED by default; this callback is
// attached to the objects whose subtree we care about (s_bubbles, s_header),
// not s_root. LV_OBJ_FLAG_EVENT_BUBBLE is set on their children (including
// dynamically created bubble rows) so child presses reach us.
static void press_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_press_start_x = (int16_t)p.x;

    // If the keyboard is visible and this press wasn't on the input row,
    // hide it. current_target is s_bubbles or s_header (the object we
    // attached to), which is always a "dismiss zone".
    if (keyboard_is_visible()) {
        keyboard_hide();
        layout_for_keyboard(false);
    }
}

// Press handler for the input row — records start x but does NOT dismiss
// the keyboard (taps on the textarea / send button should keep typing).
static void press_input_cb(lv_event_t *)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_press_start_x = (int16_t)p.x;
}

// Right-edge-to-left swipe → back to main tab view.
//
// Attached to s_root with LV_OBJ_FLAG_GESTURE_BUBBLE cleared on s_root so
// the LVGL gesture walk stops there (by default it would walk all the way
// to the screen, past s_root, and our handler would never fire).
static void gesture_cb(lv_event_t *)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_LEFT) return;
    // Only trigger if the press started in the right-edge strip.
    if (s_press_start_x < 0 || s_press_start_x < (SCR_W - EDGE_SWIPE_MARGIN)) return;
    // Consume so we don't re-fire mid-gesture.
    s_press_start_x = -1;
    // Cancel the press in the indev so no tap is synthesized on release.
    lv_indev_wait_release(indev);
    chatview_hide();
}

static void ta_focus_cb(lv_event_t *)
{
    keyboard_attach(s_textarea);
    keyboard_show();
    layout_for_keyboard(true);
    // The bubble area just shrank (input row moved up above the keyboard),
    // so the newest messages were pushed below the new bottom edge. Scroll
    // back to the tail so the latest are always visible above the keyboard.
    if (s_bubbles) {
        lv_obj_update_layout(s_bubbles);
        lv_obj_scroll_to_y(s_bubbles, LV_COORD_MAX, LV_ANIM_OFF);
    }
}
// Defocus intentionally does NOT hide the keyboard. The button matrix steals
// focus on every tap which would otherwise dismiss the keyboard mid-typing.
// The keyboard is dismissed only via Back or the OK key (handled in McKeyboard).

static void send_cb(lv_event_t *)
{
    const char *txt = lv_textarea_get_text(s_textarea);
    if (!txt || !txt[0]) return;
    if (!s_current.is_valid()) return;
    if (sender_send_text(s_current, txt)) {
        lv_textarea_set_text(s_textarea, "");
        // Intentionally DO NOT call rebuild_bubbles() here. We're in the
        // middle of LVGL's event dispatch (from LV_EVENT_RELEASED on the
        // send button or LV_EVENT_READY from the keyboard OK key), and
        // lv_obj_clean() tears down child objects that the indev's scroll
        // and gesture state may still be referencing — causing the UI to
        // hang / go unresponsive on the following touch.
        //
        // sender_send_text() already bumped messages_change_tick() via
        // messages_append(), so chatview_tick() will rebuild on the next
        // UI loop pass (a few ms later), safely outside event dispatch.
    }
}

// Build one bubble row inside the bubble container
static void add_bubble(const McMessage &m)
{
    lv_obj_t *row = lv_obj_create(s_bubbles);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    // Let presses on the row (and its bubble/label descendants) bubble up
    // to s_bubbles so the tap-to-dismiss-keyboard handler fires.
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_remove_style_all(bubble);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_width(bubble, lv_pct(82));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_pad_row(bubble, 2, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    if (m.outgoing) {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(TH_BUBBLE_OUT), 0);
        lv_obj_align(bubble, LV_ALIGN_RIGHT_MID, 0, 0);
    } else {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(TH_BUBBLE_IN), 0);
        lv_obj_align(bubble, LV_ALIGN_LEFT_MID, 0, 0);
    }

    // Header line for incoming bubbles: sender short name + time
    char head[64] = {0};
    if (!m.outgoing) {
        char sender_buf[16] = "?";
        const char *sender = sender_buf;
        if (nodeDB) {
            auto *n = nodeDB->getMeshNode(m.from_node);
            if (n && n->has_user) {
                if (n->user.short_name[0] || n->user.long_name[0]) {
                    sender = n->user.short_name[0] ? n->user.short_name : n->user.long_name;
                }
            }
        }
        if (sender == sender_buf && m.from_node != 0) {
            snprintf(sender_buf, sizeof(sender_buf), "!%08x", (unsigned)m.from_node);
        }
        char ts[8] = "--:--";
        if (m.timestamp > 1700000000) {
            struct tm lt; time_t t = m.timestamp; localtime_r(&t, &lt);
            snprintf(ts, sizeof(ts), "%02d:%02d", lt.tm_hour, lt.tm_min);
        }
        snprintf(head, sizeof(head), "%s  %s", sender, ts);
        lv_obj_t *h = lv_label_create(bubble);
        lv_label_set_text(h, head);
        lv_obj_set_style_text_color(h, lv_color_hex(TH_ACCENT_LIGHT), 0);
    }

    // Body
    lv_obj_t *body = lv_label_create(bubble);
    lv_label_set_text(body, m.text);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(TH_TEXT), 0);

    // Footer (SNR for incoming, status for outgoing)
    char foot[48] = {0};
    uint32_t foot_color = TH_TEXT3;
    if (m.outgoing) {
        if (!m.delivered) {
            snprintf(foot, sizeof(foot), "sent");
        } else if (m.ack_failed) {
            snprintf(foot, sizeof(foot), LV_SYMBOL_CLOSE " failed");
            foot_color = 0xE06060; // muted red
        } else if (m.packet_id == 0) {
            snprintf(foot, sizeof(foot), "sent");
        } else {
            snprintf(foot, sizeof(foot), LV_SYMBOL_OK LV_SYMBOL_OK " acknowledged");
        }
    } else if (m.snr != 0 || m.rssi != 0) {
        snprintf(foot, sizeof(foot), "SNR %.1f  RSSI %d", m.snr, (int)m.rssi);
    }
    if (foot[0]) {
        lv_obj_t *f = lv_label_create(bubble);
        lv_label_set_text(f, foot);
        lv_obj_set_style_text_color(f, lv_color_hex(foot_color), 0);
        lv_obj_set_style_text_font(f, &lv_font_montserrat_16, 0);
    }
}

static void rebuild_bubbles()
{
    if (!s_bubbles) return;
    lv_obj_clean(s_bubbles);
    if (!s_current.is_valid()) return;

    McMessage buf[MC_MAX_MSGS_PER_CONV];
    size_t n = messages_snapshot(s_current, buf, MC_MAX_MSGS_PER_CONV);
    for (size_t i = 0; i < n; i++) add_bubble(buf[i]);

    // Scroll to bottom (newest)
    lv_obj_scroll_to_y(s_bubbles, LV_COORD_MAX, LV_ANIM_OFF);
}

lv_obj_t *chatview_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    // Parented to the root screen so we can place the input row above the
    // keyboard (which covers the tab bar while visible). We cover the whole
    // area between the status bar and the tab bar.
    lv_obj_set_size(s_root, SCR_W, chat_h());
    lv_obj_set_pos(s_root, 0, chat_top_y());
    lv_obj_set_style_bg_color(s_root, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    // LVGL's gesture-bubble walk stops at the first ancestor WITHOUT the
    // LV_OBJ_FLAG_GESTURE_BUBBLE flag. All children get the flag by default,
    // so clearing it on s_root makes gesture events land on s_root when any
    // descendant of the chat view is pressed. Without this the gesture
    // walks all the way up to the screen and our handler never fires.
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_root, gesture_cb, LV_EVENT_GESTURE, nullptr);

    // Header bar
    s_header = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_header);
    lv_obj_set_size(s_header, SCR_W, HEADER_H);
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_style_bg_color(s_header, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_header, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_header, LV_OBJ_FLAG_SCROLLABLE);

    s_back_btn = lv_label_create(s_header);
    lv_label_set_text(s_back_btn, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(s_back_btn, lv_color_hex(TH_ACCENT), 0);
    lv_obj_align(s_back_btn, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_back_btn, back_cb, LV_EVENT_CLICKED, nullptr);

    s_title = lv_label_create(s_header);
    lv_label_set_text(s_title, "");
    lv_obj_set_style_text_color(s_title, lv_color_hex(TH_TEXT), 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_title, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_time = lv_label_create(s_header);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_style_text_color(s_time, lv_color_hex(TH_TEXT2), 0);
    lv_obj_add_flag(s_time, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Dismiss-keyboard / swipe-back press handler on the header. Events on
    // the back button and title bubble up here via LV_OBJ_FLAG_EVENT_BUBBLE
    // set above.
    lv_obj_add_event_cb(s_header, press_cb, LV_EVENT_PRESSED, nullptr);

    // Bubble container (size + pos set by layout_for_keyboard() below)
    s_bubbles = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_bubbles);
    lv_obj_set_width(s_bubbles, SCR_W);
    lv_obj_set_style_bg_color(s_bubbles, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_bubbles, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_bubbles, 8, 0);
    lv_obj_set_style_pad_row(s_bubbles, 6, 0);
    lv_obj_set_flex_flow(s_bubbles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_bubbles, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_bubbles, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_bubbles, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(s_bubbles, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    // Dismiss-keyboard / swipe-back press handler on the bubble area.
    // Dynamically created bubble rows get LV_OBJ_FLAG_EVENT_BUBBLE in
    // add_bubble() so taps on individual rows bubble up to us here.
    lv_obj_add_event_cb(s_bubbles, press_cb, LV_EVENT_PRESSED, nullptr);

    // Input row (position set by layout_for_keyboard() below)
    s_input_row = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_input_row);
    lv_obj_set_size(s_input_row, SCR_W, INPUT_ROW_H);
    lv_obj_set_style_bg_color(s_input_row, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_input_row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_input_row, 6, 0);
    lv_obj_remove_flag(s_input_row, LV_OBJ_FLAG_SCROLLABLE);
    // Input row press handler records start x for swipe-back detection
    // but intentionally does NOT hide the keyboard (we want taps on the
    // textarea / send button to keep typing active).
    lv_obj_add_event_cb(s_input_row, press_input_cb, LV_EVENT_PRESSED, nullptr);

    // Bigger send button for a more forgiving hit target. The textarea
    // takes the rest of the row width after the button + gap.
    constexpr int SEND_BTN_W = 80;
    constexpr int SEND_BTN_MARGIN = 6;
    constexpr int TA_MARGIN = 6;

    s_textarea = lv_textarea_create(s_input_row);
    lv_obj_set_size(s_textarea,
                    SCR_W - TA_MARGIN - SEND_BTN_W - SEND_BTN_MARGIN * 2,
                    INPUT_ROW_H - 12);
    lv_obj_set_pos(s_textarea, TA_MARGIN, 6);
    lv_textarea_set_one_line(s_textarea, true);
    lv_textarea_set_placeholder_text(s_textarea, "Message...");
    lv_obj_set_style_bg_color(s_textarea, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(s_textarea, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(s_textarea, 0, 0);
    lv_obj_set_style_radius(s_textarea, 0, 0);
    // Disable the blinking cursor animation — it invalidates the textarea
    // bounding box every blink, forcing an SPI-bound partial flush every
    // ~400 ms. On this panel the flush is expensive enough that the cursor
    // blink was the dominant cause of typing lag. A non-blinking cursor
    // still renders, it just doesn't animate.
    lv_obj_set_style_anim_duration(s_textarea, 0, LV_PART_CURSOR);
    // Let presses on the textarea reach the input row's press_input_cb so
    // swipe-start x is recorded if a gesture begins here.
    lv_obj_add_flag(s_textarea, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_textarea, ta_focus_cb, LV_EVENT_FOCUSED, nullptr);
    // LV_EVENT_READY is fired by the keyboard's OK key — route it to the
    // same handler as the Send button so either path works.
    lv_obj_add_event_cb(s_textarea, send_cb, LV_EVENT_READY, nullptr);

    s_send_btn = lv_button_create(s_input_row);
    lv_obj_set_size(s_send_btn, SEND_BTN_W, INPUT_ROW_H - 12);
    lv_obj_set_pos(s_send_btn, SCR_W - SEND_BTN_W - SEND_BTN_MARGIN, 6);
    lv_obj_set_style_bg_color(s_send_btn, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(s_send_btn, 0, 0);
    // Use RELEASED (not CLICKED) so a touch is committed as soon as the
    // finger lifts, even if the press period was very short or had slight
    // jitter. LV_EVENT_CLICKED can be missed on a slow-refreshing display
    // because the press-to-release window may cross a scroll-threshold
    // when the indev read is delayed by SPI contention.
    lv_obj_add_event_cb(s_send_btn, send_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *sl = lv_label_create(s_send_btn);
    lv_label_set_text(sl, LV_SYMBOL_OK " Send");
    lv_obj_set_style_text_color(sl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
    lv_obj_center(sl);

    update_chat_frame();
    update_header_layout();
    refresh_header_time();

    // Initial layout (no keyboard)
    layout_for_keyboard(false);

    return s_root;
}

void chatview_open(const McConvId &id, const char *title)
{
    if (!s_root) return;
    s_current = id;
    lv_label_set_text(s_title, title ? title : "");
    lv_textarea_set_text(s_textarea, "");
    statusbar_set_visible(!merged_landscape_header());
    update_chat_frame();
    update_header_layout();
    refresh_header_time();
    rebuild_bubbles();
    messages_mark_read(id);
    s_last_tick = messages_change_tick();
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);

    // Auto-show keyboard so the user can start typing immediately.
    keyboard_attach(s_textarea);
    keyboard_show();
    layout_for_keyboard(true);
    // Scroll to the newest message so it's visible above the keyboard.
    if (s_bubbles) {
        lv_obj_update_layout(s_bubbles);
        lv_obj_scroll_to_y(s_bubbles, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void chatview_hide()
{
    if (!s_root) return;
    keyboard_hide();
    statusbar_set_visible(true);
    update_chat_frame();
    update_header_layout();
    layout_for_keyboard(false);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_current = McConvId::none();
}

bool chatview_is_open()
{
    return s_root && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void chatview_tick()
{
    if (!chatview_is_open()) return;
    refresh_header_time();
    uint32_t t = messages_change_tick();
    if (t != s_last_tick) {
        s_last_tick = t;
        s_rebuild_pending = true;
    }
    if (s_rebuild_pending) {
        // Avoid expensive list teardown/rebuild while finger is actively
        // scrolling; apply once scrolling settles.
        if (!lv_obj_is_scrolling(s_bubbles)) {
            rebuild_bubbles();
            if (s_current.is_valid()) messages_mark_read(s_current);
            s_rebuild_pending = false;
        }
    }
}

McConvId chatview_current() { return s_current; }

} // namespace mcui

#endif
