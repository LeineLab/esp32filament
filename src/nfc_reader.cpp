#include "nfc_reader.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_PN532.h>
#include "tag_bambu.h"
#include "tag_creality.h"
#include "tag_openspool.h"
#include "tag_opentag3d.h"
#include "strings.h"
#include "hexdump.h"
#include "pn532_auth.h"

// IRQ and RESET not used (-1) — matches esp32rental's own I2C PN532 setup.
static Adafruit_PN532 nfc_dev(-1, -1);

static QueueHandle_t nfc_queue = nullptr;

// Program mode (test-tag writing) — see nfc_reader.h's own doc comment.
// write_request_queue holds an ArmedWriteRequest (below), not a bare
// FilamentTagData — the queue item type grew an `allow_overwrite` flag
// alongside `desired` once nfc_arm_write() gained that parameter, rather
// than adding a second parallel single-item queue for just one bool.
struct ArmedWriteRequest {
    FilamentTagData desired;
    bool allow_overwrite = false;
};
static QueueHandle_t write_request_queue = nullptr;
static QueueHandle_t write_result_queue  = nullptr;

// i2c_bus_recover() and the retry-wrapped pn532_authenticate_retry()/
// pn532_read_block_retry()/pn532_write_block_retry() live in
// pn532_auth.h/.cpp — see its own doc comment for why recovering the bus
// only *between* commands isn't enough on its own (a glitch that hits the
// one auth attempt itself needs that specific attempt retried, not just the
// next one protected). tag_bambu.cpp/tag_creality.cpp call the retry
// wrappers directly instead of nfc.mifareclassic_*() — this file still uses
// i2c_bus_recover() directly at its own natural attempt boundaries (after
// every readPassiveTargetID(), before the write path's reselect).

// Adafruit_PN532's own InListPassiveTarget response parsing
// (readDetectedPassiveTargetID() in the library) already extracts the raw
// ISO14443-3 ATQA (bytes 9-10) and SAK (byte 11) out of its scratch buffer
// internally — the same "what kind of tag is this" information a real
// reader (Proxmark3, an NFC-reading phone app, ...) reports before ever
// trying a key — but the library only ever prints them under its own
// MIFAREDEBUG build flag, never returns them through its public API. Rather
// than forking the library or re-implementing the raw command exchange
// (its own `readdata()` that fills this buffer is private, only
// `sendCommandCheckAck()` is public), this reads the exact same buffer the
// library already fills — a plain global with external linkage in
// Adafruit_PN532.cpp (`byte pn532_packetbuffer[64]`, no `static`), not a
// class member, so it's legitimately reachable via `extern`. Useful for
// telling whether two tags that fail every attempted key are even the same
// *kind* of chip, without needing any key to find out. Must be read
// immediately after a successful readPassiveTargetID() and before any
// other PN532 command runs — the very next command overwrites this buffer.
extern uint8_t pn532_packetbuffer[];

static const char *sak_description(uint8_t sak) {
    switch (sak) {
        case 0x00: return "MIFARE Ultralight/NTAG (kein Classic-Schlüssel möglich)";
        case 0x08: return "MIFARE Classic 1K";
        case 0x09: return "MIFARE Mini";
        case 0x18: return "MIFARE Classic 4K";
        // ISO14443-4-compliant chips (DESFire, MIFARE Plus in SL3,
        // SmartMX/JCOP, ...) negotiate a completely different, AES/3DES-
        // based security framework — none of this project's crypto1-based
        // MIFARE Classic keys (default, HKDF/AES-derived, NDEF well-known)
        // could ever authenticate against one, regardless of which key is
        // tried, which would explain exactly the symptom reported here.
        case 0x20: return "ISO14443-4 (z.B. DESFire/MIFARE Plus SL3/SmartMX) - kein MIFARE-Classic-Schlüssel möglich";
        case 0x28: return "MIFARE Classic 1K (emuliert, z.B. SmartMX/JCOP)";
        case 0x38: return "MIFARE Classic 4K (emuliert)";
        default:   return "unbekannt";
    }
}

// Prints the tag-type diagnosis for whatever's in pn532_packetbuffer right
// now — call immediately after a successful readPassiveTargetID(), before
// any other PN532 command. See this file's own comment on pn532_packetbuffer
// above for why this reads a library-internal buffer via extern.
static void report_tag_type() {
    uint16_t atqa = (static_cast<uint16_t>(pn532_packetbuffer[9]) << 8) | pn532_packetbuffer[10];
    uint8_t sak = pn532_packetbuffer[11];
    Serial.printf("  Tag-Typ: ATQA=0x%04X SAK=0x%02X (%s)\n", atqa, sak, sak_description(sak));
}

