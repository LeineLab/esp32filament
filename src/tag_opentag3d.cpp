#include "tag_opentag3d.h"
#include "ndef.h"
#include "strings.h"
#include "hexdump.h"

// Byte offsets from https://opentag3d.info/spec.
namespace OT3D {
    static const size_t BASE_MATERIAL   = 0x02, BASE_MATERIAL_LEN   = 5;
    static const size_t MODIFIERS       = 0x07, MODIFIERS_LEN       = 5;
    static const size_t MANUFACTURER    = 0x0C, MANUFACTURER_LEN    = 16;
    static const size_t COLOR_NAME      = 0x1C, COLOR_NAME_LEN      = 32;
    static const size_t COLOR_RGBA      = 0x3C, COLOR_RGBA_LEN      = 4;
    static const size_t SERIAL_BATCH_ID = 0x4C, SERIAL_BATCH_ID_LEN = 32;
    static const size_t TARGET_WEIGHT   = 0x9E;  // uint16 BE, grams
}

// Copies a UTF-8 field out of `payload`, bounds-checked, trimmed of trailing
// NUL/whitespace padding. Returns an empty string if out of range.
static void read_str_field(const uint8_t *payload, size_t payload_len, size_t start, size_t len,
                            char *out, size_t out_cap) {
    out[0] = '\0';
    if (start >= payload_len) return;
    size_t avail = payload_len - start;
    size_t copy_len = len < avail ? len : avail;
    if (copy_len > out_cap - 1) copy_len = out_cap - 1;
    memcpy(out, payload + start, copy_len);
    out[copy_len] = '\0';
    // Trim trailing NULs/spaces that pad a shorter value out to the field width.
    for (int i = (int)copy_len - 1; i >= 0 && (out[i] == '\0' || out[i] == ' '); i--) {
        out[i] = '\0';
    }
}

static uint16_t read_u16be(const uint8_t *payload, size_t payload_len, size_t start) {
    if (start + 2 > payload_len) return 0;
    return (static_cast<uint16_t>(payload[start]) << 8) | payload[start + 1];
}

// ── Writing (program mode / test-tag wizard) ────────────────────────────────

bool tag_write_opentag3d(Adafruit_PN532 &nfc, const FilamentTagData &in, char *err, size_t err_cap) {
    // Zero-filled, including the offsets this project's own parser never
    // reads at all (bytes before BASE_MATERIAL, the gaps between fields) and
    // Color Name (left blank so tag_parse_opentag3d()'s own hex-fallback
    // path kicks in, exactly as it would for a real tag that never set a
    // human-readable name) — we don't know what the unread offsets mean, so
    // we don't invent content for them. Modifiers/Manufacturer, by contrast,
    // ARE written below (from the wizard's own modifier/brand picks).
    uint8_t payload[OT3D::TARGET_WEIGHT + 2] = {0};

    // Split "Base" or "Base Modifier" (see this function's doc comment in
    // tag_opentag3d.h) on the first space into this format's own separate
    // fields — both silently clamped to their real field width.
    const char *space = strchr(in.type_name, ' ');
    size_t base_len = space ? (size_t)(space - in.type_name) : strlen(in.type_name);
    if (base_len > OT3D::BASE_MATERIAL_LEN) base_len = OT3D::BASE_MATERIAL_LEN;
    memcpy(payload + OT3D::BASE_MATERIAL, in.type_name, base_len);
    if (space) {
        size_t mod_len = strlen(space + 1);
        if (mod_len > OT3D::MODIFIERS_LEN) mod_len = OT3D::MODIFIERS_LEN;
        memcpy(payload + OT3D::MODIFIERS, space + 1, mod_len);
    }

    size_t brand_len = strlen(in.brand);
    if (brand_len > OT3D::MANUFACTURER_LEN) brand_len = OT3D::MANUFACTURER_LEN;
    memcpy(payload + OT3D::MANUFACTURER, in.brand, brand_len);

    unsigned r = 0, g = 0, b = 0;
    if (in.color[0] == '#') sscanf(in.color + 1, "%2x%2x%2x", &r, &g, &b);
    payload[OT3D::COLOR_RGBA + 0] = (uint8_t)r;
    payload[OT3D::COLOR_RGBA + 1] = (uint8_t)g;
    payload[OT3D::COLOR_RGBA + 2] = (uint8_t)b;
    payload[OT3D::COLOR_RGBA + 3] = 0xFF;

    size_t ser_len = strlen(in.vendor_serial);
    if (ser_len > OT3D::SERIAL_BATCH_ID_LEN) ser_len = OT3D::SERIAL_BATCH_ID_LEN;
    memcpy(payload + OT3D::SERIAL_BATCH_ID, in.vendor_serial, ser_len);

    payload[OT3D::TARGET_WEIGHT]     = (uint8_t)(in.weight_grams >> 8);
    payload[OT3D::TARGET_WEIGHT + 1] = (uint8_t)(in.weight_grams & 0xFF);

    if (!ndef_write_message(nfc, "application/opentag3d", payload, sizeof(payload))) {
        strlcpy(err, STR_ERR_NDEF_WRITE_FAILED_OPENTAG3D, err_cap);
        return false;
    }
    return true;
}

