// Color palette ported from MeshCore crowpanel_lvgl_chat/ui_theme.h
// Telegram-style dark theme, used by the portrait mcui.
#pragma once

#if HAS_TFT && USE_MCUI

// ---- Backgrounds ----
#define TH_BG           0x0E1621 // main screen bg
#define TH_SURFACE      0x17212B // cards / list items
#define TH_SURFACE2     0x1E2C3A // raised surface
#define TH_INPUT        0x242F3D // text input bg
#define TH_BORDER       0x2B3B4D
#define TH_SEPARATOR    0x1C2A36

// ---- Accent / brand ----
#define TH_ACCENT       0x3390EC // primary blue
#define TH_ACCENT_LIGHT 0x5EB5F7
#define TH_GREEN        0x6DC264
#define TH_RED          0xE05555
#define TH_AMBER        0xF5A623

// ---- Text ----
#define TH_TEXT         0xF5F5F5
#define TH_TEXT2        0x8696A0 // secondary (SNR/RSSI, timestamps)
#define TH_TEXT3        0x6C7883 // tertiary / placeholders

// ---- Chat bubbles ----
#define TH_BUBBLE_OUT   0x2B5278 // outgoing (self)
#define TH_BUBBLE_IN    0x182533 // incoming

// ---- Tab bar ----
#define TH_TAB_BG       0x0E1621
#define TH_TAB_ACTIVE   0x3390EC
#define TH_TAB_INACTIVE 0x546E7A

// ---- Badge ----
#define TH_BADGE_BG     0xE05555

// ---- Avatar palette ----
// Used by avatar_color_for() in McScreens.cpp to deterministically pick a
// stable colored circle per conversation (channel or DM).
#define TH_AVATAR_PALETTE_COUNT 12
#define TH_AVATAR_PALETTE_LIST  \
    0x3390EC, \
    0x4CAF50, \
    0xAB47BC, \
    0xFF7043, \
    0x26A69A, \
    0xEC407A, \
    0xFFB300, \
    0x5C6BC0, \
    0x29B6F6, \
    0xEF5350, \
    0x9CCC65, \
    0x8D6E63

#endif // HAS_TFT && USE_MCUI
