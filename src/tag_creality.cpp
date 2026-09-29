#include "tag_creality.h"
#include "serial_gen.h"
#include "creality_k2_materials.h"
#include "strings.h"
#include "hexdump.h"
#include "pn532_auth.h"
#include <mbedtls/aes.h>
#include <ctype.h>

static const uint8_t BLOCK_FIRST = 4;  // sector 1, blocks 4-6 (block 7 is the sector trailer)
static const uint8_t MIFARE_DEFAULT_KEY[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── "Old" layout: Bambu-Research-Group/RFID-Tag-Guide's CrealityRfid.md ────
// Field layout within the 48-byte concatenation of blocks 4+5+6 (position/
// length exactly as documented there — see tag_creality.h for caveats).
namespace CrealityOld {
    static const size_t BATCH_POS = 0,  BATCH_LEN = 3;   // hex
    static const size_t DATE_POS  = 3,  DATE_LEN  = 5;   // YYMDD
    static const size_t SUPPLIER_POS = 8, SUPPLIER_LEN = 4;   // hex — theorized to distinguish Generic/Creality/eSun, mapping unconfirmed
    static const size_t MATERIAL_POS = 12, MATERIAL_LEN = 5;  // hex
    static const size_t COLOR_POS    = 17, COLOR_LEN    = 6;  // RGB hex digits, no '#'
}

// ── "K2/CFS" layout: DnG-Crafts/K2-RFID ─────────────────────────────────────
// A considerably more detailed, cross-checked (Android app + Arduino
// firmware + Windows app all embed the identical constants) reverse-
// engineering of what appears to be Creality's newer K2-printer/CFS
// multi-material tag scheme — see tag_creality.h for how this relates to
// (and differs from) the "old" layout above, and why both are supported.
namespace CrealityK2 {
    // pos 0-11 is NOT a fixed signature, despite looking like one on a small
    // sample. Two factory-original tags (a Creality Hyper PLA Stardust and a
    // Hyper PLA White spool) both had "AB"/"3A" varying at pos 0-1 but a
    // consistent "A2"+'1' at pos 9-11; a third real spool — written with the
    // open-source DnG-Crafts Android app rather than by Creality's own
    // firmware, and confirmed readable by a real Creality K2 Plus printer —
    // has neither ("12..." at pos 0-1, "49"+'D' at pos 9-11, vendor "027E"
    // instead of "0276"). Since a real printer accepts that data, none of
    // pos 0-11 is a required signature, even though "A2"+'1' does appear
    // tied to vendor "0276" specifically. Only pos 17's '0' marker has held
    // across every real sample — see looks_like_k2_data() below. Pos 0-4's
    // real meaning (batch code? something vendor-specific?) is unknown, and
    // pos 5-8 ("vendor") isn't always ASCII digits (the app-written sample's
    // "027E" has a letter) — validated only as printable, like every other
    // undecoded byte here.
    // pos 5-8 — "0276" confirmed = Creality. The app-written Generic-PLA
    // sample above has "027E" here instead, suggesting this field may track
    // the WRITTEN material's own brand (Creality vs. Generic) rather than
    // being a fixed "Creality ecosystem" constant — plausible, but only one
    // data point against that hypothesis, not enough to hardcode a second
    // confirmed vendor-code→brand mapping the way "0276"→Creality is.
    // fill_from_k2() therefore still only trusts "0276" and shows anything
    // else honestly as "Creality (Vendor <code>)" rather than guessing
    // "027E" means Generic specifically — revisit if more samples confirm it.
    static const size_t  VENDOR_POS = 5, VENDOR_LEN = 4;
    // pos 9-16: no longer checked for a specific value — see the namespace
    // comment above. pos 12-16 is still the material_id field (below); pos
    // 9-11 is real data (apparently vendor-dependent, per the evidence
    // above) that this firmware doesn't decode.
    static const size_t MATERIAL_ID_POS = 12, MATERIAL_ID_LEN = 5; // pos 12-16, opaque lookup key into Creality's own material database — not decodable to a name here, see tag_creality.h; confirmed against three real spools (01004→Hyper Stardust, 01001→Hyper PLA, 00001→Generic PLA, all matching their physical labels)
    // pos 17 is a fixed '0' marker (start of the source's "color" field) —
    // the one pos-0-17 byte confirmed constant across every real sample,
    // factory-original and app-written alike.
    static const size_t COLOR_POS  = 18, COLOR_LEN  = 6;          // pos 18-23, RGB hex digits, no '#' — confirmed against real spools (all visually plausible for their actual colors)
    static const size_t LENGTH_POS = 24, LENGTH_LEN = 4;          // pos 24-27, a spool-length code (see k2_length_code_to_grams()), NOT grams directly — CONFIRMED: "0330" on all three real 1kg/330m samples
    static const size_t SERIAL_POS = 28, SERIAL_LEN = 6;          // pos 28-33, decimal digits — NOT the same as whatever serial is printed on the spool's own paper label, see tag_creality.h
    // pos 34-47 (14 bytes): reserved — all zero in the DnG-Crafts demo
    // string, but genuinely variable on real tags (0x04, 0x03, and 0x13 at
    // pos 40 across the three real samples so far, otherwise zero) —
    // unknown meaning, not validated as printable/anything else (see
    // PLAUSIBLE_CHECK_LEN below).
    static const size_t PLAUSIBLE_CHECK_LEN = 34;  // = SERIAL_POS + SERIAL_LEN — only this prefix is checked for plausibility, see looks_like_k2_data()

    // Fixed AES-128 keys, embedded in plaintext in DnG-Crafts/K2-RFID's own
    // source (Android Utils.java's createKey()/cipherData(), byte-for-byte
    // identical to the Arduino firmware's AES.cpp u_key/d_key arrays) — not
    // a secret being protected here, just two published constants that
    // happen to be AES keys, needed to interoperate with tags that scheme
    // already wrote.
    static const uint8_t UID_KEY[16]  = {113, 51, 98, 117, 94, 116, 49, 110, 113, 102, 90, 40, 112, 102, 36, 49};
    static const uint8_t DATA_KEY[16] = {72, 64, 67, 70, 107, 82, 110, 122, 64, 75, 65, 116, 66, 74, 112, 50};
}

// On failure, prints exactly which step failed (auth vs. read), the block,
// and which key was tried — same reasoning as tag_bambu.cpp's own
// auth_and_read(): before this, a total auth failure across all of
// tag_parse_creality()'s attempts left zero trace, since only a successful
// read ever got hexdumped.
static bool auth_and_read_block(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len,
                                 const uint8_t *key6, uint8_t block, uint8_t *out16, const char *key_label) {
    // Both steps retried via pn532_auth.h — see its own doc comment and
    // tag_bambu.cpp's identical use of it: a confirmed ESP32 I2C driver bug
    // can corrupt any one PN532 transaction, so a single un-retried call
    // can't tell "wrong key" from "the bus glitched on this one attempt"
    // apart, which every real-hardware failure reported against this
    // function so far could equally have been.
    if (!pn532_authenticate_retry(nfc, uid, uid_len, block, key6)) {
        char key_hex[18];
        hex_to_str(key6, 6, key_hex, sizeof(key_hex));
        Serial.printf("  Creality Block %u (%s): Authentifizierung fehlgeschlagen (Schlüssel: %s)\n",
                      (unsigned)block, key_label, key_hex);
        return false;
    }
    if (!pn532_read_block_retry(nfc, block, out16)) {
        Serial.printf("  Creality Block %u (%s): Authentifizierung ok, Lesen fehlgeschlagen\n",
                      (unsigned)block, key_label);
        return false;
    }
    return true;
}

// Authenticates sector 1 (blocks 4-6) with the given key and reads all 48
// bytes — shared by every read attempt below, plain or K2-encrypted, since
// they only differ in which key is used and whether the result needs an AES
// decrypt pass afterward.
static bool auth_and_read_sector1(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len,
                                   const uint8_t *key6, uint8_t *out48, const char *key_label) {
    for (uint8_t i = 0; i < 3; i++) {
        if (!auth_and_read_block(nfc, uid, uid_len, key6, BLOCK_FIRST + i, out48 + i * 16, key_label)) return false;
    }
    return true;
}

static bool is_hex_digits(const uint8_t *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t c = p[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

static bool is_ascii_digits(const uint8_t *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
    }
    return true;
}

static bool is_printable_ascii(const uint8_t *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] < 0x20 || p[i] > 0x7E) return false;
    }
    return true;
}

