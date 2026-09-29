#pragma once
#include <lvgl.h>
#include "tag_types.h"

enum class AppState {
    IDLE,           // "place spool on reader"
    WEIGHT_PICK,    // tag had no weight encoded — ask before calling the API at all
    PROCESSING,     // HTTP call in flight
    RESULT,         // deterministic outcome (checked in/out) or an error — auto-returns to IDLE
    MANUAL_CHOICE,  // no-serial tag: show parsed data + stock, wait for Einbuchen/Ausbuchen tap

    // Test-tag wizard ("Programmieren", entered from IDLE) — writes a freshly
    // generated test tag in one of the four supported formats to two
    // physical NFC tags (a spool's two faces). Never touches the
    // MakerSpaceAPI backend at all — purely an NFC provisioning tool. See
    // README.md's "Testtags programmieren" section.
    PROG_FORMAT,        // pick Bambu/Creality/OpenSpool/OpenTag3D
    PROG_BASE_MATERIAL, // pick PLA/PETG/ABS/... (paged)
    PROG_MODIFIER,      // pick a finish/variant (Silk/Luminous/Stardust/... or none, paged)
    PROG_COLOR,         // pick from a color palette (paged)
    PROG_BRAND,         // pick a manufacturer name — only for formats with a real, writable brand field
    PROG_WEIGHT,        // pick a weight preset (only for formats with a weight field)
    PROG_CONFIRM,       // summary + generated serial (if any) — "Schreiben" or "Abbrechen"
    PROG_WRITE_A,   // waiting for the first physical tag
    PROG_WRITE_B,   // waiting for the second physical tag
    PROG_RESULT,    // a write attempt failed — "Erneut versuchen" or "Abbrechen"
    PROG_DONE,      // both tags written and verified — auto-returns to IDLE
};

// Build all LVGL screens/widgets once.
void ui_init(lv_disp_t *disp);

// Called from the main loop whenever a freshly parsed tag comes off the NFC queue.
void ui_on_tag_scanned(const FilamentTagData &tag);

// Periodic update — handles state auto-timeouts. Call every loop iteration.
void ui_tick();

// Called from the raw touch-read callback (main.cpp) whenever a touch is
// detected anywhere on the panel, regardless of whether it lands on a real
// widget — wakes the backlight back to BRIGHTNESS_FULL while on the IDLE
// screen (the only screen that ever dims, see ui_tick()) even if the touch
// misses prog_enter_btn entirely, since a dimmed-to-near-black screen makes
// that button's exact position hard to see in the first place. A no-op in
// every other state (brightness there is already full, restored on
// transition — see set_all_hidden()).
void ui_on_touch_activity();

// OTA progress screen — call from ArduinoOTA callbacks. percent 0-100.
void ui_show_ota(int percent);

// WiFi status indicator (top-right dot) — call whenever WiFi state changes.
void ui_set_wifi_connected(bool connected);

AppState ui_get_state();
