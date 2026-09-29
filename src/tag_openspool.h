#pragma once
#include <Adafruit_PN532.h>
#include "tag_types.h"

// OpenSpool format (spuder/OpenSpool): NTAG215/216, one NDEF record of type
// "application/json" holding {protocol, version, type, color_hex, brand,
// min_temp, max_temp}. No serial number, no weight field at all — the
// station must ask the user for both when this format is detected.
// https://github.com/spuder/OpenSpool
//
// Returns true if an OpenSpool record was found and parsed (even if some
// optional fields were missing) — out.has_serial is always false.
bool tag_parse_openspool(Adafruit_PN532 &nfc, FilamentTagData &out);

// Writes a test tag in this format (program mode, see ui.cpp/nfc_reader.cpp's
// write-request handling). Uses `in.type_name`, `in.color` (must be
// "#RRGGBB"), and `in.brand` — the wizard always has the user pick a brand
// for this format (a real, writable JSON field, unlike Bambu/Creality — see
// prog_format_has_brand_field() in ui.cpp), so it's always written verbatim
// rather than relying on tag_parse_openspool()'s own `"Generic"` fallback.
// No weight, no serial — OpenSpool carries neither by design (see
// tag_openspool.h above); the wizard's confirmation screen makes this
// limitation explicit rather than silently writing a made-up serial this
// format has no field for.
bool tag_write_openspool(Adafruit_PN532 &nfc, const FilamentTagData &in, char *err, size_t err_cap);