// 0xFFFFFFFFFFFF is the MIFARE Classic factory-default key — it authenticates
// against countless generic/blank MIFARE Classic cards that have nothing to
// do with Creality (any card whose sectors were never rekeyed), and can even
// spuriously "succeed" via some PN532 clones' InDataExchange handling against
// a non-MIFARE-Classic ISO14443A
// target entirely (a virtual/tokenized contactless card, e.g. Apple Pay,
// still responds to the ISO14443-3 anticollision readPassiveTargetID() uses,
// even though it implements none of MIFARE Classic's crypto1 auth) — in that
// case mifareclassic_ReadDataBlock() can return whatever was left over in
// the module's internal buffer rather than a clean failure. A successful
// auth+read is therefore not, on its own, good evidence this is really a
// Creality tag in either supported layout — both plausibility checks below
// require the whole 48 bytes to actually look like their own documented
// shape before accepting a match.
static bool looks_like_old_data(const uint8_t *data) {
    if (!is_printable_ascii(data, 48)) return false;
    return is_hex_digits(data + CrealityOld::BATCH_POS, CrealityOld::BATCH_LEN) &&
           is_hex_digits(data + CrealityOld::SUPPLIER_POS, CrealityOld::SUPPLIER_LEN) &&
           is_hex_digits(data + CrealityOld::MATERIAL_POS, CrealityOld::MATERIAL_LEN) &&
           is_hex_digits(data + CrealityOld::COLOR_POS, CrealityOld::COLOR_LEN);
}