bool tag_parse_opentag3d(Adafruit_PN532 &nfc, FilamentTagData &out) {
    uint8_t buf[320];
    NdefRecord rec;
    if (!ndef_read_first_record(nfc, buf, sizeof(buf), rec)) return false;
    Serial.printf("  NDEF-Record gefunden, MIME-Typ: \"%s\"\n", rec.type);
    hexdump("NDEF-Payload (OpenTag3D-Versuch)", rec.payload, rec.payload_len);
    if (strcmp(rec.type, "application/opentag3d") != 0) return false;

    out.format = TagFormat::OPENTAG3D;

    char base_material[OT3D::BASE_MATERIAL_LEN + 1];
    char modifiers[OT3D::MODIFIERS_LEN + 1];
    read_str_field(rec.payload, rec.payload_len, OT3D::BASE_MATERIAL, OT3D::BASE_MATERIAL_LEN, base_material, sizeof(base_material));
    read_str_field(rec.payload, rec.payload_len, OT3D::MODIFIERS, OT3D::MODIFIERS_LEN, modifiers, sizeof(modifiers));
    if (base_material[0] == '\0') strlcpy(base_material, "PLA", sizeof(base_material));
    if (modifiers[0] != '\0') {
        snprintf(out.type_name, sizeof(out.type_name), "%s %s", base_material, modifiers);
    } else {
        strlcpy(out.type_name, base_material, sizeof(out.type_name));
    }

    read_str_field(rec.payload, rec.payload_len, OT3D::MANUFACTURER, OT3D::MANUFACTURER_LEN, out.brand, sizeof(out.brand));
    if (out.brand[0] == '\0') strlcpy(out.brand, "OpenTag3D", sizeof(out.brand));

    read_str_field(rec.payload, rec.payload_len, OT3D::COLOR_NAME, OT3D::COLOR_NAME_LEN, out.color, sizeof(out.color));
    if (out.color[0] == '\0') {
        // Fall back to the RGBA hex value when no human-readable color name was written.
        if (OT3D::COLOR_RGBA + OT3D::COLOR_RGBA_LEN <= rec.payload_len) {
            const uint8_t *rgba = rec.payload + OT3D::COLOR_RGBA;
            snprintf(out.color, sizeof(out.color), "#%02X%02X%02X", rgba[0], rgba[1], rgba[2]);
        } else {
            strlcpy(out.color, "unbekannt", sizeof(out.color));
        }
    }

    out.weight_grams = read_u16be(rec.payload, rec.payload_len, OT3D::TARGET_WEIGHT);

    read_str_field(rec.payload, rec.payload_len, OT3D::SERIAL_BATCH_ID, OT3D::SERIAL_BATCH_ID_LEN,
                    out.vendor_serial, sizeof(out.vendor_serial));
    out.has_serial = out.vendor_serial[0] != '\0';

    return true;
}
