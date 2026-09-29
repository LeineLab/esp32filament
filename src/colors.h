#pragma once

// ── Colour palette for the ESP32 filament station UI ──────────────────────────
// All values are 24-bit RGB suitable for lv_color_hex().
// Edit here to retheme the entire UI. Same palette as esp32rental for a
// consistent look across MakerSpaceAPI's own hardware terminals.

// ── Backgrounds ───────────────────────────────────────────────────────────────
#define CLR_BG              0x1a1a2e   // base screen surface
#define CLR_BG_RAISED       0x16213e   // elevated surface: cards, overlays
#define CLR_BG_TRACK        0x666666   // progress bar track

// ── Borders ───────────────────────────────────────────────────────────────────
#define CLR_BORDER          0x333366   // subtle border
#define CLR_ACCENT          0x4a90d9   // accent blue: highlight text, primary action

// ── Text ──────────────────────────────────────────────────────────────────────
#define CLR_TEXT_PRIMARY    0xe0e0e0   // main labels and titles
#define CLR_TEXT_SECONDARY  0xaaaaaa   // hints, secondary info
#define CLR_TEXT_MUTED      0x999999   // very faint placeholder
#define CLR_TEXT_BODY       0xcccccc   // card / dialog body text

// ── Status ────────────────────────────────────────────────────────────────────
#define CLR_SUCCESS         0x2ecc71   // green: checked in, WiFi ok
#define CLR_SUCCESS_BG      0x145214   // success card background
#define CLR_WARNING         0xff9900   // orange: attention (e.g. weight not on tag)
#define CLR_ERROR           0xff6b6b   // red: error text, error border
#define CLR_ERROR_BG        0x521414   // error card background
#define CLR_INFO            0x3498db   // blue: secondary action (Ausbuchen)

// ── Buttons ───────────────────────────────────────────────────────────────────
#define CLR_BTN_SECONDARY   0x555577   // muted: cancel/back buttons
#define CLR_BTN_CHECKIN     0x2ecc71   // green: Einbuchen
#define CLR_BTN_CHECKOUT    0x4a90d9   // blue: Ausbuchen