// Validates only what real-hardware testing (see CrealityK2's own namespace
// comment) confirmed is actually fixed: pos 17's '0' marker, plus requiring
// the fields this code actually decodes and uses (color/length/serial) to
// have the right character shape. Pos 0-11 is either genuinely variable or
// vendor-dependent, not a format-wide constant, so it isn't checked here.
// This is individually weaker than a longer fixed-byte signature would be,
// but the AES authentication step *before* this function is ever called (a
// tag has to accept a key derived from its own UID, see
// k2_derive_sector1_key()) is itself already a strong filter against an
// unrelated card matching by coincidence; this only needs to catch
// "authenticated with the right key but decrypted into something that isn't
// really K2 data" from here.
//
// Only checks through PLAUSIBLE_CHECK_LEN (pos 0-33), NOT the full 48 bytes
// — the "reserved" tail (pos 34-47) is NOT reliably all zero on a real tag
// (unlike the DnG-Crafts demo string it's modeled on), so a printable-ASCII
// check over the full 48 bytes would wrongly reject genuine K2 data.
static bool looks_like_k2_data(const uint8_t *data) {
    if (!is_printable_ascii(data, CrealityK2::PLAUSIBLE_CHECK_LEN)) return false;
    if (data[17] != '0') return false;
    return is_hex_digits(data + CrealityK2::COLOR_POS, CrealityK2::COLOR_LEN) &&
           is_ascii_digits(data + CrealityK2::LENGTH_POS, CrealityK2::LENGTH_LEN) &&
           is_ascii_digits(data + CrealityK2::SERIAL_POS, CrealityK2::SERIAL_LEN);
}

static void aes128_ecb(const uint8_t *key16, int mode, const uint8_t *in16, uint8_t *out16) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mode == MBEDTLS_AES_ENCRYPT) mbedtls_aes_setkey_enc(&ctx, key16, 128);
    else mbedtls_aes_setkey_dec(&ctx, key16, 128);
    mbedtls_aes_crypt_ecb(&ctx, mode, in16, out16);
    mbedtls_aes_free(&ctx);
}

