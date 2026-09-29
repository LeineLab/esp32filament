#pragma once
#include <Adafruit_PN532.h>
#include "tag_types.h"

// OpenTag3D format (https://opentag3d.info/spec): NTAG213/215/216, one NDEF
// record of type "application/opentag3d" with a fixed-offset binary payload
// (UTF-8 strings + big-endian integers). Carries a "Serial/Batch ID" field —
// used as vendor_serial here, though the spec itself leaves it up to the
// tag's writer whether that value is unique per spool or shared per
// production batch; see README.md.
//
// Returns true if an OpenTag3D record was found and parsed.
bool tag_parse_opentag3d(Adafruit_PN532 &nfc, FilamentTagData &out);

// Writes a test tag in this format (program mode, see ui.cpp/nfc_reader.cpp's
// write-request handling). `in.type_name` is expected as "Base" or
// "Base Modifier" (the wizard's own base-material + modifier picks combined
// with a space, see build_prog_write_tag() in ui.cpp) and is split back
// apart on the first space into this format's own genuinely separate Base
// Material (5 bytes) and Modifiers (5 bytes) fields — the one format able to
// store the two independently, unlike Bambu/Creality/OpenSpool's single
// combined text field. Both are silently truncated at 5 bytes if longer
// (e.g. "Luminous" -> "Lumin") — a real constraint of this format's fixed
// layout (see README.md's tag-format table), accounted for by
// nfc_reader.cpp's own write-verification rather than avoided here.
// `in.color` (must be "#RRGGBB") is written as Color RGBA; Color Name is
// left blank, matching tag_parse_opentag3d()'s own hex-fallback path.
// `in.brand` is written to Manufacturer verbatim (the wizard always has the
// user pick one for this format — see prog_format_has_brand_field() in
// ui.cpp). `in.weight_grams` -> Target Weight, `in.vendor_serial` ->
// Serial/Batch ID. No UID/key handling needed (unlike Bambu) — this is a
// plain NDEF write via ndef_write_message(), same underlying mechanism as
// OpenSpool.
bool tag_write_opentag3d(Adafruit_PN532 &nfc, const FilamentTagData &in, char *err, size_t err_cap);