// Attempts to identify `uid`/`uid_len` as any of the four known formats —
// shared between the normal read path (nfc_task's main dispatch below) and
// the write path's "refuse to overwrite an already-written tag" safety check.
static bool try_parse_any_format(const uint8_t *uid, uint8_t uid_len, FilamentTagData &out) {
    if (uid_len == 7) {
        return tag_parse_openspool(nfc_dev, out) || tag_parse_opentag3d(nfc_dev, out);
    }
    return tag_parse_creality(nfc_dev, uid, uid_len, out) || tag_parse_bambu(nfc_dev, uid, uid_len, out);
}

// OpenTag3D genuinely stores base material and modifier as two separate
// 5-byte fields (see tag_write_opentag3d()'s own doc comment) — a modifier
// name longer than 5 characters (e.g. "Luminous", "Stardust", both offered
// by the wizard, see ui.cpp's PROG_MODIFIERS) is silently truncated when
// written, so the read-back's reconstructed "Base Modifier" string
// genuinely, correctly differs from what was requested. This reconstructs
// what the read-back SHOULD say after that real, documented truncation, so
// the verification step doesn't misreport a truncation the format itself
// causes as a write failure.
static void expected_opentag3d_type_name(const char *desired_type, char *out, size_t out_cap) {
    const char *space = strchr(desired_type, ' ');
    char base[8] = {0}, modifier[8] = {0};
    if (space) {
        size_t base_len = (size_t)(space - desired_type);
        if (base_len > 5) base_len = 5;
        memcpy(base, desired_type, base_len);
        strlcpy(modifier, space + 1, sizeof(modifier));
        if (strlen(modifier) > 5) modifier[5] = '\0';
    } else {
        strlcpy(base, desired_type, sizeof(base));
        if (strlen(base) > 5) base[5] = '\0';
    }
    if (modifier[0] != '\0') {
        snprintf(out, out_cap, "%s %s", base, modifier);
    } else {
        strlcpy(out, base, out_cap);
    }
}

// Diagnostic-only probe for a write attempt where NEITHER the MIFARE
// factory default key NOR any of this firmware's own derived keys
// authenticates against the card — including the plain default-key attempt
// on Creality's sector, which should always succeed against a genuinely
// blank/never-rekeyed card. One common, well-documented reason a MIFARE
// Classic 1K card sold as a generic "NFC tag" rejects the plain default key
// specifically: it may have shipped pre-formatted for NDEF storage, which
// conventionally uses two different NFC-Forum-published well-known keys
// instead of the factory default — the MAD key (0xA0A1A2A3A4A5) on sector 0
// (where Bambu's own block 2 lives), and the NDEF data key
// (0xD3F7D3F7D3F7) on every other data sector (where Creality's block 4
// lives). Purely informational — doesn't change write behavior — logged
// only when the "already written?" check above found nothing recognizable,
// so this specific, common cause can be confirmed or ruled out before
// deciding whether to build support for writing to an NDEF-preformatted card.
static bool probe_key(const uint8_t *uid, uint8_t uid_len, uint8_t block, const uint8_t *key6) {
    return pn532_authenticate_retry(nfc_dev, uid, uid_len, block, key6);
}

static void probe_ndef_preformatted_keys(const uint8_t *uid, uint8_t uid_len) {
    static const uint8_t NDEF_MAD_KEY[6]  = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    static const uint8_t NDEF_DATA_KEY[6] = {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7};
    bool mad_ok  = probe_key(uid, uid_len, /*block=*/2, NDEF_MAD_KEY);
    bool data_ok = probe_key(uid, uid_len, /*block=*/4, NDEF_DATA_KEY);
    Serial.printf("  Diagnose: NDEF-MAD-Schlüssel auf Block 2: %s | NDEF-Daten-Schlüssel auf Block 4: %s\n",
                  mad_ok ? "authentifiziert (!)" : "fehlgeschlagen",
                  data_ok ? "authentifiziert (!)" : "fehlgeschlagen");
}