// Replicates DnG-Crafts/K2-RFID's own createKey() exactly, including its one
// real quirk: it always cycles indices 0-3 of the UID (`x=0; if(x>=4) x=0;
// key[i]=uid[x]; x++`), never using a byte beyond index 3 even on a 7-byte
// UID. That's kept as-is rather than "fixed" to use more UID bytes — doing
// so would derive a DIFFERENT key than their real hardware/software
// actually produces, defeating the entire point of matching it.
static void k2_derive_sector1_key(const uint8_t *uid, uint8_t *out_key6) {
    uint8_t buf[16];
    for (int i = 0; i < 16; i++) buf[i] = uid[i % 4];
    uint8_t enc[16];
    aes128_ecb(CrealityK2::UID_KEY, MBEDTLS_AES_ENCRYPT, buf, enc);
    memcpy(out_key6, enc, 6);
}

// AES-128-ECB, three independent 16-byte blocks (no chaining — that's what
// ECB means), matching cipherData()'s single Cipher.doFinal() call over the
// full 48 bytes on the Android side.
static void k2_decrypt_sector1(uint8_t *data48) {
    for (int i = 0; i < 48; i += 16) {
        uint8_t out[16];
        aes128_ecb(CrealityK2::DATA_KEY, MBEDTLS_AES_DECRYPT, data48 + i, out);
        memcpy(data48 + i, out, 16);
    }
}

// Inverse of k2_decrypt_sector1() above — used by tag_write_creality() when
// writing fresh data back into an already K2-ENCRYPTED tag (see
// tag_creality.h's own doc comment; real factory spools tend to be this
// variant), so it needs to be re-encrypted with the same data key for
// tag_parse_creality()'s existing K2-encrypted branch to read it back
// correctly on the next scan.
static void k2_encrypt_sector1(uint8_t *data48) {
    for (int i = 0; i < 48; i += 16) {
        uint8_t out[16];
        aes128_ecb(CrealityK2::DATA_KEY, MBEDTLS_AES_ENCRYPT, data48 + i, out);
        memcpy(data48 + i, out, 16);
    }
}

// The source's own GetMaterialLength()/GetMaterialWeight() round-trip
// exactly these five values — a small, fixed set of Creality's own
// "spool size" presets, not a computed length-to-weight conversion (it's
// the same 5 codes regardless of material, so it can't be one). Any other
// code is real data this firmware just doesn't have a mapping for yet —
// reported as weight_grams=0 (unknown) rather than guessed.
static uint16_t k2_length_code_to_grams(const char *code4) {
    if (strncmp(code4, "0330", 4) == 0) return 1000;
    if (strncmp(code4, "0247", 4) == 0) return 750;
    if (strncmp(code4, "0198", 4) == 0) return 600;
    if (strncmp(code4, "0165", 4) == 0) return 500;
    if (strncmp(code4, "0082", 4) == 0) return 250;
    return 0;
}

// Linear scan over CREALITY_K2_MATERIALS (101 entries as of the snapshot
// this was generated from, see creality_k2_materials.h) — small enough that
// a linear search is simpler than sorting it and bisecting, and this only
// runs once per tag scan.
static const CrealityK2Material *find_k2_material(const char *id) {
    for (int i = 0; i < CREALITY_K2_MATERIAL_COUNT; i++) {
        if (strcmp(CREALITY_K2_MATERIALS[i].id, id) == 0) return &CREALITY_K2_MATERIALS[i];
    }
    return nullptr;
}

