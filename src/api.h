#pragma once
#include <Arduino.h>
#include "tag_types.h"

// ── Response structures ───────────────────────────────────────────────────────

struct ApiScanResult {
    bool ok = false;              // false = network/HTTP/parse error, see `error`
    bool identify_required = false;
    char action[16] = "";         // "checked_in" | "checked_out" (only when !identify_required)
    char brand_name[32] = "";
    char type_name[48]  = "";
    char color[24]      = "";
    uint16_t weight_grams = 0;
    char vendor_serial[40] = "";
    int  current_stock = -1;      // set only when identify_required
    char error[128] = "";
};

struct ApiActionResult {
    bool ok = false;
    char brand_name[32] = "";
    char type_name[48]  = "";
    char color[24]      = "";
    uint16_t weight_grams = 0;
    char error[128] = "";
};

struct ApiStatusResult {
    bool ok = false;
    bool in_stock = false;
    char error[128] = "";
};

// Initialise HTTP client (call once after WiFi connects).
void api_init();

// POST /api/v1/filament/scan — deterministic for a serialized tag (checks a
// known serial out, or auto-creates brand/type and checks a new one in).
// With tag.has_serial == false, takes no action; out.identify_required is
// set and out.current_stock reports the matching spec's stock instead.
// NOTE: for a tag WITH a serial, this firmware no longer calls this directly
// from a scan — see api_status()/ui.cpp's run_identify() below. Kept intact
// (not dead code): /scan itself is still a fully valid, tested backend
// endpoint, this firmware just chooses to always confirm before acting now.
bool api_scan(const FilamentTagData &tag, ApiScanResult &out);

// POST /api/v1/filament/status — read-only lookup for a serial's current
// in-stock status, never checks anything in or out. Used instead of
// api_scan() whenever tag.has_serial is true, so the station can show an
// explicit Einbuchen/Ausbuchen confirmation before acting — scanning the
// same physical spool twice in a row would otherwise silently flip its
// state via api_scan()'s deterministic auto-action, with no chance for the
// operator to notice.
bool api_status(const FilamentTagData &tag, ApiStatusResult &out);

// POST /api/v1/filament/checkin — explicit check-in (auto-creates brand/type
// by name). `weight_grams` overrides tag.weight_grams (e.g. from the
// on-screen picker when the tag itself didn't encode a weight).
bool api_checkin(const FilamentTagData &tag, uint16_t weight_grams, ApiActionResult &out);

// POST /api/v1/filament/checkout — explicit check-out. Without a serial,
// removes the oldest in-stock roll matching brand/type/weight/color (FIFO).
bool api_checkout(const FilamentTagData &tag, uint16_t weight_grams, ApiActionResult &out);