// Handles one armed write request against the tag just detected this
// iteration (`uid`/`uid_len`) — called from inside nfc_task, never directly.
// Refuses (and reports `already_written`) if the tag already parses as any
// known format, so this can never silently clobber a real spool's tag, or
// re-run against a tag this same wizard already wrote — UNLESS
// `allow_overwrite` is set (ui.cpp's own explicit "Überschreiben"
// confirmation, see nfc_arm_write()'s doc comment in nfc_reader.h), in which
// case that whole check (and its own diagnostic NDEF-key probe, equally
// pointless once the operator already knows the tag isn't blank) is skipped
// entirely. On a successful write, immediately reads the result back
// through the exact same parser real scans use and compares it against what
// was requested — proof the write is actually correct, not just that the
// write call returned success.
static void handle_write_request(const FilamentTagData &desired, const uint8_t *uid, uint8_t uid_len,
                                  bool allow_overwrite) {
    ProgramWriteResult result;

    // Prints the UID the write path is actually using — unlike the normal
    // scan path (see the "=== NFC-Tag erkannt ===" header below in this
    // file's own dispatch). Useful for telling apart "several physical tags
    // produce identical derived keys because the same UID is being fed into
    // the derivation every time" from "the tags themselves happen to share a
    // UID" (e.g. non-unique "magic"/clone cards from the same batch).
    char uid_hex[22];
    hex_to_str(uid, uid_len, uid_hex, sizeof(uid_hex));
    Serial.printf("\n=== Programmieren: Tag erkannt UID=%s (%u Byte)%s ===\n", uid_hex, (unsigned)uid_len,
                  allow_overwrite ? " (Ueberschreiben erzwungen)" : "");

    if (!allow_overwrite) {
        FilamentTagData existing;
        if (try_parse_any_format(uid, uid_len, existing)) {
            // Pairs with ui.cpp's own diagnostic for this same screen — if
            // this line prints but the UI's doesn't, the failure is on the
            // UI side, not detection; useful for isolating a report of the
            // "Tag ist nicht leer" screen not appearing for a tag that does
            // decode correctly in the Serial hexdump.
            Serial.printf("  -> Tag bereits beschrieben (Format: %s) - Ueberschreiben-Dialog wird angezeigt\n",
                          tag_format_name(existing.format));
            result.already_written  = true;
            result.existing_format  = existing.format;
            xQueueOverwrite(write_result_queue, &result);
            return;
        }
    }

    // Between distinct attempts — see i2c_bus_recover()'s own comment.
    i2c_bus_recover();

    // See probe_ndef_preformatted_keys()'s own comment — diagnostic only,
    // for 7-byte-UID (MIFARE Classic) targets only, and only when the
    // already-written check above actually ran (see this function's own
    // comment on allow_overwrite).
    if (!allow_overwrite && uid_len != 7) probe_ndef_preformatted_keys(uid, uid_len);

    // Recover the I2C bus (see i2c_bus_recover()) AND re-select the tag
    // before writing. Both matter here, for two different reasons: the bus
    // recovery addresses the I2C-NG driver bug documented above; the
    // re-select addresses a separate, real-hardware-confirmed issue — after
    // several failed MIFARE auth attempts against different sectors/keys,
    // this PN532 module's own RF-side target tracking can need a fresh
    // select before the NEXT auth attempt succeeds (see the scan-path's own
    // identical call above). Into a scratch buffer, not `uid` itself — this
    // call exists purely for its hardware side effect, we already trust
    // uid/uid_len from this same detection cycle.
    uint8_t reselect_uid[7];
    uint8_t reselect_len = uid_len;
    nfc_dev.readPassiveTargetID(PN532_MIFARE_ISO14443A, reselect_uid, &reselect_len, 200);

    char err[100] = "";  // matches ProgramWriteResult::error's size — see nfc_reader.h
    bool ok = false;
    switch (desired.format) {
        case TagFormat::BAMBU:     ok = tag_write_bambu(nfc_dev, uid, uid_len, desired, err, sizeof(err)); break;
        case TagFormat::CREALITY:  ok = tag_write_creality(nfc_dev, uid, uid_len, desired, err, sizeof(err)); break;
        case TagFormat::OPENSPOOL: ok = tag_write_openspool(nfc_dev, desired, err, sizeof(err)); break;
        case TagFormat::OPENTAG3D: ok = tag_write_opentag3d(nfc_dev, desired, err, sizeof(err)); break;
        default: strlcpy(err, STR_ERR_UNKNOWN_TARGET_FORMAT, sizeof(err)); break;
    }

    if (ok) {
        FilamentTagData verify;
        bool parsed = false;
        switch (desired.format) {
            case TagFormat::BAMBU:     parsed = tag_parse_bambu(nfc_dev, uid, uid_len, verify); break;
            case TagFormat::CREALITY:  parsed = tag_parse_creality(nfc_dev, uid, uid_len, verify); break;
            case TagFormat::OPENSPOOL: parsed = tag_parse_openspool(nfc_dev, verify); break;
            case TagFormat::OPENTAG3D: parsed = tag_parse_opentag3d(nfc_dev, verify); break;
            default: break;
        }
        bool matches = parsed &&
            strcmp(verify.color, desired.color) == 0 &&
            (desired.weight_grams == 0 || verify.weight_grams == desired.weight_grams) &&
            (!desired.has_serial || strcmp(verify.vendor_serial, desired.vendor_serial) == 0) &&
            // OpenSpool/OpenTag3D have a real, writable brand field; Creality
            // has one too via its K2 layout's material_id lookup (see
            // tag_write_creality()) — desired.brand is empty only for Bambu
            // (see prog_format_has_brand_field() in ui.cpp — comparing it
            // there would wrongly fail against Bambu's always-hardcoded
            // "Bambu Lab" brand).
            (desired.brand[0] == '\0' || strcmp(verify.brand, desired.brand) == 0);
        // Creality's material lookup (find_k2_material_for_wizard() in
        // tag_creality.cpp) picks a REAL product name from
        // creality_k2_materials.h for the requested brand/base material —
        // it's a genuine human-readable name, but not necessarily worded
        // exactly like the wizard's own generic "<base material> <modifier>"
        // string (e.g. requesting brand=Creality/base=PLA/modifier=Hyper
        // could pick the real entry "Hyper PLA" — same filament, different
        // word order) — so type_name is still not a meaningful field to
        // compare for this format, just for a different reason than before
        // this format switched from the old layout to K2 (see
        // tag_write_creality()'s own comment for the full history).
        if (matches && desired.format == TagFormat::OPENTAG3D) {
            char expected[24];
            expected_opentag3d_type_name(desired.type_name, expected, sizeof(expected));
            matches = strcmp(verify.type_name, expected) == 0;
        } else if (matches && desired.format != TagFormat::CREALITY) {
            matches = strcmp(verify.type_name, desired.type_name) == 0;
        }
        if (!matches) {
            ok = false;
            strlcpy(err, STR_ERR_VERIFY_MISMATCH, sizeof(err));
        }
    }

    result.ok = ok;
    strlcpy(result.error, err, sizeof(result.error));
    xQueueOverwrite(write_result_queue, &result);
}

