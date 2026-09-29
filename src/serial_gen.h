#pragma once
#include <Arduino.h>
#include <esp_random.h>

// Generates a random uppercase hex string into `out` (which must have room
// for num_chars+1 bytes) — used by the tag-writing wizard (ui.cpp/program
// mode) to invent a vendor_serial-shaped value for a freshly-programmed test
// tag. Uses the ESP32 hardware RNG (esp_random()) — there is no
// cryptographic requirement here, this only needs to look plausible and
// avoid colliding with another freshly-written test tag.
inline void gen_hex_serial(char *out, size_t out_cap, size_t num_chars) {
    // Named HEX_DIGITS, not HEX — Arduino's Print.h #defines a bare `HEX` to
    // 16 (for Serial.print(x, HEX)), which silently mangles a same-named
    // local into "16[]" and fails to compile with a confusing error pointing
    // at Print.h instead of here.
    static const char HEX_DIGITS[] = "0123456789ABCDEF";
    if (num_chars + 1 > out_cap) num_chars = out_cap - 1;
    for (size_t i = 0; i < num_chars; i++) {
        out[i] = HEX_DIGITS[esp_random() & 0x0F];
    }
    out[num_chars] = '\0';
}

// Same idea, but plain ASCII decimal digits — for a field whose real-tag
// content is validated as decimal digits, not hex (e.g. Creality's K2/CFS
// layout SERIAL_POS field, see tag_creality.cpp's is_ascii_digits() check).
inline void gen_decimal_serial(char *out, size_t out_cap, size_t num_chars) {
    if (num_chars + 1 > out_cap) num_chars = out_cap - 1;
    for (size_t i = 0; i < num_chars; i++) {
        out[i] = '0' + (esp_random() % 10);
    }
    out[num_chars] = '\0';
}