static void fill_from_k2(FilamentTagData &out, const uint8_t *data) {
    out.format = TagFormat::CREALITY;

    char material_id[CrealityK2::MATERIAL_ID_LEN + 1];
    memcpy(material_id, data + CrealityK2::MATERIAL_ID_POS, CrealityK2::MATERIAL_ID_LEN);
    material_id[CrealityK2::MATERIAL_ID_LEN] = '\0';

    const CrealityK2Material *known = find_k2_material(material_id);
    if (known) {
        // The real, per-material brand from Creality's own material catalog
        // (Generic/Creality/eSUN/Polymaker, confirmed against a real
        // material_database.json — see creality_k2_materials.h) — this is
        // what actually answers "which brand is this filament," not the
        // sector's own 4-digit vendor code below, which turned out to be a
        // separate concept (present on every material regardless of its
        // real-world brand).
        strlcpy(out.brand, known->brand, sizeof(out.brand));
        strlcpy(out.type_name, known->name, sizeof(out.type_name));
    } else {
        // Unknown id — not in the one material_database.json snapshot this
        // firmware has (Creality's cloud catalog grows over time, see
        // creality_k2_materials.h) — fall back to the sector's own vendor
        // code for at least a brand guess, and show the raw id rather than
        // inventing a name.
        char vendor[CrealityK2::VENDOR_LEN + 1];
        memcpy(vendor, data + CrealityK2::VENDOR_POS, CrealityK2::VENDOR_LEN);
        vendor[CrealityK2::VENDOR_LEN] = '\0';
        if (strcmp(vendor, "0276") == 0) {
            strlcpy(out.brand, "Creality", sizeof(out.brand));
        } else {
            // Only "0276" is confirmed as Creality — a different vendor code
            // is real data we can read, but not one we can honestly name.
            snprintf(out.brand, sizeof(out.brand), "Creality (Vendor %s)", vendor);
        }
        snprintf(out.type_name, sizeof(out.type_name), "Material %s", material_id);
    }

    char color_hex[CrealityK2::COLOR_LEN + 1];
    memcpy(color_hex, data + CrealityK2::COLOR_POS, CrealityK2::COLOR_LEN);
    color_hex[CrealityK2::COLOR_LEN] = '\0';
    snprintf(out.color, sizeof(out.color), "#%s", color_hex);

    char length_code[CrealityK2::LENGTH_LEN + 1];
    memcpy(length_code, data + CrealityK2::LENGTH_POS, CrealityK2::LENGTH_LEN);
    length_code[CrealityK2::LENGTH_LEN] = '\0';
    out.weight_grams = k2_length_code_to_grams(length_code);

    // The 6-digit serial here is a real, per-write field (unlike the old
    // layout's "Spool ID...?"), but this is still third-party, unofficial
    // reverse-engineering with no confirmation it's guaranteed unique per
    // *physical* spool rather than, say, a counter that could repeat across
    // tools/tags — same conservative reasoning as the old layout's own
    // Spool ID (see tag_creality.h), so it's not trusted as vendor_serial.
    out.has_serial = false;
    out.vendor_serial[0] = '\0';
}

static void fill_from_old(FilamentTagData &out, const uint8_t *data) {
    out.format = TagFormat::CREALITY;
    strlcpy(out.brand, "Creality", sizeof(out.brand));

    char material_id[CrealityOld::MATERIAL_LEN + 1];
    memcpy(material_id, data + CrealityOld::MATERIAL_POS, CrealityOld::MATERIAL_LEN);
    material_id[CrealityOld::MATERIAL_LEN] = '\0';
    snprintf(out.type_name, sizeof(out.type_name), "Material %s", material_id);

    char color_hex[CrealityOld::COLOR_LEN + 1];
    memcpy(color_hex, data + CrealityOld::COLOR_POS, CrealityOld::COLOR_LEN);
    color_hex[CrealityOld::COLOR_LEN] = '\0';
    snprintf(out.color, sizeof(out.color), "#%s", color_hex);

    // No weight field in this layout, and the "Spool ID" field is
    // deliberately not trusted as a serial — see tag_creality.h.
    out.weight_grams = 0;
    out.has_serial = false;
    out.vendor_serial[0] = '\0';
}

