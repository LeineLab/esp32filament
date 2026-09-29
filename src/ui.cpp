#include "ui.h"
#include "config.h"
#include "colors.h"
#include "strings.h"
#include "api.h"
#include "led.h"
#include "nfc_reader.h"
#include "serial_gen.h"
#include "display.h"  // extern LGFX gfx — needed for the idle-dim brightness control below

// Fallback for a config.h created before the test-tag wizard existed (this
// project's config.h is gitignored/user-maintained — see
// config.h.example's own copy of this constant and README.md's
// "Testtags programmieren" section; never assume an existing config.h has
// been regenerated just because a new feature wants a new setting).
#ifndef TIMEOUT_PROGRAM_IDLE_MS
#define TIMEOUT_PROGRAM_IDLE_MS 60000
#endif
// Same reasoning — added along with the idle-dim feature itself, see
// set_all_hidden()/ui_tick() below and config.h.example's own copy.
#ifndef TIMEOUT_DIM_MS
#define TIMEOUT_DIM_MS 20000
#endif

// When LVGL_EXT_FONTS is defined (see platformio.ini — already on, fonts are
// pre-generated in src/fonts/ by scripts/gen_fonts.sh), use the custom
// extended-Latin fonts so umlauts render; otherwise fall back to LVGL's
// built-in ASCII-only ones (see lv_conf.h for why the built-ins can't do umlauts).
#ifdef LVGL_EXT_FONTS
  #define MY_FONT_14 (&montserrat_ext_14)
  #define MY_FONT_16 (&montserrat_ext_16)
  #define MY_FONT_20 (&montserrat_ext_20)
#else
  #define MY_FONT_14 (&lv_font_montserrat_14)
  #define MY_FONT_16 (&lv_font_montserrat_16)
  #define MY_FONT_20 (&lv_font_montserrat_20)
#endif

// ── State ────────────────────────────────────────────────────────────────────

static AppState g_state = AppState::IDLE;
static unsigned long g_state_entered_at = 0;

static FilamentTagData g_pending_tag;      // the tag currently being processed
static int g_manual_current_stock = 0;     // for MANUAL_CHOICE

// ── Widgets ──────────────────────────────────────────────────────────────────

static lv_obj_t *scr;
static lv_obj_t *wifi_dot;
static lv_obj_t *waiting_label;
static lv_obj_t *info_line1;     // "Marke Typ"
static lv_obj_t *info_line2_row; // flex row: [color_swatch] [info_line2] — see show_tag_info()
static lv_obj_t *color_swatch;   // small colored square next to the color hex value, hidden when the tag's color isn't a parseable "#RRGGBB"
static lv_obj_t *info_line2;     // "Farbe · Gewicht g"
static lv_obj_t *info_line3;     // serial, or "Im Bestand: N"
static lv_obj_t *banner_label; // big status text (colored)
static lv_obj_t *spinner;
static lv_obj_t *weight_btn_row;
static lv_obj_t *choice_btn_row;
static lv_obj_t *checkin_btn;   // child of choice_btn_row — individually
static lv_obj_t *checkout_btn;  // shown/hidden by set_choice_buttons() below

// Test-tag wizard ("Programmieren", program mode) — see the AppState::PROG_*
// doc comments in ui.h and README.md's "Testtags programmieren" section.
static lv_obj_t *prog_enter_btn;       // "Programmieren" — visible only on IDLE
static lv_obj_t *prog_title;
// A single scrollable, vertically-stacked list of full-width rows (format/
// material/modifier/color/brand) — replaces an earlier fixed 8-button-grid-
// plus-◀/▶-pager design (see git history) that was dropped after real-
// hardware use: the 70px-wide grid cells cut off longer option names, and
// paging through e.g. 16 colors 8-at-a-time got unwieldy. Rows are created
// fresh (lv_obj_clean() + rebuild) by prog_refresh_list() every time a step
// is (re)entered — there's no fixed row count to pre-allocate like the old
// grid's 8 static buttons, since the item count varies per step (4..16).
static lv_obj_t *prog_list;
static lv_obj_t *prog_cancel_btn;      // lone Abbrechen (FORMAT/BASE_MATERIAL/MODIFIER/COLOR/BRAND/WRITE_A/WRITE_B)
static lv_obj_t *prog_confirm_btn_row; // "Schreiben" / "Abbrechen"
static lv_obj_t *prog_retry_btn_row;   // "Erneut versuchen" / "Abbrechen" — a plain write failure, not "already written"
// A single full-width (304px) button shown ADDITIONALLY, right above
// prog_retry_btn_row, only for the "already written" screen — see
// enter_prog_already_written(). Deliberately full-width rather than a third
// button in a row of three narrow ones (96px — far narrower than every other
// button in this UI, and on a board where touch calibration already needs
// real per-unit correction, see config.h's TOUCH_*_INVERTED) — a narrow
// button there risks a mis-tap landing on "Erneut versuchen" instead. A
// single full-width button plus the existing, already-reliable 145px
// retry/cancel pair reuses proven touch targets instead of introducing new
// narrow ones.
static lv_obj_t *prog_overwrite_btn;

// weight_btn_cb() (below) needs these before their own definitions — the
// rest of the program-mode flow (data tables, widget construction, the other
// enter_prog_*() functions, and their button callbacks) is defined together,
// further down, alongside its own widgets.
struct ProgWizardState {
    TagFormat format = TagFormat::UNKNOWN;
    int base_material_idx = 0;
    int modifier_idx = 0;     // 0 = "-" (no modifier/finish)
    int color_idx = 0;
    int brand_idx = 0;        // only meaningful for formats with a real brand field, see prog_format_has_brand_field()
    uint16_t weight_grams = 0;
    char serial[24] = "";     // generated once, in enter_prog_confirm(); empty for formats with no serial field
    bool tag_a_done = false;  // which of the two tags still needs (re)writing — drives the retry button
};
static ProgWizardState g_prog;
static void enter_prog_confirm();