static void nfc_task(void *) {
    bool tag_present = false;
    uint8_t last_uid[7] = {0};
    uint8_t last_uid_len = 0;

    while (true) {
        uint8_t uid[7] = {0};
        uint8_t uid_len = 0;

        bool found = nfc_dev.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uid_len, 200);

        if (!found) {
            tag_present = false;
            vTaskDelay(pdMS_TO_TICKS(NFC_POLL_INTERVAL_MS));
            continue;
        }

        bool same_as_last = tag_present && uid_len == last_uid_len && memcmp(uid, last_uid, uid_len) == 0;
        if (same_as_last) {
            // Same physical tag still sitting on the reader — already processed it.
            vTaskDelay(pdMS_TO_TICKS(NFC_POLL_INTERVAL_MS));
            continue;
        }

        tag_present = true;
        memcpy(last_uid, uid, uid_len);
        last_uid_len = uid_len;

        // Must run before anything else touches the PN532 (any further
        // command overwrites the scratch buffer this reads) — see
        // report_tag_type()'s own comment.
        report_tag_type();

        // See i2c_bus_recover()'s own comment above for why this runs here,
        // before any further PN532 command in either the write or normal
        // scan path below.
        i2c_bus_recover();

        // Program mode: a write request armed by ui.cpp takes over this
        // detection entirely instead of the normal read/identify dispatch
        // below — single-shot, xQueueReceive already consumes it here.
        ArmedWriteRequest armed;
        if (write_request_queue && xQueueReceive(write_request_queue, &armed, 0) == pdTRUE) {
            handle_write_request(armed.desired, uid, uid_len, armed.allow_overwrite);
            vTaskDelay(pdMS_TO_TICKS(NFC_POLL_INTERVAL_MS));
            continue;
        }

        FilamentTagData data;
        memcpy(data.uid, uid, uid_len);
        data.uid_len = uid_len;

        // Serial diagnostics for real-hardware reference-gathering (see
        // hexdump.h and README.md's "Serial-Diagnose" section) — each
        // tag_parse_*() call below prints its own raw-bytes dump(s) as it
        // goes; this just brackets them with a UID header and, once dispatch
        // is done, the final decoded result (or UNKNOWN).
        char uid_hex[22];  // 7 bytes -> "XX XX XX XX XX XX XX" = 20 chars + NUL
        hex_to_str(uid, uid_len, uid_hex, sizeof(uid_hex));
        Serial.printf("\n=== NFC-Tag erkannt: UID=%s (%u Byte) ===\n", uid_hex, (unsigned)uid_len);

        if (uid_len == 7) {
            // NTAG21x candidate (OpenSpool / OpenTag3D — both use 7-byte UIDs)
            if (!tag_parse_openspool(nfc_dev, data)) {
                tag_parse_opentag3d(nfc_dev, data);  // leaves data.format == UNKNOWN on failure too
            }
        } else {
            // MIFARE Classic 1K candidate (Creality / Bambu Lab)
            if (!tag_parse_creality(nfc_dev, uid, uid_len, data)) {
                // After one or more failed MIFARE Classic auth attempts
                // against a target (here, Creality's own key attempts), some
                // PN532 (clone) modules need a fresh anticollision/select
                // before a NEW attempt with a different key succeeds — see
                // e.g. esphome/esphome#19806 for the same symptom reported
                // elsewhere. Forcing a fresh readPassiveTargetID() here is
                // cheap and can't make things worse — if the tag hasn't
                // moved this just re-confirms it, if it has this only costs
                // one failed Bambu attempt exactly like before. Return value
                // intentionally unchecked: if reselect itself fails,
                // tag_parse_bambu() below will simply fail too, no worse off.
                nfc_dev.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uid_len, 200);
                tag_parse_bambu(nfc_dev, uid, uid_len, data);
            }
        }

        if (data.format == TagFormat::UNKNOWN) {
            Serial.println("  -> Kein bekanntes Format erkannt (UNKNOWN)");
        } else {
            Serial.printf("  -> Format: %s | Marke: %s | Typ: %s | Farbe: %s | Gewicht: %ug | Seriennummer: %s\n",
                           tag_format_name(data.format), data.brand, data.type_name, data.color,
                           (unsigned)data.weight_grams, data.has_serial ? data.vendor_serial : "(keine)");
        }

        xQueueOverwrite(nfc_queue, &data);
        vTaskDelay(pdMS_TO_TICKS(NFC_POLL_INTERVAL_MS));
    }
}

