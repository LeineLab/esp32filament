#pragma once
#include <Adafruit_PN532.h>
#include "tag_types.h"

// Bambu Lab RFID tag format: MIFARE Classic 1K, per-sector keys derived from
// the tag's own UID (see bambu_kdf.h). Block layout from
// Bambu-Research-Group/RFID-Tag-Guide (BambuLabRfid.md):
//   Block 2 (sector 0): Filament Type (16-byte string)
//   Block 5 (sector 1): Color RGBA (4 bytes) + Spool Weight (uint16 LE, grams)
//   Block 9 (sector 2): Tray UID (16-byte string) — used as vendor_serial
//
// Only reads the sectors/blocks this project actually needs (brand is always
// reported as "Bambu Lab" — it isn't itself an on-tag field).
//
// Call only after readPassiveTargetID() has already identified a 4-byte-UID
// ISO14443A target (see nfc_reader.cpp) — this does NOT re-poll for a target.
bool tag_parse_bambu(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len, FilamentTagData &out);

// Writes a test tag in Bambu Lab's format (program mode, see ui.cpp/
// nfc_reader.cpp's write-request handling). Uses `in.type_name`/`in.color`
// (must be "#RRGGBB")/`in.weight_grams`/`in.vendor_serial`. Requires a BLANK
// or still-factory-default-keyed MIFARE Classic 1K card: it authenticates
// with the default key (0xFFFFFFFFFFFF) to write the three data blocks
// first, and only once all three succeed does it go back and overwrite each
// sector's trailer with that sector's HKDF-derived key (see bambu_kdf.h) —
// exactly what makes a real Bambu tag only readable via the derived key
// instead of the default one, needed for tag_parse_bambu() (and therefore
// the normal /filament/scan reading path) to actually recognize the result
// as a genuine Bambu tag afterward. Sequencing data-then-lock this way means
// a failure partway through the data writes never leaves a half-locked tag —
// it just stays fully blank/default-keyed and safe to retry from scratch.
// A tag that doesn't accept the default key at all (already locked — a real
// spool, or one this wizard already wrote) fails cleanly at the very first
// write with `err` explaining why, never silently overwriting it.
bool tag_write_bambu(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len, const FilamentTagData &in,
                      char *err, size_t err_cap);