bool tag_parse_creality(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len, FilamentTagData &out) {
    uint8_t data[48];

    // 1) K2/CFS *encrypted* variant: derive this tag's sector-1 key from its
    //    own UID and try authenticating with it first. A tag that isn't
    //    genuinely K2-encrypted will essentially never accept this
    //    effectively-random derived key by coincidence (the same reasoning
    //    Bambu Lab's own derived-key auth already relies on, see
    //    tag_bambu.cpp) — trying it first can't misfire onto an unrelated
    //    plain-keyed card.
    uint8_t derived_key[6];
    k2_derive_sector1_key(uid, derived_key);
    if (auth_and_read_sector1(nfc, uid, uid_len, derived_key, data, "K2-Schlüssel")) {
        hexdump("Creality Sektor 1, K2-Schlüssel, verschlüsselt gelesen", data, 48);
        k2_decrypt_sector1(data);
        hexdump("Creality Sektor 1, K2-Schlüssel, entschlüsselt", data, 48);
        if (looks_like_k2_data(data)) {
            fill_from_k2(out, data);
            return true;
        }
        // Authenticated with the derived key but didn't decrypt into
        // anything plausible — treated the same as "didn't authenticate" and
        // falls through to the default-key attempt below, rather than
        // reporting UNKNOWN outright.
    }

    // 2) Default-key sector — covers both remaining known layouts: the
    //    K2/CFS *plain* (unencrypted) variant, and the older, simpler layout
    //    from CrealityRfid.md. Tried in this order because the K2 magic
    //    bytes are a far more specific signature (see looks_like_k2_data())
    //    than the old layout's loose hex-digit check, so checking for them
    //    first can't accidentally swallow a genuine old-layout tag.
    if (!auth_and_read_sector1(nfc, uid, uid_len, MIFARE_DEFAULT_KEY, data, "Standardschlüssel")) {
        return false;  // neither key works — not a Creality tag (or an unsupported variant)
    }
    hexdump("Creality Sektor 1, Standardschlüssel", data, 48);
    if (looks_like_k2_data(data)) {
        fill_from_k2(out, data);
        return true;
    }
    if (looks_like_old_data(data)) {
        fill_from_old(out, data);
        return true;
    }
    return false;
}

// ── Writing (program mode / test-tag wizard) ────────────────────────────────
// Writes the K2/CFS layout, ALWAYS AES-encrypted and ALWAYS ending with the
// sector locked to the UID-derived key — not the "old" layout, and never a
// "plain, unlocked" shape either. fill_from_k2() looks up the real brand
// from MATERIAL_ID_POS via creality_k2_materials.h (Generic/Creality/
// eSUN/Polymaker, confirmed against real spools, see tag_creality.h), so a
// K2-format tag genuinely reads back with the chosen brand — the old
// layout's own SUPPLIER field has no confirmed real-world meaning (see
// CrealityOld's own comment) and fill_from_old() always reports "Creality"
// regardless of what's written there, so a brand choice would have had no
// visible effect at all.
//
// This mirrors the reference DnG-Crafts/K2-RFID Android app's own
// WriteTag()/CheckTag() exactly (see lock_sector_with_derived_key()'s own
// comment below for the reasoning): every real factory Creality tag is
// already K2-ENCRYPTED, i.e. its sector 1 key was changed away from the
// MIFARE default the first time it was ever written — authenticating with
// the default key can't succeed against one. tag_write_creality() therefore
// always AES-encrypts (k2_encrypt_sector1(), the exact inverse of
// k2_decrypt_sector1()), authenticates with the default key first and
// relocks to the derived key on success (lock_sector_with_derived_key()) for
// a genuinely blank card, or falls back to authenticating with the derived
// key directly (no relock needed — already correct) when the default key
// doesn't work at all, meaning the tag was already K2-ENCRYPTED going in.

static bool write_block_with_key(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len,
                                  const uint8_t *key6, uint8_t block, const uint8_t *data16) {
    if (!pn532_authenticate_retry(nfc, uid, uid_len, block, key6)) return false;
    return pn532_write_block_retry(nfc, block, data16);
}

static bool write_block_default_key(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len,
                                     uint8_t block, const uint8_t *data16) {
    return write_block_with_key(nfc, uid, uid_len, MIFARE_DEFAULT_KEY, block, data16);
}

// Mirrors DnG-Crafts/K2-RFID's own Android app (MainActivity.java's
// WriteTag()/CheckTag()): that app's "encrypted" flag does NOT mean "the
// data on the tag is AES-encrypted" — it ALWAYS AES-encrypts sector 1's data
// via cipherData(), unconditionally, on every single write. "encrypted"
// there only tracks whether the SECTOR KEY has already been changed away
// from the MIFARE default (i.e. whether this tag has been written before).
// On a genuinely virgin card, the app authenticates with the plain default
// key, writes the AES-encrypted data with it, and THEN immediately relocks
// the trailer to the UID-derived key — read-modify-write, preserving
// whatever access-bits/GPB (trailer bytes 6-9) the card already has rather
// than assuming a fixed value, exactly mirrored here. A real tag from this
// ecosystem is therefore never left on the default key after its first
// write, and never carries plain (unencrypted) data.
static bool lock_sector_with_derived_key(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len,
                                          uint8_t trailer_block, const uint8_t *derived_key6) {
    if (!pn532_authenticate_retry(nfc, uid, uid_len, trailer_block, MIFARE_DEFAULT_KEY)) return false;
    uint8_t trailer[16];
    if (!pn532_read_block_retry(nfc, trailer_block, trailer)) return false;
    memcpy(trailer, derived_key6, 6);
    memcpy(trailer + 10, derived_key6, 6);
    return pn532_write_block_retry(nfc, trailer_block, trailer);
}