static void set_all_hidden() {
    // Restore full brightness on every state transition (every enter_*()
    // below calls this first) — mirrors esp32rental's own ui_set_brightness()
    // pattern, just centralized here instead of repeated at every call site,
    // since this is already the one function every transition already goes
    // through. Only IDLE ever dims (see ui_tick()), so this is a no-op
    // outside that one case, but unconditional is simpler and harmless (same
    // brightness value set twice costs nothing) than tracking "was it
    // actually dimmed."
    gfx.setBrightness(BRIGHTNESS_FULL);
    lv_obj_add_flag(waiting_label,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(info_line1,      LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(info_line2_row,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(info_line3,      LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(banner_label,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(spinner,         LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weight_btn_row,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(choice_btn_row,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prog_enter_btn,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prog_title,           LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prog_list,            LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prog_cancel_btn,      LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prog_confirm_btn_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prog_retry_btn_row,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prog_overwrite_btn,   LV_OBJ_FLAG_HIDDEN);
}

// Not every format's `color` is a hex value — OpenTag3D can carry a
// human-readable color NAME instead (see tag_opentag3d.cpp), and a failed
// read falls back to the literal string "unbekannt" — so this returns false
// (rather than a best-effort guess) for anything that isn't a clean
// "#RRGGBB", and the caller hides the swatch entirely in that case instead
// of showing a wrong or placeholder color.
static bool try_parse_hex_color(const char *s, lv_color_t &out) {
    if (s[0] != '#' || strlen(s) != 7) return false;
    unsigned r, g, b;
    if (sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) != 3) return false;
    out = lv_color_hex((r << 16) | (g << 8) | b);
    return true;
}

static void set_color_swatch(lv_color_t color) {
    lv_obj_set_style_bg_color(color_swatch, color, 0);
    lv_obj_set_style_bg_opa(color_swatch, LV_OPA_COVER, 0);
    lv_obj_clear_flag(color_swatch, LV_OBJ_FLAG_HIDDEN);
}

static void show_tag_info(const FilamentTagData &tag) {
    lv_label_set_text_fmt(info_line1, "%s  -  %s", tag.brand, tag.type_name);
    if (tag.weight_grams > 0) {
        lv_label_set_text_fmt(info_line2, "%s: %s   %s: %dg", STR_LABEL_COLOR, tag.color, STR_LABEL_WEIGHT, tag.weight_grams);
    } else {
        lv_label_set_text_fmt(info_line2, "%s: %s", STR_LABEL_COLOR, tag.color);
    }
    lv_color_t rgb;
    if (try_parse_hex_color(tag.color, rgb)) {
        set_color_swatch(rgb);
    } else {
        lv_obj_add_flag(color_swatch, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(info_line1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(info_line2_row, LV_OBJ_FLAG_HIDDEN);
}

// ── State transitions ────────────────────────────────────────────────────────

static void enter_idle() {
    g_state = AppState::IDLE;
    g_state_entered_at = millis();  // needed for the dim-after-idle check in ui_tick()
    set_all_hidden();
    lv_obj_clear_flag(waiting_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(prog_enter_btn, LV_OBJ_FLAG_HIDDEN);
    led_off();
}

// On a fast local network, api_scan()/api_status()/api_checkin()/
// api_checkout() can return well within a fraction of a second — fast
// enough that the PROCESSING banner+spinner enter_processing() shows right
// before the blocking call barely registers at all before the screen
// already moves on to the result. The operator then only notices the
// button's own brief LVGL press-highlight, with nothing in between that
// clearly says "this is being processed". g_processing_started_at (set by
// enter_processing()) plus wait_out_min_processing_time() (called by every
// run_*() function right after its own blocking API call returns, success
// or failure, before acting on the result) together guarantee the
// PROCESSING screen stays visible for at least this long regardless of how
// fast the backend actually answered — a genuinely slow request is
// unaffected, since the elapsed time already exceeds this floor by then.
static const unsigned long MIN_PROCESSING_VISIBLE_MS = 400;
static unsigned long g_processing_started_at = 0;

static void wait_out_min_processing_time() {
    unsigned long elapsed = millis() - g_processing_started_at;
    if (elapsed < MIN_PROCESSING_VISIBLE_MS) {
        delay(MIN_PROCESSING_VISIBLE_MS - elapsed);
    }
}

static void enter_processing(const char *message) {
    g_state = AppState::PROCESSING;
    g_processing_started_at = millis();
    set_all_hidden();
    // Restore the default positions — enter_prog_write() (program mode)
    // moves both of these to make room for its own 2-line prompt text next
    // to the spinner, and LVGL positions are sticky across calls, not
    // per-state, so this can't be skipped just because it "was already at
    // 112/140 the very first time."
    lv_obj_align(banner_label, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 140);
    lv_label_set_text(banner_label, message);
    lv_obj_set_style_text_font(banner_label, MY_FONT_20, 0);
    lv_obj_set_style_text_color(banner_label, lv_color_hex(CLR_TEXT_PRIMARY), 0);
    lv_obj_clear_flag(banner_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    led_blue();
    // Force a render pass so the spinner is actually visible before the
    // blocking HTTP call that immediately follows this in the caller.
    //
    // Calling lv_timer_handler() again here would be a complete no-op in the
    // context this always runs in: checkin_btn_cb()/weight_btn_cb()/etc.
    // fire from *inside* LVGL's own input-event processing, which itself
    // happens inside the OUTER lv_timer_handler() call in main.cpp's
    // loop() — and LVGL 8.x's lv_timer_handler() has a reentrancy guard (a
    // static `already_running` flag in lv_timer.c) that makes any NESTED
    // call while one is already in progress return immediately without
    // rendering or flushing anything. Without this, the widget state
    // (banner text, hidden flags) would be set correctly but never actually
    // reach the physical display before the blocking API call started — the
    // screen would only get its first real repaint once the OUTER
    // lv_timer_handler() call finally finished, by which point
    // enter_result() has already run, so the display jumps straight from
    // idle to the final result with the "processing" screen never visibly
    // shown at all. lv_refr_now(NULL) is LVGL's own dedicated API for
    // exactly this — forcing an immediate refresh from inside an event
    // handler — and is NOT routed through lv_timer_handler()'s reentrancy
    // guard, so it actually renders and flushes here, synchronously, even
    // when called from deep inside a button's click callback. NULL selects
    // the (only) registered display.
    lv_refr_now(NULL);
}

// Only sets up the banner/state/LED/timeout — deliberately does NOT touch
// info_line1/info_line2. Whether (and with what) those get shown alongside
// the result is the caller's call: some results have tag info to show
// (call show_tag_info() *after* this), an error or an unrecognized tag does
// not (leave them hidden, or set only info_line1 to a plain hint — see
// ui_on_tag_scanned()'s UNKNOWN branch). This used to unconditionally show
// both — which meant a caller that never populated info_line2 (the unknown-
// tag path) displayed LVGL's literal default label text, "Text", right
// alongside the intended message. Always call this before show_tag_info()/
// custom info-line setup, never after — set_all_hidden() below would
// immediately hide whatever the caller had just shown.
static void enter_result(bool success, const char *headline, uint32_t color) {
    g_state = AppState::RESULT;
    g_state_entered_at = millis();
    set_all_hidden();
    lv_obj_align(banner_label, LV_ALIGN_TOP_MID, 0, 112);  // see enter_processing()'s comment on why this is needed defensively
    lv_label_set_text(banner_label, headline);
    lv_obj_set_style_text_font(banner_label, MY_FONT_20, 0);
    lv_obj_set_style_text_color(banner_label, lv_color_hex(color), 0);
    lv_obj_clear_flag(banner_label, LV_OBJ_FLAG_HIDDEN);
    if (success) led_green(); else led_red();
}

static void enter_weight_pick(const FilamentTagData &tag) {
    g_pending_tag = tag;
    g_state = AppState::WEIGHT_PICK;
    set_all_hidden();
    // Restore default positions — enter_prog_weight() (program mode) moves
    // weight_btn_row up to make room for its own separate Abbrechen button,
    // and banner_label down for PROG_CONFIRM's longer summary; see those
    // functions' own comments on why this can't be skipped.
    lv_obj_align(banner_label, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_align(weight_btn_row, LV_ALIGN_BOTTOM_MID, 0, -8);
    show_tag_info(tag);
    // Smaller font + an explicit line break (rather than relying on
    // word-wrap) so this two-part prompt reliably fits within the label's
    // width without overflowing the screen.
    lv_label_set_text_fmt(banner_label, "%s\n%s", STR_PICK_WEIGHT, STR_PICK_WEIGHT_SUB);
    lv_obj_set_style_text_font(banner_label, MY_FONT_16, 0);
    lv_obj_set_style_text_color(banner_label, lv_color_hex(CLR_WARNING), 0);
    lv_obj_clear_flag(banner_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(weight_btn_row, LV_OBJ_FLAG_HIDDEN);
    led_orange();
}

// Shows/hides checkin_btn/checkout_btn individually within choice_btn_row —
// for a serial-less tag (enter_manual_choice()) either action is genuinely
// plausible (the backend can't tell a new roll from a returning one without
// a serial), so both stay offered; for a serialized tag
// (enter_confirm_choice()) the current in-stock status is already known, so
// only the one sensible action is shown — offering the other would invite
// exactly the kind of mistaken tap this whole confirm-step exists to
// prevent. When only one button is shown it's re-centered in the row
// (79px — (304-145)/2 — instead of the two-button 0/159px positions) so it
// doesn't look stranded to one side; both positions are reset here every
// time so this never depends on whatever the row last looked like.
static void set_choice_buttons(bool show_checkin, bool show_checkout) {
    if (show_checkin) {
        lv_obj_clear_flag(checkin_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(checkin_btn, show_checkout ? 0 : 79, 0);
    } else {
        lv_obj_add_flag(checkin_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (show_checkout) {
        lv_obj_clear_flag(checkout_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(checkout_btn, show_checkin ? 159 : 79, 0);
    } else {
        lv_obj_add_flag(checkout_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void enter_manual_choice(const FilamentTagData &tag, int current_stock) {
    g_pending_tag = tag;
    g_manual_current_stock = current_stock;
    g_state = AppState::MANUAL_CHOICE;
    g_state_entered_at = millis();
    set_all_hidden();
    show_tag_info(tag);
    lv_label_set_text_fmt(info_line3, "%s: %d", STR_LABEL_STOCK, current_stock);
    lv_obj_clear_flag(info_line3, LV_OBJ_FLAG_HIDDEN);
    // Ausbuchen only makes sense with at least one matching roll in stock —
    // with current_stock == 0 there is nothing to check out (the API would
    // just reject it, 404), so only Einbuchen is offered, same reasoning as
    // enter_confirm_choice()'s own single-button case above.
    set_choice_buttons(true, current_stock > 0);
    lv_obj_clear_flag(choice_btn_row, LV_OBJ_FLAG_HIDDEN);
    led_orange();
}

// Same MANUAL_CHOICE screen as enter_manual_choice() above, but for a tag
// that DOES carry a serial (Bambu, sometimes OpenTag3D) — used instead of
// letting run_scan() auto-decide and act via /scan, so the operator always
// confirms explicitly before anything is booked. A tag held up twice in a
// row (e.g. checking the label on the far side of the spool) would
// otherwise silently check the same roll out again immediately, with no
// chance to notice. Shows whether this exact serial is currently known to
// be in stock (Ja/Nein) rather than a spec-matching count, since a serial
// identifies one specific physical roll, not just a matching spec — and,
// since that status is already known, only the one matching action
// (Ausbuchen if in stock, Einbuchen if not) is offered at all, see
// set_choice_buttons() above.
static void enter_confirm_choice(const FilamentTagData &tag, bool in_stock) {
    g_pending_tag = tag;
    g_state = AppState::MANUAL_CHOICE;
    g_state_entered_at = millis();
    set_all_hidden();
    show_tag_info(tag);
    lv_label_set_text_fmt(info_line3, "%s: %s", STR_LABEL_STOCK, in_stock ? STR_YES : STR_NO);
    lv_obj_clear_flag(info_line3, LV_OBJ_FLAG_HIDDEN);
    set_choice_buttons(/*show_checkin=*/!in_stock, /*show_checkout=*/in_stock);
    lv_obj_clear_flag(choice_btn_row, LV_OBJ_FLAG_HIDDEN);
    led_orange();
}

// ── API call helpers (blocking — see enter_processing()'s comment) ───────────

static void run_scan(const FilamentTagData &tag) {
    enter_processing(STR_READING);
    ApiScanResult res;
    bool ok = api_scan(tag, res);
    wait_out_min_processing_time();
    if (!ok) {
        enter_result(false, res.error[0] ? res.error : STR_NO_CONNECTION, CLR_ERROR);
        return;
    }
    if (res.identify_required) {
        enter_manual_choice(tag, res.current_stock);
        return;
    }
    FilamentTagData shown = tag;
    strlcpy(shown.brand, res.brand_name, sizeof(shown.brand));
    strlcpy(shown.type_name, res.type_name, sizeof(shown.type_name));
    strlcpy(shown.color, res.color, sizeof(shown.color));
    shown.weight_grams = res.weight_grams;
    bool checked_in = strcmp(res.action, "checked_in") == 0;
    enter_result(true, checked_in ? STR_CHECKED_IN : STR_CHECKED_OUT, checked_in ? CLR_SUCCESS : CLR_INFO);
    show_tag_info(shown);  // must come after enter_result() — see its comment
}

// For a tag WITH a serial: read-only status check, then always show the
// explicit Einbuchen/Ausbuchen confirmation (enter_confirm_choice()) rather
// than letting a deterministic /scan call act immediately — see that
// function's own comment for why.
static void run_identify(const FilamentTagData &tag) {
    enter_processing(STR_READING);
    ApiStatusResult res;
    bool ok = api_status(tag, res);
    wait_out_min_processing_time();
    if (!ok) {
        enter_result(false, res.error[0] ? res.error : STR_NO_CONNECTION, CLR_ERROR);
        return;
    }
    enter_confirm_choice(tag, res.in_stock);
}

static void run_checkin_or_checkout(bool checkin) {
    FilamentTagData tag = g_pending_tag;
    // The UI has nothing to show but this banner+spinner before it blocks on
    // the API call (see enter_processing()'s own comment on forcing a
    // render pass first) — STR_BOOKING_CHECKIN/OUT ("Wird
    // ein-/ausgebucht...") read unambiguously as "in progress", unlike
    // STR_BTN_CHECKIN/OUT (the button's own static label) would.
    enter_processing(checkin ? STR_BOOKING_CHECKIN : STR_BOOKING_CHECKOUT);
    ApiActionResult res;
    bool ok = checkin ? api_checkin(tag, tag.weight_grams, res) : api_checkout(tag, tag.weight_grams, res);
    wait_out_min_processing_time();
    if (!ok) {
        enter_result(false, res.error[0] ? res.error : STR_NO_CONNECTION, CLR_ERROR);
        return;
    }
    FilamentTagData shown = tag;
    strlcpy(shown.brand, res.brand_name, sizeof(shown.brand));
    strlcpy(shown.type_name, res.type_name, sizeof(shown.type_name));
    strlcpy(shown.color, res.color, sizeof(shown.color));
    shown.weight_grams = res.weight_grams;
    enter_result(true, checkin ? STR_CHECKED_IN : STR_CHECKED_OUT, checkin ? CLR_SUCCESS : CLR_INFO);
    show_tag_info(shown);  // must come after enter_result() — see its comment
}

// ── Button callbacks ─────────────────────────────────────────────────────────

static void weight_btn_cb(lv_event_t *e) {
    uint16_t weight = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    if (g_state == AppState::PROG_WEIGHT) {
        g_prog.weight_grams = weight;
        enter_prog_confirm();
        return;
    }
    FilamentTagData tag = g_pending_tag;
    tag.weight_grams = weight;
    if (tag.has_serial) {
        run_identify(tag);
    } else {
        run_scan(tag);
    }
}

static void checkin_btn_cb(lv_event_t *) { run_checkin_or_checkout(true); }
static void checkout_btn_cb(lv_event_t *) { run_checkin_or_checkout(false); }

// ── Test-tag wizard ("Programmieren", program mode) ─────────────────────────
//
// Writes a freshly generated test tag to two physical NFC tags (a spool's two
// faces get the same identifier) in one of the four supported formats — pure
// NFC provisioning, this never calls the MakerSpaceAPI backend. See
// README.md's "Testtags programmieren" section for the full walkthrough and
// the reasoning behind the design choices below (in particular why Bambu
// tags get their sector trailers rewritten with the derived key, and why
// brand/serial aren't asked for every format).
//
// All actual PN532 I/O for the write happens inside nfc_reader.cpp's own
// task (see nfc_arm_write()'s doc comment in nfc_reader.h) — this file only
// arms/disarms a request and polls for the result in ui_tick().

static const TagFormat PROG_FORMATS[] = {
    TagFormat::BAMBU, TagFormat::CREALITY, TagFormat::OPENSPOOL, TagFormat::OPENTAG3D,
};
static const char *PROG_FORMAT_LABELS[] = {
    "Bambu Lab", "Creality", "OpenSpool", "OpenTag3D",
};

// A fixed list rather than free-text entry (no keyboard on this board);
// rendered as a scrollable row list, one row per option, whenever a step has
// more than fit on screen at once (see prog_refresh_list()).
static const char *PROG_BASE_MATERIALS[] = {
    "PLA", "PETG", "ABS", "TPU", "ASA", "PC", "PA", "HIPS", "PVA", "Holz",
};
static const int PROG_BASE_MATERIAL_COUNT = sizeof(PROG_BASE_MATERIALS) / sizeof(PROG_BASE_MATERIALS[0]);

// Index 0 ("-") means "no modifier" — build_prog_write_tag() then writes
// just the base material alone. Real filament lines commonly combine a base
// material with a finish/effect like this (e.g. "PLA Luminous", "PLA
// Stardust" — both real eSun/Creality product names) — modeled as a second,
// independent pick rather than folding dozens of combinations into one long
// list. OpenTag3D genuinely stores these as two separate fields already
// (Base Material + Modifiers, see tag_opentag3d.h) — the other three
// formats only have one text field, so tag_write_bambu()/
// tag_write_creality()/tag_write_openspool() just get the combined string
// (see build_prog_write_tag()). A name longer than OpenTag3D's own 5-byte
// Modifiers field (e.g. "Luminous") gets truncated when written to THAT
// format specifically — a real constraint of its fixed layout, not a bug
// (see tag_write_opentag3d()'s own comment and nfc_reader.cpp's write-
// verification, which accounts for it).
static const char *PROG_MODIFIERS[] = {
    "-", "Silk", "Luminous", "Stardust", "Glitter", "Matte", "Marmor", "CF", "GF", "Transparent", "Hyper",
};
static const int PROG_MODIFIER_COUNT = sizeof(PROG_MODIFIERS) / sizeof(PROG_MODIFIERS[0]);

// Asked for OpenSpool/OpenTag3D, both of which have a plain writable brand
// text field — free-text-shaped, but this board has no keyboard, so a fixed
// list stands in for one, same reasoning as every other wizard step.
static const char *PROG_BRANDS[] = {
    "Generic", "Creality", "Bambu Lab", "eSun", "Overture", "Sunlu", "Prusament", "Polymaker",
};
static const int PROG_BRAND_COUNT = sizeof(PROG_BRANDS) / sizeof(PROG_BRANDS[0]);

// Also asked for Creality — but from a DIFFERENT, narrower list than
// PROG_BRANDS above: a Creality K2 tag's brand isn't free text at all, it's
// resolved by looking up the written material_id in creality_k2_materials.h
// (see tag_creality.cpp's find_k2_material_for_wizard()), whose real
// material_database.json snapshot only ever contains these four brands —
// offering "Bambu Lab" or "Prusament" here would just fall back to whichever
// brand happens to sort first in the table, silently ignoring the pick.
static const char *PROG_CREALITY_BRANDS[] = {"Generic", "Creality", "eSUN", "Polymaker"};
static const int PROG_CREALITY_BRAND_COUNT = sizeof(PROG_CREALITY_BRANDS) / sizeof(PROG_CREALITY_BRANDS[0]);

static const char **prog_active_brands() {
    return g_prog.format == TagFormat::CREALITY ? PROG_CREALITY_BRANDS : PROG_BRANDS;
}
static int prog_active_brand_count() {
    return g_prog.format == TagFormat::CREALITY ? PROG_CREALITY_BRAND_COUNT : PROG_BRAND_COUNT;
}

struct ProgColor {
    const char *name;
    const char *hex;      // "#RRGGBB", uppercase — must match exactly what every tag_parse_*() formats back
    uint32_t    rgb;
    bool        light_bg; // true = use black label text (this swatch is light)
};
static const ProgColor PROG_COLORS[] = {
    {"Schwarz", "#000000", 0x000000, false},
    {"Weiß",    "#FFFFFF", 0xFFFFFF, true},
    {"Rot",     "#FF0000", 0xFF0000, false},
    {"Grün",    "#00A651", 0x00A651, false},
    {"Blau",    "#0066CC", 0x0066CC, false},
    {"Gelb",    "#FFD400", 0xFFD400, true},
    {"Orange",  "#FF7A00", 0xFF7A00, false},
    {"Grau",    "#808080", 0x808080, false},
    {"Pink",    "#FF66B2", 0xFF66B2, false},
    {"Lila",    "#8E44AD", 0x8E44AD, false},
    {"Braun",   "#6B3F1D", 0x6B3F1D, false},
    {"Türkis",  "#00B8B8", 0x00B8B8, false},
    {"Silber",  "#C0C0C0", 0xC0C0C0, true},
    {"Gold",    "#D4AF37", 0xD4AF37, true},
    {"Natur",   "#F0E6D2", 0xF0E6D2, true},
    {"Beige",   "#E8D6B3", 0xE8D6B3, true},
};
static const int PROG_COLOR_COUNT = sizeof(PROG_COLORS) / sizeof(PROG_COLORS[0]);

static const int PROG_LIST_ROW_H = 40;
static const int PROG_LIST_GAP   = 6;

// Bambu, OpenTag3D, AND Creality (the K2/CFS layout the wizard writes, see
// tag_write_creality()) all have a real weight/length field on the tag —
// only OpenSpool has none by design. Only Bambu/OpenTag3D have a genuine
// per-spool serial field the backend trusts, though (see CLAUDE.md #69/#70)
// — Creality's own K2 "Spool ID"-equivalent field is never trusted as a
// serial even when present, same reasoning as the old layout, so it's still
// excluded from prog_format_has_serial() below.
static bool prog_format_needs_weight() {
    return g_prog.format == TagFormat::BAMBU || g_prog.format == TagFormat::OPENTAG3D ||
           g_prog.format == TagFormat::CREALITY;
}
static bool prog_format_has_serial() {
    return g_prog.format == TagFormat::BAMBU || g_prog.format == TagFormat::OPENTAG3D;
}
// OpenSpool (a plain JSON "brand" field), OpenTag3D (a dedicated
// Manufacturer field), and Creality (via its K2 layout's material_id, looked
// up in creality_k2_materials.h — see tag_write_creality()) all have
// somewhere a chosen brand actually lands. Bambu Lab tags never carry a
// brand field at all (tag_parse_bambu() always reports "Bambu Lab"
// regardless of tag content) — asking for one there would write it nowhere
// and silently do nothing, so it isn't asked. Creality's OLD layout has the
// identical problem (see CrealityOld's own comment in tag_creality.cpp),
// which is exactly why the wizard writes the K2 layout instead.
static bool prog_format_has_brand_field() {
    return g_prog.format == TagFormat::OPENSPOOL || g_prog.format == TagFormat::OPENTAG3D ||
           g_prog.format == TagFormat::CREALITY;
}

static bool is_prog_wizard_state(AppState s) {
    return s == AppState::PROG_FORMAT || s == AppState::PROG_BASE_MATERIAL || s == AppState::PROG_MODIFIER ||
           s == AppState::PROG_COLOR || s == AppState::PROG_BRAND || s == AppState::PROG_WEIGHT ||
           s == AppState::PROG_CONFIRM || s == AppState::PROG_WRITE_A || s == AppState::PROG_WRITE_B ||
           s == AppState::PROG_RESULT;  // PROG_DONE has its own short timeout, see ui_tick()
}

static void prog_list_btn_cb(lv_event_t *e);

// Appends one full-width row button to prog_list, carrying `idx` as its
// click callback's user_data so prog_list_btn_cb() knows which option was
// tapped without needing to search the list for it.
static void prog_list_add_row(const char *text, uint32_t bg_rgb, uint32_t text_rgb, int idx) {
    lv_obj_t *btn = lv_btn_create(prog_list);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, PROG_LIST_ROW_H);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_rgb), 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(text_rgb), 0);
    lv_obj_set_style_text_font(label, MY_FONT_16, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, prog_list_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);
}

// Rebuilds prog_list from scratch for whichever step is currently active —
// called every time a step is (re)entered. lv_obj_clean() discards the
// previous step's row widgets (and their event callbacks) before the new
// ones are created, so there's nothing left over to defensively reset here,
// unlike the old fixed-8-button grid (which needed exactly this kind of
// defensive re-populate to avoid showing a previous step's leftover content
// — see git history for that bug and its one-line fix). No paging: every
// option gets its own row, and the container scrolls if they don't all fit.
static void prog_refresh_list() {
    lv_obj_clean(prog_list);
    switch (g_state) {
        case AppState::PROG_FORMAT:
            for (int i = 0; i < 4; i++) prog_list_add_row(PROG_FORMAT_LABELS[i], CLR_ACCENT, CLR_TEXT_PRIMARY, i);
            break;
        case AppState::PROG_BASE_MATERIAL:
            for (int i = 0; i < PROG_BASE_MATERIAL_COUNT; i++) prog_list_add_row(PROG_BASE_MATERIALS[i], CLR_ACCENT, CLR_TEXT_PRIMARY, i);
            break;
        case AppState::PROG_MODIFIER:
            for (int i = 0; i < PROG_MODIFIER_COUNT; i++) prog_list_add_row(PROG_MODIFIERS[i], CLR_ACCENT, CLR_TEXT_PRIMARY, i);
            break;
        case AppState::PROG_BRAND: {
            const char **brands = prog_active_brands();
            int count = prog_active_brand_count();
            for (int i = 0; i < count; i++) prog_list_add_row(brands[i], CLR_ACCENT, CLR_TEXT_PRIMARY, i);
            break;
        }
        case AppState::PROG_COLOR:
            for (int i = 0; i < PROG_COLOR_COUNT; i++) {
                prog_list_add_row(PROG_COLORS[i].name, PROG_COLORS[i].rgb,
                                   PROG_COLORS[i].light_bg ? 0x000000 : 0xFFFFFF, i);
            }
            break;
        default: break;
    }
}

// Shared by every list step (FORMAT only has 4 items and never needs to
// scroll, but routes through here too for one consistent construction path).
static void enter_prog_list_step(AppState state, const char *title) {
    g_state = state;
    g_state_entered_at = millis();
    set_all_hidden();
    lv_label_set_text(prog_title, title);
    lv_obj_clear_flag(prog_title, LV_OBJ_FLAG_HIDDEN);
    prog_refresh_list();
    lv_obj_scroll_to_y(prog_list, 0, LV_ANIM_OFF);  // start scrolled to the top on every fresh entry into a step
    lv_obj_clear_flag(prog_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(prog_cancel_btn, LV_OBJ_FLAG_HIDDEN);
}

static void enter_prog_format() {
    g_prog = ProgWizardState();  // fresh wizard run — discard any previous picks
    enter_prog_list_step(AppState::PROG_FORMAT, STR_PROG_PICK_FORMAT);
}
static void enter_prog_base_material() { enter_prog_list_step(AppState::PROG_BASE_MATERIAL, STR_PROG_PICK_BASE_MATERIAL); }
static void enter_prog_modifier()      { enter_prog_list_step(AppState::PROG_MODIFIER, STR_PROG_PICK_MODIFIER); }
static void enter_prog_color()         { enter_prog_list_step(AppState::PROG_COLOR, STR_PROG_PICK_COLOR); }
static void enter_prog_brand()         { enter_prog_list_step(AppState::PROG_BRAND, STR_PROG_PICK_BRAND); }

static void enter_prog_weight() {
    g_state = AppState::PROG_WEIGHT;
    g_state_entered_at = millis();
    set_all_hidden();
    lv_label_set_text(prog_title, STR_PROG_PICK_WEIGHT);
    lv_obj_clear_flag(prog_title, LV_OBJ_FLAG_HIDDEN);
    // Moved up from the default bottom position — prog_cancel_btn (this
    // step's own Abbrechen) sits there too, and the two would otherwise
    // exactly overlap: the real bug this fixes had the weight preset
    // buttons completely hidden underneath Abbrechen, which was added to
    // the screen after them and therefore drawn on top. Restored to the
    // default in enter_weight_pick() (the normal, non-wizard weight picker,
    // which has no Abbrechen button at all and needs the bottom position back).
    lv_obj_align(weight_btn_row, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_clear_flag(weight_btn_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(prog_cancel_btn, LV_OBJ_FLAG_HIDDEN);
}

// Builds the FilamentTagData describing what should be written to whichever
// physical tag is presented next — identical for tag A and tag B (both faces
// of the same spool get the same identifier).
static void build_prog_write_tag(FilamentTagData &out) {
    out = FilamentTagData();
    out.format = g_prog.format;
    const char *modifier = PROG_MODIFIERS[g_prog.modifier_idx];
    if (modifier[0] == '-' && modifier[1] == '\0') {
        strlcpy(out.type_name, PROG_BASE_MATERIALS[g_prog.base_material_idx], sizeof(out.type_name));
    } else {
        snprintf(out.type_name, sizeof(out.type_name), "%s %s", PROG_BASE_MATERIALS[g_prog.base_material_idx], modifier);
    }
    strlcpy(out.color, PROG_COLORS[g_prog.color_idx].hex, sizeof(out.color));
    if (prog_format_has_brand_field()) strlcpy(out.brand, prog_active_brands()[g_prog.brand_idx], sizeof(out.brand));
    out.weight_grams = prog_format_needs_weight() ? g_prog.weight_grams : 0;
    out.has_serial = prog_format_has_serial();
    if (out.has_serial) strlcpy(out.vendor_serial, g_prog.serial, sizeof(out.vendor_serial));
}

// Builds the extra fact line shown on the confirm/done screens — brand
// and/or serial, whichever this format actually has, or a plain "no serial
// field" note when it has neither (Creality). Bambu (serial only), OpenSpool
// (brand only) and OpenTag3D (both) each get exactly what applies.
static void build_prog_extra_line(char *out, size_t cap) {
    bool has_brand = prog_format_has_brand_field();
    bool has_serial = prog_format_has_serial();
    if (has_brand && has_serial) {
        snprintf(out, cap, "%s: %s | %s: %s", STR_LABEL_BRAND, prog_active_brands()[g_prog.brand_idx], STR_LABEL_SERIAL, g_prog.serial);
    } else if (has_brand) {
        snprintf(out, cap, "%s: %s", STR_LABEL_BRAND, prog_active_brands()[g_prog.brand_idx]);
    } else if (has_serial) {
        snprintf(out, cap, "%s: %s", STR_LABEL_SERIAL, g_prog.serial);
    } else {
        strlcpy(out, STR_PROG_NO_SERIAL_NOTE, cap);
    }
}

static void enter_prog_confirm() {
    g_state = AppState::PROG_CONFIRM;
    g_state_entered_at = millis();
    set_all_hidden();

    if (prog_format_has_serial()) {
        // Bambu's Tray UID field is read/written as a 16-byte ASCII string
        // (see tag_bambu.cpp); OpenTag3D's Serial/Batch ID is 32 bytes — 12
        // and 16 hex characters respectively leave comfortable NUL-padding
        // room in both.
        gen_hex_serial(g_prog.serial, sizeof(g_prog.serial), g_prog.format == TagFormat::BAMBU ? 12 : 16);
    } else {
        g_prog.serial[0] = '\0';
    }

    FilamentTagData preview;
    build_prog_write_tag(preview);

    char line1[64], line2[64], line3[64];
    snprintf(line1, sizeof(line1), "%s - %s", tag_format_name(g_prog.format), preview.type_name);
    snprintf(line2, sizeof(line2), "%s: %s", STR_LABEL_COLOR, PROG_COLORS[g_prog.color_idx].name);
    if (prog_format_needs_weight()) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "   %s: %ug", STR_LABEL_WEIGHT, (unsigned)g_prog.weight_grams);
        strlcat(line2, tmp, sizeof(line2));
    }
    build_prog_extra_line(line3, sizeof(line3));

    lv_label_set_text(info_line1, line1);
    lv_label_set_text(info_line2, line2);
    lv_label_set_text(info_line3, line3);
    // The exact swatch color is already known here (picked from PROG_COLORS
    // a moment ago, see enter_prog_color()) — no parsing needed, unlike
    // show_tag_info()'s own use of this same swatch for a real scanned tag.
    set_color_swatch(lv_color_hex(PROG_COLORS[g_prog.color_idx].rgb));
    lv_obj_clear_flag(info_line1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(info_line2_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(info_line3, LV_OBJ_FLAG_HIDDEN);

    // Moved down from the default y=112 — with a brand+serial line3 (e.g.
    // OpenTag3D) this can wrap to two lines, which used to run straight into
    // this banner at the default position (the reported overlap bug); this
    // leaves comfortable clearance regardless of exactly how long line3
    // ends up being. Restored to the default in enter_processing()/
    // enter_result()/enter_weight_pick()/enter_prog_write()/
    // enter_prog_result_error()/enter_prog_done().
    lv_obj_align(banner_label, LV_ALIGN_TOP_MID, 0, 142);
    lv_label_set_text(banner_label, STR_PROG_CONFIRM_TITLE);
    lv_obj_set_style_text_font(banner_label, MY_FONT_16, 0);
    lv_obj_set_style_text_color(banner_label, lv_color_hex(CLR_TEXT_PRIMARY), 0);
    lv_obj_clear_flag(banner_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(prog_confirm_btn_row, LV_OBJ_FLAG_HIDDEN);
}

static void enter_prog_write(bool tag_a, bool allow_overwrite = false) {
    g_state = tag_a ? AppState::PROG_WRITE_A : AppState::PROG_WRITE_B;
    g_state_entered_at = millis();
    set_all_hidden();

    FilamentTagData desired;
    build_prog_write_tag(desired);
    nfc_arm_write(desired, allow_overwrite);

    // STR_PROG_WRITE_A/B are two explicit lines ("Ersten Tag auflegen" /
    // "(Seite A der Spule)") at MY_FONT_16 — a ~40px-tall label, which
    // collides with the spinner's own default position (also untouched
    // since PROCESSING's own y=140 default, since nothing in the PROG_*
    // flow ever calls enter_processing()) at the default banner y=112 — the
    // originally-reported overlap. The first fix (banner→96, spinner→150)
    // still wasn't enough clearance on real hardware — the spinner's actual
    // rendered footprint runs lower than its nominal 40px box, right into
    // prog_cancel_btn's own top edge at 188 — so both are now shifted up
    // further still, with real margin on every side: banner→60 (spans
    // ~60-100), spinner→120 (spans 120-160, a full 28px clear of the
    // cancel button). Both restored to their 112/140 defaults by
    // enter_processing() the next time a normal scan runs it.
    lv_obj_align(banner_label, LV_ALIGN_TOP_MID, 0, 60);
    lv_label_set_text(banner_label, tag_a ? STR_PROG_WRITE_A : STR_PROG_WRITE_B);
    lv_obj_set_style_text_font(banner_label, MY_FONT_16, 0);
    lv_obj_set_style_text_color(banner_label, lv_color_hex(CLR_TEXT_PRIMARY), 0);
    lv_obj_clear_flag(banner_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(prog_cancel_btn, LV_OBJ_FLAG_HIDDEN);
    led_orange();
}

static void enter_prog_result_error(const char *headline) {
    g_state = AppState::PROG_RESULT;
    g_state_entered_at = millis();
    set_all_hidden();
    lv_obj_align(banner_label, LV_ALIGN_TOP_MID, 0, 112);  // restore default — see enter_prog_confirm()'s comment
    lv_label_set_text(banner_label, headline);
    lv_obj_set_style_text_font(banner_label, MY_FONT_14, 0);  // smaller than usual — these error messages can be long
    lv_obj_set_style_text_color(banner_label, lv_color_hex(CLR_ERROR), 0);
    lv_obj_clear_flag(banner_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(prog_retry_btn_row, LV_OBJ_FLAG_HIDDEN);
    led_red();
}

// Same PROG_RESULT screen as enter_prog_result_error() above, but for the
// specific "the tag already carries known data" refusal — additionally
// shows prog_overwrite_btn (a lone full-width button) right above the
// existing prog_retry_btn_row (Erneut versuchen / Abbrechen, unchanged and
// reused as-is), since a plain retry against the SAME physical tag would
// just hit the identical refusal again without the operator's explicit
// override. Orange/warning rather than red/error — this isn't really a
// failure, it's a safety check the operator can now knowingly override, see
// STR_BTN_OVERWRITE's own comment. banner_label sits MUCH higher than
// enter_prog_result_error()'s default 112 to leave clearance above the extra
// button (prog_overwrite_btn's own top edge is at y=140, see its comment) —
// the title line plus a wrapped hint line can reach ~140+, so this uses the
// same generous y=60 this file already established for enter_prog_write()'s
// own 2-line prompt sitting well clear of the spinner 60px further down.
static void enter_prog_already_written(const char *headline) {
    g_state = AppState::PROG_RESULT;
    g_state_entered_at = millis();
    set_all_hidden();
    lv_obj_align(banner_label, LV_ALIGN_TOP_MID, 0, 60);
    lv_label_set_text(banner_label, headline);
    lv_obj_set_style_text_font(banner_label, MY_FONT_14, 0);
    lv_obj_set_style_text_color(banner_label, lv_color_hex(CLR_WARNING), 0);
    lv_obj_clear_flag(banner_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(prog_overwrite_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(prog_retry_btn_row, LV_OBJ_FLAG_HIDDEN);
    led_orange();
}

static void enter_prog_done() {
    g_state = AppState::PROG_DONE;
    g_state_entered_at = millis();
    set_all_hidden();
    lv_obj_align(banner_label, LV_ALIGN_TOP_MID, 0, 112);  // restore default — see enter_prog_confirm()'s comment
    lv_label_set_text(banner_label, STR_PROG_DONE);
    lv_obj_set_style_text_font(banner_label, MY_FONT_20, 0);
    lv_obj_set_style_text_color(banner_label, lv_color_hex(CLR_SUCCESS), 0);
    lv_obj_clear_flag(banner_label, LV_OBJ_FLAG_HIDDEN);
    if (prog_format_has_serial() || prog_format_has_brand_field()) {
        char line[64];
        build_prog_extra_line(line, sizeof(line));
        lv_label_set_text(info_line1, line);
        lv_obj_clear_flag(info_line1, LV_OBJ_FLAG_HIDDEN);
    }
    led_green();
}

static void prog_enter_btn_cb(lv_event_t *) { enter_prog_format(); }

static void prog_cancel_btn_cb(lv_event_t *) {
    nfc_disarm_write();
    enter_idle();
}

// What comes after the color/brand pick, shared since brand is skipped
// entirely for formats without a writable brand field.
static void prog_after_color_or_brand() {
    if (prog_format_needs_weight()) enter_prog_weight();
    else enter_prog_confirm();
}

static void prog_list_btn_cb(lv_event_t *e) {
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    switch (g_state) {
        case AppState::PROG_FORMAT:
            if (idx >= 4) return;
            g_prog.format = PROG_FORMATS[idx];
            enter_prog_base_material();
            break;
        case AppState::PROG_BASE_MATERIAL:
            if (idx >= PROG_BASE_MATERIAL_COUNT) return;
            g_prog.base_material_idx = idx;
            enter_prog_modifier();
            break;
        case AppState::PROG_MODIFIER:
            if (idx >= PROG_MODIFIER_COUNT) return;
            g_prog.modifier_idx = idx;
            enter_prog_color();
            break;
        case AppState::PROG_COLOR:
            if (idx >= PROG_COLOR_COUNT) return;
            g_prog.color_idx = idx;
            if (prog_format_has_brand_field()) enter_prog_brand();
            else prog_after_color_or_brand();
            break;
        case AppState::PROG_BRAND:
            if (idx >= prog_active_brand_count()) return;
            g_prog.brand_idx = idx;
            prog_after_color_or_brand();
            break;
        default: break;
    }
}

static void prog_confirm_write_cb(lv_event_t *) { enter_prog_write(/*tag_a=*/true); }

static void prog_retry_btn_cb(lv_event_t *) {
    // g_prog.tag_a_done says which tag still needs (re)writing — set in
    // ui_tick() right before showing this failure screen. Plain retry —
    // for when the operator has swapped in a different (blank) tag.
    enter_prog_write(!g_prog.tag_a_done);
}

// Explicit "Überschreiben" — same target slot as prog_retry_btn_cb() above,
// but with allow_overwrite forced true, so the SAME physical tag currently
// on the reader (still carrying whatever it was already reporting) gets
// deliberately overwritten instead of refused again.
static void prog_overwrite_btn_cb(lv_event_t *) {
    // Pairs with nfc_reader.cpp's own "(Ueberschreiben erzwungen)" suffix on
    // the write path's UID-detected log line — if THIS line prints but that
    // suffix is missing on the next tag placement, the bug is in the
    // hand-off between here and handle_write_request() (the armed-write
    // queue); if THIS line doesn't print at all, the tap isn't reaching this
    // callback in the first place (an LVGL event-routing/touch issue).
    Serial.println("  UI: Ueberschreiben-Button angetippt, arme Schreibvorgang mit allow_overwrite=true");
    enter_prog_write(!g_prog.tag_a_done, /*allow_overwrite=*/true);
}

// ── Widget construction helpers ──────────────────────────────────────────────

static lv_obj_t *make_weight_btn(lv_obj_t *parent, int x, uint16_t grams) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 70, 44);
    lv_obj_set_pos(btn, x, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(CLR_ACCENT), 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text_fmt(label, "%dg", grams);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, weight_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)grams);
    return btn;
}

// ── Public API ────────────────────────────────────────────────────────────────

void ui_init(lv_disp_t *disp) {
    scr = lv_disp_get_scr_act(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG), 0);

    // Top bar
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, STR_APP_TITLE);
    lv_obj_set_style_text_color(title, lv_color_hex(CLR_TEXT_SECONDARY), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 4);

    wifi_dot = lv_obj_create(scr);
    lv_obj_set_size(wifi_dot, 12, 12);
    lv_obj_set_style_radius(wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wifi_dot, lv_color_hex(CLR_ERROR), 0);
    lv_obj_set_style_border_width(wifi_dot, 0, 0);
    lv_obj_align(wifi_dot, LV_ALIGN_TOP_RIGHT, -8, 6);

    // Content
    // Every label below gets a fixed width (matching the button rows) plus
    // center-aligned, word-wrapped text — not just for the specific
    // "Gewicht nicht auf Tag..." prompt that first exposed the lack of this,
    // but defensively for any long text at this label's font size, including
    // a server error message (ApiScanResult.error can be up to 127 chars).
    static const lv_coord_t CONTENT_WIDTH = 304;

    waiting_label = lv_label_create(scr);
    lv_label_set_text(waiting_label, STR_WAITING);
    lv_obj_set_style_text_font(waiting_label, MY_FONT_20, 0);
    lv_obj_set_style_text_color(waiting_label, lv_color_hex(CLR_TEXT_PRIMARY), 0);
    lv_obj_set_width(waiting_label, CONTENT_WIDTH);
    lv_label_set_long_mode(waiting_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(waiting_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(waiting_label, LV_ALIGN_CENTER, 0, -10);

    // Explicit empty initial text on every label below except waiting_label
    // (which gets real text immediately anyway) — lv_label_create()'s own
    // default is the literal string "Text", which would otherwise flash
    // briefly or, worse, actually get shown if some future code path shows
    // a label before ever setting its content (exactly the bug that showed
    // "Text" overlapping the unknown-tag hint, fixed above in enter_result()).
    info_line1 = lv_label_create(scr);
    lv_label_set_text(info_line1, "");
    lv_obj_set_style_text_font(info_line1, MY_FONT_16, 0);
    lv_obj_set_style_text_color(info_line1, lv_color_hex(CLR_TEXT_PRIMARY), 0);
    lv_obj_set_width(info_line1, CONTENT_WIDTH);
    lv_label_set_long_mode(info_line1, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(info_line1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info_line1, LV_ALIGN_TOP_MID, 0, 40);

    // A flex row (not a plain label) so color_swatch can sit directly next
    // to the color text and stay centered as a group regardless of how long
    // that text ends up being — see show_tag_info()'s own comment.
    info_line2_row = lv_obj_create(scr);
    lv_obj_set_size(info_line2_row, CONTENT_WIDTH, 22);
    lv_obj_align(info_line2_row, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_opa(info_line2_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_line2_row, 0, 0);
    lv_obj_set_style_pad_all(info_line2_row, 0, 0);
    lv_obj_set_style_pad_column(info_line2_row, 6, 0);
    lv_obj_clear_flag(info_line2_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(info_line2_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_line2_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    color_swatch = lv_obj_create(info_line2_row);
    lv_obj_set_size(color_swatch, 14, 14);
    lv_obj_set_style_radius(color_swatch, 2, 0);
    lv_obj_set_style_border_width(color_swatch, 1, 0);
    lv_obj_set_style_border_color(color_swatch, lv_color_hex(CLR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(color_swatch, 0, 0);
    lv_obj_clear_flag(color_swatch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(color_swatch, LV_OBJ_FLAG_HIDDEN);  // shown only when the color is a parseable "#RRGGBB" — see show_tag_info()

    info_line2 = lv_label_create(info_line2_row);
    lv_label_set_text(info_line2, "");
    lv_obj_set_style_text_font(info_line2, MY_FONT_14, 0);
    lv_obj_set_style_text_color(info_line2, lv_color_hex(CLR_TEXT_SECONDARY), 0);

    info_line3 = lv_label_create(scr);
    lv_label_set_text(info_line3, "");
    lv_obj_set_style_text_font(info_line3, MY_FONT_14, 0);
    lv_obj_set_style_text_color(info_line3, lv_color_hex(CLR_TEXT_SECONDARY), 0);
    lv_obj_set_width(info_line3, CONTENT_WIDTH);
    lv_label_set_long_mode(info_line3, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(info_line3, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info_line3, LV_ALIGN_TOP_MID, 0, 86);

    banner_label = lv_label_create(scr);
    lv_label_set_text(banner_label, "");
    lv_obj_set_style_text_font(banner_label, MY_FONT_20, 0);
    lv_obj_set_width(banner_label, CONTENT_WIDTH);
    lv_label_set_long_mode(banner_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(banner_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(banner_label, LV_ALIGN_TOP_MID, 0, 112);

    spinner = lv_spinner_create(scr, 1000, 60);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 140);

    // Weight picker row
    weight_btn_row = lv_obj_create(scr);
    lv_obj_set_size(weight_btn_row, 304, 44);
    lv_obj_align(weight_btn_row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(weight_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weight_btn_row, 0, 0);
    lv_obj_set_style_pad_all(weight_btn_row, 0, 0);
    lv_obj_clear_flag(weight_btn_row, LV_OBJ_FLAG_SCROLLABLE);
    make_weight_btn(weight_btn_row, 0,   250);
    make_weight_btn(weight_btn_row, 78,  500);
    make_weight_btn(weight_btn_row, 156, 750);
    make_weight_btn(weight_btn_row, 234, DEFAULT_WEIGHT_GRAMS);

    // Einbuchen / Ausbuchen row
    choice_btn_row = lv_obj_create(scr);
    lv_obj_set_size(choice_btn_row, 304, 44);
    lv_obj_align(choice_btn_row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(choice_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(choice_btn_row, 0, 0);
    lv_obj_set_style_pad_all(choice_btn_row, 0, 0);
    lv_obj_clear_flag(choice_btn_row, LV_OBJ_FLAG_SCROLLABLE);

    checkin_btn = lv_btn_create(choice_btn_row);
    lv_obj_set_size(checkin_btn, 145, 44);
    lv_obj_set_pos(checkin_btn, 0, 0);
    lv_obj_set_style_bg_color(checkin_btn, lv_color_hex(CLR_BTN_CHECKIN), 0);
    lv_obj_t *checkin_label = lv_label_create(checkin_btn);
    lv_label_set_text(checkin_label, STR_BTN_CHECKIN);
    lv_obj_center(checkin_label);
    lv_obj_add_event_cb(checkin_btn, checkin_btn_cb, LV_EVENT_CLICKED, nullptr);

    checkout_btn = lv_btn_create(choice_btn_row);
    lv_obj_set_size(checkout_btn, 145, 44);
    lv_obj_set_pos(checkout_btn, 159, 0);
    lv_obj_set_style_bg_color(checkout_btn, lv_color_hex(CLR_BTN_CHECKOUT), 0);
    lv_obj_t *checkout_label = lv_label_create(checkout_btn);
    lv_label_set_text(checkout_label, STR_BTN_CHECKOUT);
    lv_obj_center(checkout_label);
    lv_obj_add_event_cb(checkout_btn, checkout_btn_cb, LV_EVENT_CLICKED, nullptr);

    // ── Test-tag wizard widgets ("Programmieren") ───────────────────────────

    prog_enter_btn = lv_btn_create(scr);
    lv_obj_set_size(prog_enter_btn, 160, 36);
    lv_obj_align(prog_enter_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(prog_enter_btn, lv_color_hex(CLR_BTN_SECONDARY), 0);
    lv_obj_t *prog_enter_label = lv_label_create(prog_enter_btn);
    lv_label_set_text(prog_enter_label, STR_PROG_ENTER_BTN);
    lv_obj_set_style_text_font(prog_enter_label, MY_FONT_14, 0);
    lv_obj_center(prog_enter_label);
    lv_obj_add_event_cb(prog_enter_btn, prog_enter_btn_cb, LV_EVENT_CLICKED, nullptr);

    prog_title = lv_label_create(scr);
    lv_label_set_text(prog_title, "");
    lv_obj_set_style_text_font(prog_title, MY_FONT_16, 0);
    lv_obj_set_style_text_color(prog_title, lv_color_hex(CLR_TEXT_PRIMARY), 0);
    lv_obj_set_width(prog_title, CONTENT_WIDTH);
    lv_label_set_long_mode(prog_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(prog_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(prog_title, LV_ALIGN_TOP_MID, 0, 40);

    // Spans from just below prog_title (68) down to just above prog_cancel_btn's
    // own top edge (188, see its own creation below) — rows are added/removed
    // by prog_refresh_list() per step; this container only ever provides the
    // scrollable viewport around them.
    prog_list = lv_obj_create(scr);
    lv_obj_set_size(prog_list, 304, 116);
    lv_obj_align(prog_list, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_bg_opa(prog_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(prog_list, 0, 0);
    lv_obj_set_style_pad_all(prog_list, 0, 0);
    lv_obj_set_style_pad_row(prog_list, PROG_LIST_GAP, 0);
    lv_obj_set_flex_flow(prog_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(prog_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(prog_list, LV_SCROLLBAR_MODE_AUTO);

    prog_cancel_btn = lv_btn_create(scr);
    lv_obj_set_size(prog_cancel_btn, 304, 44);
    lv_obj_align(prog_cancel_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(prog_cancel_btn, lv_color_hex(CLR_BTN_SECONDARY), 0);
    lv_obj_t *prog_cancel_label = lv_label_create(prog_cancel_btn);
    lv_label_set_text(prog_cancel_label, STR_BTN_CANCEL);
    lv_obj_center(prog_cancel_label);
    lv_obj_add_event_cb(prog_cancel_btn, prog_cancel_btn_cb, LV_EVENT_CLICKED, nullptr);

    prog_confirm_btn_row = lv_obj_create(scr);
    lv_obj_set_size(prog_confirm_btn_row, 304, 44);
    lv_obj_align(prog_confirm_btn_row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(prog_confirm_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(prog_confirm_btn_row, 0, 0);
    lv_obj_set_style_pad_all(prog_confirm_btn_row, 0, 0);
    lv_obj_clear_flag(prog_confirm_btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prog_write_btn = lv_btn_create(prog_confirm_btn_row);
    lv_obj_set_size(prog_write_btn, 145, 44);
    lv_obj_set_pos(prog_write_btn, 0, 0);
    lv_obj_set_style_bg_color(prog_write_btn, lv_color_hex(CLR_SUCCESS), 0);
    lv_obj_t *prog_write_label = lv_label_create(prog_write_btn);
    lv_label_set_text(prog_write_label, STR_BTN_WRITE);
    lv_obj_center(prog_write_label);
    lv_obj_add_event_cb(prog_write_btn, prog_confirm_write_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *prog_confirm_cancel_btn = lv_btn_create(prog_confirm_btn_row);
    lv_obj_set_size(prog_confirm_cancel_btn, 145, 44);
    lv_obj_set_pos(prog_confirm_cancel_btn, 159, 0);
    lv_obj_set_style_bg_color(prog_confirm_cancel_btn, lv_color_hex(CLR_BTN_SECONDARY), 0);
    lv_obj_t *prog_confirm_cancel_label = lv_label_create(prog_confirm_cancel_btn);
    lv_label_set_text(prog_confirm_cancel_label, STR_BTN_CANCEL);
    lv_obj_center(prog_confirm_cancel_label);
    lv_obj_add_event_cb(prog_confirm_cancel_btn, prog_cancel_btn_cb, LV_EVENT_CLICKED, nullptr);

    prog_retry_btn_row = lv_obj_create(scr);
    lv_obj_set_size(prog_retry_btn_row, 304, 44);
    lv_obj_align(prog_retry_btn_row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(prog_retry_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(prog_retry_btn_row, 0, 0);
    lv_obj_set_style_pad_all(prog_retry_btn_row, 0, 0);
    lv_obj_clear_flag(prog_retry_btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prog_retry_btn = lv_btn_create(prog_retry_btn_row);
    lv_obj_set_size(prog_retry_btn, 145, 44);
    lv_obj_set_pos(prog_retry_btn, 0, 0);
    lv_obj_set_style_bg_color(prog_retry_btn, lv_color_hex(CLR_ACCENT), 0);
    lv_obj_t *prog_retry_label = lv_label_create(prog_retry_btn);
    lv_label_set_text(prog_retry_label, STR_BTN_RETRY);
    lv_obj_center(prog_retry_label);
    lv_obj_add_event_cb(prog_retry_btn, prog_retry_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *prog_retry_cancel_btn = lv_btn_create(prog_retry_btn_row);
    lv_obj_set_size(prog_retry_cancel_btn, 145, 44);
    lv_obj_set_pos(prog_retry_cancel_btn, 159, 0);
    lv_obj_set_style_bg_color(prog_retry_cancel_btn, lv_color_hex(CLR_BTN_SECONDARY), 0);
    lv_obj_t *prog_retry_cancel_label = lv_label_create(prog_retry_cancel_btn);
    lv_label_set_text(prog_retry_cancel_label, STR_BTN_CANCEL);
    lv_obj_center(prog_retry_cancel_label);
    lv_obj_add_event_cb(prog_retry_cancel_btn, prog_cancel_btn_cb, LV_EVENT_CLICKED, nullptr);

    // Lone full-width "Überschreiben" button, shown ADDITIONALLY right above
    // prog_retry_btn_row (reused as-is for "Erneut versuchen"/"Abbrechen")
    // only on the "already written" screen — see prog_overwrite_btn's own
    // declaration comment for why this replaced an earlier three-96px-button
    // row design. Positioned with an 8px gap above prog_retry_btn_row's own
    // top edge (that row is height 44 at LV_ALIGN_BOTTOM_MID -8, so its top
    // edge sits at screen_height-8-44=188 on this 240px-tall landscape
    // screen — this button's own bottom edge needs to land at 188-8=180,
    // hence height 40 at offset -60).
    prog_overwrite_btn = lv_btn_create(scr);
    lv_obj_set_size(prog_overwrite_btn, 304, 40);
    lv_obj_align(prog_overwrite_btn, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_bg_color(prog_overwrite_btn, lv_color_hex(CLR_WARNING), 0);
    lv_obj_t *prog_overwrite_label = lv_label_create(prog_overwrite_btn);
    lv_label_set_text(prog_overwrite_label, STR_BTN_OVERWRITE);
    lv_obj_center(prog_overwrite_label);
    lv_obj_add_event_cb(prog_overwrite_btn, prog_overwrite_btn_cb, LV_EVENT_CLICKED, nullptr);

    enter_idle();
}

void ui_on_tag_scanned(const FilamentTagData &tag) {
    // Only react while idle — a tag scanned during PROCESSING/RESULT/etc. is
    // ignored until the current flow finishes (matches the nfc_reader.cpp
    // debounce: the tag needs to be lifted and placed again to re-trigger).
    if (g_state != AppState::IDLE) return;

    if (tag.format == TagFormat::UNKNOWN) {
        enter_result(false, STR_UNKNOWN_TAG, CLR_ERROR);
        // Only info_line1 gets a hint here — info_line2 stays hidden
        // (set_all_hidden() inside enter_result() already did that), since
        // there's no second line of tag info to show for an unrecognized tag.
        lv_label_set_text(info_line1, STR_UNKNOWN_TAG_HINT);
        lv_obj_clear_flag(info_line1, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (tag.weight_grams == 0) {
        enter_weight_pick(tag);
        return;
    }

    // A tag WITH a serial always goes through an explicit confirm step
    // (run_identify()) rather than /scan's auto-deciding action — see
    // enter_confirm_choice()'s own comment. Serial-less tags (OpenSpool,
    // and Creality — whose own serial-like field is deliberately never
    // trusted, see CLAUDE.md #69/#70) already go through run_scan()'s
    // identify_required path unchanged, which never auto-acts either.
    if (tag.has_serial) {
        run_identify(tag);
    } else {
        run_scan(tag);
    }
}

void ui_tick() {
    unsigned long now = millis();

    // Dim the backlight after sitting idle on the waiting-for-a-tag screen —
    // same idea as esp32rental's own idle-dim feature. Restored to full
    // brightness the moment *any* state is entered (see set_all_hidden()).
    if (g_state == AppState::IDLE && now - g_state_entered_at >= TIMEOUT_DIM_MS) {
        gfx.setBrightness(BRIGHTNESS_DIM);
    }

    if (g_state == AppState::PROG_WRITE_A || g_state == AppState::PROG_WRITE_B) {
        ProgramWriteResult result;
        if (nfc_get_write_result(result)) {
            if (result.already_written) {
                // Pairs with nfc_reader.cpp's own print right before it
                // queues this result — if THIS line is missing from a log,
                // the write-result queue never delivered the result to the
                // main loop at all (a bug to chase there); if it IS present
                // but the screen still doesn't show as expected, the bug is
                // in what happens after this point (enter_prog_already_
                // written() itself, or something transitioning g_state away
                // right after).
                Serial.println("  UI: already_written erkannt, zeige Ueberschreiben-Dialog");
                char msg[100];
                snprintf(msg, sizeof(msg), "%s (%s)\n%s", STR_PROG_ALREADY_TITLE,
                         tag_format_name(result.existing_format), STR_PROG_ALREADY_HINT);
                g_prog.tag_a_done = (g_state == AppState::PROG_WRITE_B);
                enter_prog_already_written(msg);
            } else if (result.ok) {
                if (g_state == AppState::PROG_WRITE_A) {
                    g_prog.tag_a_done = true;
                    enter_prog_write(/*tag_a=*/false);
                } else {
                    enter_prog_done();
                }
            } else {
                g_prog.tag_a_done = (g_state == AppState::PROG_WRITE_B);
                enter_prog_result_error(result.error);
            }
        }
    }

    // `now` was captured once at the very top of this function, BEFORE the
    // block above ever runs — but every enter_prog_already_written()/
    // enter_prog_write()/enter_prog_result_error()/enter_prog_done() call in
    // that block sets g_state_entered_at = millis() to a timestamp that can
    // be EQUAL TO OR LATER than that stale `now`. Since both are
    // `unsigned long`, `now - g_state_entered_at` UNDERFLOWS to a huge value
    // (~4 billion) whenever g_state_entered_at ends up even 1ms after `now`
    // — which then satisfies every `>= TIMEOUT_*_MS` check below
    // immediately, firing `enter_idle()` in the very same tick a screen was
    // just shown in, before a render pass could ever display it. Re-fetching
    // `now` here — strictly after every g_state_entered_at write above
    // already happened — guarantees now >= g_state_entered_at for anything
    // the block above just did, eliminating the underflow for exactly this
    // interaction.
    now = millis();

    if (g_state == AppState::RESULT && now - g_state_entered_at >= TIMEOUT_RESULT_MS) {
        enter_idle();
    } else if (g_state == AppState::MANUAL_CHOICE && now - g_state_entered_at >= TIMEOUT_CHOICE_MS) {
        enter_idle();
    } else if (g_state == AppState::PROG_DONE && now - g_state_entered_at >= TIMEOUT_RESULT_MS) {
        enter_idle();
    } else if (is_prog_wizard_state(g_state) && now - g_state_entered_at >= TIMEOUT_PROGRAM_IDLE_MS) {
        // Safety net for "Abbruch sollte jederzeit möglich sein" beyond the
        // explicit Abbrechen button — a wizard screen left untouched (e.g.
        // the station abandoned mid-flow) doesn't block normal scanning
        // forever.
        nfc_disarm_write();
        enter_idle();
    }
}

void ui_on_touch_activity() {
    // Only IDLE ever dims (see ui_tick() above) — every other state already
    // sits at full brightness, restored on transition by set_all_hidden().
    // A touch that misses prog_enter_btn (easy to do once the screen has
    // dimmed enough that the button isn't clearly visible) should still
    // wake the display, not just an actual tag scan or a precisely-aimed
    // tap on the button itself.
    if (g_state != AppState::IDLE) return;
    gfx.setBrightness(BRIGHTNESS_FULL);
    g_state_entered_at = millis();  // restart the idle-dim countdown — without this it would re-dim on the very next ui_tick()
}

void ui_show_ota(int percent) {
    set_all_hidden();
    lv_label_set_text_fmt(banner_label, STR_OTA_UPDATE_FMT, percent);
    lv_obj_set_style_text_font(banner_label, MY_FONT_20, 0);
    lv_obj_set_style_text_color(banner_label, lv_color_hex(CLR_ACCENT), 0);
    lv_obj_clear_flag(banner_label, LV_OBJ_FLAG_HIDDEN);
    // lv_refr_now(), not lv_timer_handler() — see enter_processing()'s own
    // comment on why the latter is a no-op when called from inside an event
    // callback (LVGL's own reentrancy guard). ArduinoOTA's callbacks
    // (setup()'s onStart()/onProgress()) run from ArduinoOTA.handle(), called
    // before lv_timer_handler() in loop() — likely not actually nested today
    // — but lv_refr_now() is the correct tool here regardless of that, with
    // no downside over lv_timer_handler() in a context where it isn't nested.
    lv_refr_now(NULL);
}

void ui_set_wifi_connected(bool connected) {
    lv_obj_set_style_bg_color(wifi_dot, lv_color_hex(connected ? CLR_SUCCESS : CLR_ERROR), 0);
}

AppState ui_get_state() { return g_state; }
