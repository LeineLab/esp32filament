#pragma once
#include <Arduino.h>
#include "tag_types.h"

// PN532 NFC reader (I2C, no IRQ pin) — runs in a FreeRTOS task, same pattern
// as esp32rental/src/nfc_reader.cpp. Parsed tag data delivered via queue.
//
// Tag-type dispatch happens inside the task: an ISO14443A target with a
// 7-byte UID is treated as an NTAG21x candidate (OpenSpool / OpenTag3D, both
// NDEF-based); a shorter UID (4 bytes, the common case) is treated as a
// MIFARE Classic 1K candidate (Creality tried first via the default key,
// then Bambu Lab via its derived key) — see nfc_reader.cpp.

// Initialise PN532 and start the polling task. Must be called after Wire.begin()
// is NOT required — this calls Wire.begin() itself with the configured pins.
bool nfc_init();

// Peek without consuming — true if a newly parsed tag is pending.
bool nfc_available();

// Consume and return the latest parsed tag. Returns false if nothing pending.
bool nfc_read(FilamentTagData &out);

// ── Program mode (test-tag writing wizard, see ui.cpp) ──────────────────────
//
// All actual PN532 I/O — including writing — happens exclusively inside the
// nfc_task started by nfc_init(), same as every read in this project: the
// PN532 sits on a single shared I2C bus, so a write triggered directly from
// ui.cpp/the main loop while nfc_task is mid-poll would be a genuine
// concurrent-access bug, not just bad style. The functions below are the
// only way ui.cpp reaches into that task: nfc_arm_write() hands over what to
// write next time *any* new tag is detected (single-shot — the task consumes
// the request the moment it acts on it), and the result comes back via the
// same peek/receive queue pattern nfc_available()/nfc_read() already use.

struct ProgramWriteResult {
    bool ok = false;
    bool already_written = false;     // the presented tag already parses as a known format — refused, nothing was touched
    TagFormat existing_format = TagFormat::UNKNOWN;  // set when already_written
    // 100, not 80 — found too small by inspection while moving these
    // messages into strings.h: STR_ERR_NDEF_WRITE_FAILED_OPENTAG3D alone is
    // 90 bytes (UTF-8, "nötig"'s ö included), which an 80-byte buffer would
    // have silently truncated via strlcpy() rather than overflowed (strlcpy
    // is always null-terminated/bounded, so this was never a memory-safety
    // bug — just a cut-off message on screen).
    char error[100] = "";
};

// Arms a one-shot write: `desired.format`/`type_name`/`color`/`weight_grams`/
// `vendor_serial` (has_serial set accordingly) describe what to write to
// whichever physical tag is detected next. Safe to call repeatedly (e.g. to
// re-arm after a failed attempt) — each call replaces any still-pending one.
//
// `allow_overwrite` (default false) skips the "refuse to overwrite an
// already-written tag" safety check in handle_write_request() entirely —
// so a tag can be deliberately transferred from one spool to a new one with
// different filament, rather than only ever being writable once. Left false
// for a normal write; ui.cpp only ever
// passes true from its own explicit overwrite confirmation, shown
// after a plain write already reported the tag as non-blank — never as a
// silent default, since accidentally clobbering a real spool's tag is
// exactly what the safety check exists to prevent.
void nfc_arm_write(const FilamentTagData &desired, bool allow_overwrite = false);

// Cancels a pending armed write (e.g. the user tapped Abbrechen) so the next
// detected tag is parsed normally instead of written to.
void nfc_disarm_write();

bool nfc_write_result_available();
bool nfc_get_write_result(ProgramWriteResult &out);