// Case-insensitive substring search — used below to match the wizard's own
// generic base-material/modifier naming (e.g. "PLA", "Hyper") against a real
// material_database.json entry's product name (e.g. "Hyper PLA"), which
// doesn't necessarily share the wizard's word order or exact vocabulary.
static bool contains_ci(const char *haystack, const char *needle) {
    size_t hn = strlen(haystack), nn = strlen(needle);
    if (nn == 0) return true;
    if (nn > hn) return false;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        while (j < nn && tolower((unsigned char)haystack[i + j]) == tolower((unsigned char)needle[j])) j++;
        if (j == nn) return true;
    }
    return false;
}

// Picks a real creality_k2_materials.h entry for the requested brand — the
// K2 layout's material_id is the only place a written tag's brand actually
// lives (fill_from_k2() looks it up the identical way), so writing a tag
// that reads back with the chosen brand means picking a real id whose own
// .brand matches, never inventing one. type_name is the wizard's combined
// "<base material> <modifier>" string (see build_prog_write_tag()) — split
// back into its two parts here (mirroring nfc_reader.cpp's
// expected_opentag3d_type_name(), the same base+modifier split already used
// there) so each can be matched against a real product name's own wording
// independently. Preference order: brand + base + modifier all matching,
// else brand + base, else brand alone — always finds something, since every
// brand this wizard offers has at least one real entry in the table (see
// creality_k2_materials.h's own brand counts).
static const CrealityK2Material *find_k2_material_for_wizard(const char *brand, const char *type_name) {
    char base[24] = "", modifier[24] = "";
    const char *space = strchr(type_name, ' ');
    if (space) {
        size_t base_len = (size_t)(space - type_name);
        if (base_len >= sizeof(base)) base_len = sizeof(base) - 1;
        memcpy(base, type_name, base_len);
        base[base_len] = '\0';
        strlcpy(modifier, space + 1, sizeof(modifier));
    } else {
        strlcpy(base, type_name, sizeof(base));
    }

    const CrealityK2Material *brand_only = nullptr;
    const CrealityK2Material *base_match = nullptr;
    for (int i = 0; i < CREALITY_K2_MATERIAL_COUNT; i++) {
        const CrealityK2Material &m = CREALITY_K2_MATERIALS[i];
        if (strcmp(m.brand, brand) != 0) continue;
        if (!brand_only) brand_only = &m;
        if (!contains_ci(m.name, base)) continue;
        if (!base_match) base_match = &m;
        if (modifier[0] != '\0' && contains_ci(m.name, modifier)) return &m;
    }
    return base_match ? base_match : brand_only;
}

// Inverse of k2_length_code_to_grams() above — the wizard's weight presets
// (250/500/750/1000g) cover 4 of the format's 5 known codes; 600g has no
// preset button and is unreachable from the wizard, so it needs no case
// here.
static const char *k2_grams_to_length_code(uint16_t grams) {
    switch (grams) {
        case 750:  return "0247";
        case 500:  return "0165";
        case 250:  return "0082";
        default:   return "0330";  // 1000g, and the fallback for anything unexpected
    }
}

