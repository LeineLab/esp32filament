#include "tag_bambu.h"
#include "bambu_kdf.h"
#include "strings.h"
#include "hexdump.h"
#include "pn532_auth.h"

static const uint8_t BLOCK_FILAMENT_TYPE = 2;   // sector 0
static const uint8_t BLOCK_COLOR_WEIGHT  = 5;   // sector 1
static const uint8_t BLOCK_TRAY_UID      = 9;   // sector 2
static const uint8_t NUM_SECTORS_NEEDED  = 3;   // sectors 0, 1, 2

// Copies a fixed-width, NUL/space-padded ASCII field out of a 16-byte MIFARE
// block into a NUL-terminated C string, trimming the padding.
static void extract_string(const uint8_t *block, size_t len, char *out, size_t out_cap) {
    size_t copy_len = len < out_cap - 1 ? len : out_cap - 1;
    memcpy(out, block, copy_len);
    out[copy_len] = '\0';
    for (int i = (int)copy_len - 1; i >= 0 && (out[i] == '\0' || out[i] == ' '); i--) {
        out[i] = '\0';
    }
}

// On failure, prints exactly which step failed (auth vs. read) and the
// derived key that was tried — a total auth failure otherwise leaves zero
// trace to debug from (only a successful read ever gets hexdumped, see the
// hexdump() calls below). This can't fix a wrong derived key by itself, but
// it turns "nothing happened" into "here's the exact key that didn't
// authenticate", which can be cross-checked against an independent HKDF
// computation (RFC 5869, salt=BAMBU_MASTER/ikm=UID/info="RFID-A\0", same as
// Bambu-Research-Group/RFID-Tag-Guide's deriveKeys.py) or another tool
// (Flipper Zero, Proxmark3, an NFC-reading Android app) against the same
// physical tag.
static bool auth_and_read(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len,
                           uint8_t block, const uint8_t *sector_key, uint8_t *out16) {
    // Both steps go through pn532_auth.h's retry wrappers, not a single
    // un-retried nfc.mifareclassic_*() call — see its own doc comment: a
    // real, confirmed ESP32 I2C driver bug can corrupt any one PN532
    // transaction, indistinguishable from a genuine key/read rejection from
    // this call's own return value alone, so a single un-retried attempt
    // can't tell "wrong key" from "the bus glitched on this one attempt"
    // apart. A failure reported here (see the printf() below) is now much
    // more likely to be a real one.
    if (!pn532_authenticate_retry(nfc, uid, uid_len, block, sector_key)) {
        char key_hex[18];
        hex_to_str(sector_key, 6, key_hex, sizeof(key_hex));
        Serial.printf("  Bambu Block %u: Authentifizierung fehlgeschlagen (abgeleiteter Schlüssel: %s)\n",
                      (unsigned)block, key_hex);
        return false;
    }
    if (!pn532_read_block_retry(nfc, block, out16)) {
        Serial.printf("  Bambu Block %u: Authentifizierung ok, Lesen fehlgeschlagen\n", (unsigned)block);
        return false;
    }
    return true;
}