bool nfc_init() {
    Wire.begin(PN532_SDA, PN532_SCL);

    nfc_dev.begin();

    uint32_t versiondata = nfc_dev.getFirmwareVersion();
    if (!versiondata) {
        Serial.println("NFC: PN532 not found");
        return false;
    }
    Serial.printf("NFC: PN532 fw v%d.%d\n", (versiondata >> 16) & 0xFF, (versiondata >> 8) & 0xFF);

    nfc_dev.SAMConfig();

    nfc_queue = xQueueCreate(1, sizeof(FilamentTagData));
    // Must be sized for the full ArmedWriteRequest, not just FilamentTagData
    // — FreeRTOS queues copy EXACTLY the byte count given here, regardless
    // of the actual struct size passed to xQueueOverwrite()/xQueueReceive()
    // at the call sites. Getting this wrong silently truncates
    // allow_overwrite away on every write (it sits right after `desired` in
    // memory) with no error at either end — the receiving side's default-
    // constructed `false` simply never gets overwritten.
    write_request_queue = xQueueCreate(1, sizeof(ArmedWriteRequest));
    write_result_queue  = xQueueCreate(1, sizeof(ProgramWriteResult));
    if (!nfc_queue || !write_request_queue || !write_result_queue) return false;

    // Larger stack than esp32rental's simple UID-only reader: MIFARE Classic
    // block reads + the Bambu HKDF-SHA256 derivation (mbedtls) need more room.
    xTaskCreatePinnedToCore(nfc_task, "nfc", 8192, nullptr, 1, nullptr, 0);
    return true;
}

bool nfc_available() {
    if (!nfc_queue) return false;
    FilamentTagData tmp;
    return xQueuePeek(nfc_queue, &tmp, 0) == pdTRUE;
}

bool nfc_read(FilamentTagData &out) {
    if (!nfc_queue) return false;
    return xQueueReceive(nfc_queue, &out, 0) == pdTRUE;
}

void nfc_arm_write(const FilamentTagData &desired, bool allow_overwrite) {
    if (!write_request_queue) return;
    ArmedWriteRequest req{desired, allow_overwrite};
    xQueueOverwrite(write_request_queue, &req);
}

void nfc_disarm_write() {
    if (!write_request_queue) return;
    xQueueReset(write_request_queue);
}

bool nfc_write_result_available() {
    if (!write_result_queue) return false;
    ProgramWriteResult tmp;
    return xQueuePeek(write_result_queue, &tmp, 0) == pdTRUE;
}

bool nfc_get_write_result(ProgramWriteResult &out) {
    if (!write_result_queue) return false;
    return xQueueReceive(write_result_queue, &out, 0) == pdTRUE;
}