bool tag_write_creality(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len, const FilamentTagData &in,
                         char *err, size_t err_cap) {
    const char *brand = in.brand[0] ? in.brand : "Generic";
    const CrealityK2Material *material = find_k2_material_for_wizard(brand, in.type_name);
    if (!material) {
        strlcpy(err, STR_ERROR_GENERIC, err_cap);
        return false;
    }

    uint8_t data[48];
    // Printable filler for every byte not explicitly set below — positions
    // 0-16 and 34-47 have no confirmed meaning (see tag_creality.h), and
    // looks_like_k2_data() only requires them to be printable ASCII.
    memset(data, '0', sizeof(data));

    memcpy(data + CrealityK2::MATERIAL_ID_POS, material->id, CrealityK2::MATERIAL_ID_LEN);
    data[17] = '0';  // the one marker confirmed fixed across every real sample

    const char *color_hex = (in.color[0] == '#') ? in.color + 1 : in.color;
    memcpy(data + CrealityK2::COLOR_POS, color_hex, CrealityK2::COLOR_LEN);

    memcpy(data + CrealityK2::LENGTH_POS, k2_grams_to_length_code(in.weight_grams), CrealityK2::LENGTH_LEN);

    char serial[CrealityK2::SERIAL_LEN + 1];
    gen_decimal_serial(serial, sizeof(serial), CrealityK2::SERIAL_LEN);
    memcpy(data + CrealityK2::SERIAL_POS, serial, CrealityK2::SERIAL_LEN);

    // Always AES-encrypt before writing sector 1 — see
    // lock_sector_with_derived_key()'s own comment: the reference app never
    // writes plain data there, regardless of the card's current key state.
    uint8_t encrypted[48];
    memcpy(encrypted, data, sizeof(encrypted));
    k2_encrypt_sector1(encrypted);

    static const uint8_t TRAILER_BLOCK = BLOCK_FIRST + 3;  // block 7

    // Try the default key first (a genuinely blank card, or one already in
    // the plain K2 layout from before this fix existed) — only on block 4,
    // the first of the three; if that authenticates, the rest of the sector
    // uses the same key by construction (all three blocks are one MIFARE
    // Classic sector), so the remaining two are written the same way
    // without re-probing.
    if (write_block_default_key(nfc, uid, uid_len, BLOCK_FIRST, encrypted)) {
        for (uint8_t i = 1; i < 3; i++) {
            if (!write_block_default_key(nfc, uid, uid_len, BLOCK_FIRST + i, encrypted + (i * 16))) {
                snprintf(err, err_cap, STR_ERR_BLOCK_NOT_WRITABLE_FMT, (unsigned)(BLOCK_FIRST + i));
                return false;
            }
        }
        // Relock to the UID-derived key, same as the reference app's own
        // WriteTag() does on a virgin card's first write — see
        // lock_sector_with_derived_key()'s own comment for why.
        uint8_t derived_key[6];
        k2_derive_sector1_key(uid, derived_key);
        if (!lock_sector_with_derived_key(nfc, uid, uid_len, TRAILER_BLOCK, derived_key)) {
            snprintf(err, err_cap, STR_ERR_SECTOR_LOCK_FAILED_FMT, (unsigned)1);
            return false;
        }
        return true;
    }

    // Default key didn't authenticate — already K2-ENCRYPTED (locked to the
    // derived key from an earlier write, e.g. real factory data or a
    // previous Überschreiben). Reselecting here before trying the derived
    // key is required — the same real-hardware-confirmed pattern relied on
    // elsewhere in this project (see nfc_reader.cpp's own reselect between
    // Creality's and Bambu's attempts, and between the "already written?"
    // check and the write dispatch): after a FAILED auth attempt, the
    // PN532's own RF-side target tracking can need a fresh reselect before
    // the NEXT attempt — even with a different, genuinely correct key —
    // succeeds. Into a scratch buffer, not `uid` itself, for the same reason
    // as every other reselect in this codebase: it exists purely for its
    // hardware side effect, uid/uid_len are already trusted from this
    // detection cycle.
    uint8_t reselect_uid[7];
    uint8_t reselect_len = uid_len;
    nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, reselect_uid, &reselect_len, 200);

    // No relock needed here — the sector is already on the correct per-UID
    // derived key.
    uint8_t derived_key[6];
    k2_derive_sector1_key(uid, derived_key);
    for (uint8_t i = 0; i < 3; i++) {
        if (!write_block_with_key(nfc, uid, uid_len, derived_key, BLOCK_FIRST + i, encrypted + (i * 16))) {
            snprintf(err, err_cap, STR_ERR_BLOCK_NOT_WRITABLE_FMT, (unsigned)(BLOCK_FIRST + i));
            return false;
        }
    }
    return true;
}