// Some PN532 clones' InDataExchange handling can spuriously report success
// against a target that doesn't actually implement MIFARE Classic
// authentication at all (e.g. a tokenized contactless card like Apple Pay,
// which still answers the ISO14443-3 anticollision readPassiveTargetID()
// uses, but implements none of MIFARE Classic's crypto1 auth) —
// mifareclassic_ReadDataBlock() then returns whatever was left over in the
// module's buffer rather than failing cleanly. A "successful" auth+read is
// therefore not, on its own, proof this is really a Bambu tag — see the
// identical reasoning in tag_creality.cpp's looks_like_creality_data(). The
// documented Filament Type field is a plain string, so requiring it to
// actually look like one (printable ASCII, NUL-padded) is cheap, effective
// insurance on top of the already-strong signal of a derived 48-bit key
// happening to authenticate at all.
static bool is_printable_ascii_or_nul(const uint8_t *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t c = p[i];
        if (c != '\0' && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

// ── Writing (program mode / test-tag wizard) ────────────────────────────────

static const uint8_t TRAILER_BLOCK[NUM_SECTORS_NEEDED] = {3, 7, 11};  // sector 0/1/2 trailers
static const uint8_t MIFARE_DEFAULT_KEY[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Standard MIFARE Classic "transport configuration" access bits (0xFF 0x07
// 0x80) + the common factory General Purpose Byte (0x69) — i.e. the exact
// same access-bits/GPB combination virtually every blank MIFARE Classic card
// already ships with. Deliberately reused as-is rather than inventing a
// different bit pattern: it already grants Key A full read/write on the data
// blocks and the trailer, which is all this project's own reader (Key-A-only
// auth, see auth_and_read() above) ever needs, and it's the single
// best-tested access-bits value in the wild.
static const uint8_t TRAILER_ACCESS_BITS_GPB[4] = {0xFF, 0x07, 0x80, 0x69};

static bool write_block_with_key(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len,
                                  const uint8_t *key6, uint8_t block, const uint8_t *data16) {
    if (!pn532_authenticate_retry(nfc, uid, uid_len, block, key6)) return false;
    return pn532_write_block_retry(nfc, block, data16);
}

// Re-authenticates with `current_key6` — the key that actually authenticated
// this sector's own data-block write a moment ago (see tag_write_bambu()'s
// own comment: this is the plain default key on a genuinely virgin card, but
// the sector's OWN derived key when Überschreiben is re-writing a tag that's
// already Bambu-formatted, since that's what's actually guarding it by
// then) — and overwrites the trailer with the sector's derived Key A/B. When
// `current_key6` already IS `derived_key6` (the already-formatted case),
// this "relocks" the sector with the exact value it already holds — a
// harmless no-op rewrite, not a real re-keying, since both are the same
// UID-derived key computed the same way every time. Key B is set to the same
// derived value as Key A: this project's reader never authenticates with Key
// B at all (see auth_and_read()'s keyNumber=0), so there's no reason to give
// it a different, undocumented value — a known, derivable value is strictly
// better than an arbitrary one if this sector's key ever needs recovering.
static bool lock_sector_with_derived_key(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len,
                                          uint8_t trailer_block, const uint8_t *current_key6,
                                          const uint8_t *derived_key6) {
    if (!pn532_authenticate_retry(nfc, uid, uid_len, trailer_block, current_key6)) return false;

    uint8_t trailer[16];
    memcpy(trailer, derived_key6, 6);
    memcpy(trailer + 6, TRAILER_ACCESS_BITS_GPB, 4);
    memcpy(trailer + 10, derived_key6, 6);
    return pn532_write_block_retry(nfc, trailer_block, trailer);
}

bool tag_write_bambu(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len, const FilamentTagData &in,
                      char *err, size_t err_cap) {
    uint8_t block2[16] = {0};
    strncpy((char *)block2, in.type_name, sizeof(block2));  // NUL-padded (strncpy, not strlcpy: padding is the point here)

    uint8_t block5[16] = {0};
    unsigned r = 0, g = 0, b = 0;
    if (in.color[0] == '#') sscanf(in.color + 1, "%2x%2x%2x", &r, &g, &b);
    block5[0] = (uint8_t)r;
    block5[1] = (uint8_t)g;
    block5[2] = (uint8_t)b;
    block5[3] = 0xFF;  // alpha — Bambu tags encode RGBA, this project's own reader only ever uses RGB
    block5[4] = (uint8_t)(in.weight_grams & 0xFF);
    block5[5] = (uint8_t)((in.weight_grams >> 8) & 0xFF);

    uint8_t block9[16] = {0};
    strncpy((char *)block9, in.vendor_serial, sizeof(block9));

    uint8_t keys[NUM_SECTORS_NEEDED * 6];
    bambu_derive_sector_keys(uid, uid_len, NUM_SECTORS_NEEDED, keys);

    // Try the default key first (a genuinely blank card) — only on block 2,
    // sector 0's data block; if that authenticates, the rest of the tag is
    // blank too (a real Bambu tag, and any test tag this wizard already
    // wrote, is always either fully blank or fully locked across all three
    // sectors, never a mix), so blocks 5/9 use the same key scheme without
    // re-probing each one individually.
    //
    // Überschreiben (nfc_arm_write()'s allow_overwrite) against any tag this
    // wizard already wrote in an earlier run — or a real factory Bambu spool
    // — would otherwise fail here identically to the same class of bug fixed
    // on the Creality write path (see tag_creality.cpp's own doc comment):
    // tag_write_bambu() itself is what locks a sector to its derived key on
    // first write (see lock_sector_with_derived_key() above), so a
    // default-key-only write can never authenticate against an
    // already-rekeyed sector. Falls back to the same UID-derived key
    // (computed once here, reused for both the fallback write and the later
    // lock step) when the default key doesn't authenticate, with the same
    // reselect-after-failed-auth this codebase relies on elsewhere (see the
    // identical pattern in tag_creality.cpp). Not yet confirmed against real
    // hardware — Überschreiben has so far only been exercised against
    // Creality tags.
    bool use_derived_keys = !write_block_with_key(nfc, uid, uid_len, MIFARE_DEFAULT_KEY, BLOCK_FILAMENT_TYPE, block2);
    if (use_derived_keys) {
        uint8_t reselect_uid[7];
        uint8_t reselect_len = uid_len;
        nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, reselect_uid, &reselect_len, 200);
        if (!write_block_with_key(nfc, uid, uid_len, keys + 0, BLOCK_FILAMENT_TYPE, block2)) {
            snprintf(err, err_cap, STR_ERR_BLOCK_NOT_WRITABLE_FMT, (unsigned)BLOCK_FILAMENT_TYPE);
            return false;
        }
    }
    if (!write_block_with_key(nfc, uid, uid_len, use_derived_keys ? keys + 6 : MIFARE_DEFAULT_KEY,
                               BLOCK_COLOR_WEIGHT, block5)) {
        strlcpy(err, STR_ERR_WRITE_FAILED_BLOCK5, err_cap);
        return false;
    }
    if (!write_block_with_key(nfc, uid, uid_len, use_derived_keys ? keys + 12 : MIFARE_DEFAULT_KEY,
                               BLOCK_TRAY_UID, block9)) {
        strlcpy(err, STR_ERR_WRITE_FAILED_BLOCK9, err_cap);
        return false;
    }

    for (uint8_t s = 0; s < NUM_SECTORS_NEEDED; s++) {
        const uint8_t *current_key = use_derived_keys ? (keys + s * 6) : MIFARE_DEFAULT_KEY;
        if (!lock_sector_with_derived_key(nfc, uid, uid_len, TRAILER_BLOCK[s], current_key, keys + (s * 6))) {
            snprintf(err, err_cap, STR_ERR_SECTOR_LOCK_FAILED_FMT, (unsigned)s);
            return false;
        }
    }
    return true;
}

bool tag_parse_bambu(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len, FilamentTagData &out) {
    uint8_t keys[NUM_SECTORS_NEEDED * 6];
    bambu_derive_sector_keys(uid, uid_len, NUM_SECTORS_NEEDED, keys);

    uint8_t block[16];

    // Sector 0 — Filament Type. Also serves as our "is this actually a Bambu
    // tag" probe: if authentication fails, or the content doesn't look like
    // plausible data (see is_printable_ascii_or_nul()), it's not (or the
    // KDF/library assumptions above don't hold for this particular tag) —
    // caller falls back to trying Creality, then UNKNOWN.
    if (!auth_and_read(nfc, uid, uid_len, BLOCK_FILAMENT_TYPE, keys + 0, block)) return false;
    hexdump("Bambu Block 2 (Filament Type)", block, sizeof(block));
    if (!is_printable_ascii_or_nul(block, sizeof(block))) {
        Serial.println("  Bambu Block 2: authentifiziert und gelesen, Inhalt sieht aber nicht wie ein Bambu-Tag aus");
        return false;
    }
    extract_string(block, sizeof(block), out.type_name, sizeof(out.type_name));

    out.format = TagFormat::BAMBU;
    strlcpy(out.brand, "Bambu Lab", sizeof(out.brand));
    if (out.type_name[0] == '\0') strlcpy(out.type_name, "PLA", sizeof(out.type_name));

    // Sector 1 — Color RGBA (offset 0, 4 bytes) + Spool Weight (offset 4, uint16 LE).
    if (auth_and_read(nfc, uid, uid_len, BLOCK_COLOR_WEIGHT, keys + 6, block)) {
        hexdump("Bambu Block 5 (Color/Weight)", block, sizeof(block));
        snprintf(out.color, sizeof(out.color), "#%02X%02X%02X", block[0], block[1], block[2]);
        out.weight_grams = static_cast<uint16_t>(block[4]) | (static_cast<uint16_t>(block[5]) << 8);
    } else {
        strlcpy(out.color, "unbekannt", sizeof(out.color));
        out.weight_grams = 0;
    }

    // Sector 2 — Tray UID (offset 0, 16 bytes) — the per-spool serial. This
    // field is NOT reliably printable ASCII text — a real spool's Tray UID
    // can be raw binary. Treating raw binary as a NUL/space-padded C string
    // (extract_string(), used everywhere else in this file) leaves embedded
    // NUL bytes and non-UTF-8 garbage in vendor_serial — invalid inside a
    // JSON string sent to the backend (the API rejects it as malformed
    // JSON). Hex-encode instead whenever the raw bytes aren't already
    // printable ASCII/NUL — always JSON-safe, and still a perfectly good
    // unique-per-spool identifier even when it isn't human-readable.
    if (auth_and_read(nfc, uid, uid_len, BLOCK_TRAY_UID, keys + 12, block)) {
        hexdump("Bambu Block 9 (Tray UID)", block, sizeof(block));
        if (is_printable_ascii_or_nul(block, sizeof(block))) {
            extract_string(block, sizeof(block), out.vendor_serial, sizeof(out.vendor_serial));
        } else {
            hex_encode_compact(block, sizeof(block), out.vendor_serial, sizeof(out.vendor_serial));
        }
        out.has_serial = out.vendor_serial[0] != '\0';
    } else {
        out.vendor_serial[0] = '\0';
        out.has_serial = false;
    }

    return true;
}
