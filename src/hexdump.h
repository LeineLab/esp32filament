#pragma once
#include <Arduino.h>

// Serial diagnostic helper — dumps raw tag bytes (and a short hex UID) to
// Serial (115200 baud, see main.cpp's Serial.begin()) so real spool data can
// be captured during on-site testing and fed back for reference. See
// README.md's "Serial-Diagnose" section for the full picture: which
// tag_parse_*() call sites use this and why, and how to capture a session's
// output. Not gated behind a build flag — this is plain Serial.printf(), the
// same as every other diagnostic line already in this firmware, and costs
// nothing when nobody has a serial monitor open.

inline void hexdump(const char *label, const uint8_t *data, size_t len) {
    Serial.printf("  %s (%u Byte):\n", label, (unsigned)len);
    for (size_t i = 0; i < len; i += 16) {
        Serial.printf("    %04u: ", (unsigned)i);
        size_t line_len = (len - i < 16) ? (len - i) : 16;
        for (size_t j = 0; j < 16; j++) {
            if (j < line_len) Serial.printf("%02X ", data[i + j]);
            else Serial.print("   ");
        }
        Serial.print(" |");
        for (size_t j = 0; j < line_len; j++) {
            uint8_t c = data[i + j];
            Serial.print((c >= 0x20 && c <= 0x7E) ? (char)c : '.');
        }
        Serial.println("|");
    }
}

// Compact one-line hex string for a short buffer (e.g. a 4/7-byte NFC UID) --
// `out` must be at least 3*len bytes (two hex digits + separator per byte,
// no trailing separator, plus the NUL terminator fits within that).
inline void hex_to_str(const uint8_t *data, size_t len, char *out, size_t out_cap) {
    out[0] = '\0';
    for (size_t i = 0; i < len; i++) {
        char piece[4];
        snprintf(piece, sizeof(piece), "%s%02X", i ? " " : "", data[i]);
        strlcat(out, piece, out_cap);
    }
}

// Same idea as hex_to_str(), but with no separator between bytes — for a
// value that needs to actually round-trip as one token (a JSON string field,
// an API request parameter), not just be human-readable on a Serial
// console. `out` must be at least 2*len+1 bytes. See tag_bambu.cpp's use for
// why this exists: a Bambu tag's Tray UID field isn't reliably printable
// ASCII text on real hardware, and a raw binary/non-UTF-8 vendor_serial
// breaks JSON serialization outright.
inline void hex_encode_compact(const uint8_t *data, size_t len, char *out, size_t out_cap) {
    out[0] = '\0';
    for (size_t i = 0; i < len; i++) {
        char piece[3];
        snprintf(piece, sizeof(piece), "%02X", data[i]);
        strlcat(out, piece, out_cap);
    }
}
