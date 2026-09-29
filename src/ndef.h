#pragma once
#include <Arduino.h>
#include <Adafruit_PN532.h>

// Minimal NDEF (Type 2 Tag) reader for NTAG213/215/216 — just enough to pull
// out a single MIME-type NDEF record, which is what both OpenSpool
// ("application/json") and OpenTag3D ("application/opentag3d") write.
// Not a general-purpose NDEF library: no chunking, no multi-record messages,
// no writing.

struct NdefRecord {
    char           type[48] = "";
    const uint8_t *payload  = nullptr;
    size_t         payload_len = 0;
};

// Reads pages starting at page 4 (the start of the user memory / NDEF area on
// a Type 2 Tag) into `buffer` (caller-owned, must be at least buffer_cap
// bytes) and parses the first NDEF record found in the leading NDEF Message
// TLV (tag byte 0x03). `out.payload`/`out.type` point into `buffer`, so
// `buffer` must stay alive as long as `out` is used.
// Returns false if no NDEF TLV, or the message/record doesn't fit in buffer_cap.
bool ndef_read_first_record(Adafruit_PN532 &nfc, uint8_t *buffer, size_t buffer_cap, NdefRecord &out);

// Writes a single, well-formed "short record" NDEF message (one MIME-type
// record, MB=ME=1, no ID field) starting at page 4, followed by a Terminator
// TLV (0xFE) and zero-padding to the next 4-byte page boundary — used by the
// tag-writing wizard (program mode, see ui.cpp) to produce OpenSpool/
// OpenTag3D test tags. Only supports the 1-byte NDEF-length TLV form
// (message length <= 254 bytes total) — plenty for both formats' payloads —
// and does NOT touch page 3 (the Capability Container): this assumes the
// target is a factory-fresh NTAG213/215/216, which already ships with a
// valid CC declaring the tag NDEF-capable. Returns false (and writes
// nothing useful) if the message would be too long, or if any page write
// fails — e.g. because the physical tag is smaller than the message needs
// (a fixed-layout OpenTag3D record needs ~47 pages, more than an NTAG213's
// user memory can hold — see README.md's tag-format table).
bool ndef_write_message(Adafruit_PN532 &nfc, const char *mime_type, const uint8_t *payload, size_t payload_len);
