#include "tag_openspool.h"
#include "ndef.h"
#include "strings.h"
#include "hexdump.h"
#include <ArduinoJson.h>

// ── Writing (program mode / test-tag wizard) ────────────────────────────────

bool tag_write_openspool(Adafruit_PN532 &nfc, const FilamentTagData &in, char *err, size_t err_cap) {
    JsonDocument doc;
    doc["protocol"] = "openspool";
    doc["version"]  = 1;
    doc["type"]     = in.type_name;
    doc["color_hex"] = (in.color[0] == '#') ? in.color + 1 : in.color;
    doc["brand"] = in.brand;

    char json[192];
    size_t len = serializeJson(doc, json, sizeof(json));
    if (len == 0 || len >= sizeof(json)) {
        strlcpy(err, STR_ERR_JSON_TOO_LARGE, err_cap);
        return false;
    }
    if (!ndef_write_message(nfc, "application/json", (const uint8_t *)json, len)) {
        strlcpy(err, STR_ERR_NDEF_WRITE_FAILED_OPENSPOOL, err_cap);
        return false;
    }
    return true;
}

bool tag_parse_openspool(Adafruit_PN532 &nfc, FilamentTagData &out) {
    uint8_t buf[256];
    NdefRecord rec;
    if (!ndef_read_first_record(nfc, buf, sizeof(buf), rec)) return false;
    Serial.printf("  NDEF-Record gefunden, MIME-Typ: \"%s\"\n", rec.type);
    hexdump("NDEF-Payload (OpenSpool-Versuch)", rec.payload, rec.payload_len);
    if (strcmp(rec.type, "application/json") != 0) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, rec.payload, rec.payload_len);
    if (err) return false;

    const char *protocol = doc["protocol"] | "";
    if (strcmp(protocol, "openspool") != 0) return false;  // some other JSON MIME record, not ours

    out.format = TagFormat::OPENSPOOL;
    strlcpy(out.brand, doc["brand"] | "Generic", sizeof(out.brand));
    strlcpy(out.type_name, doc["type"] | "PLA", sizeof(out.type_name));

    const char *color_hex = doc["color_hex"] | "";
    if (color_hex[0] != '\0') {
        snprintf(out.color, sizeof(out.color), "#%s", color_hex);
    } else {
        strlcpy(out.color, "unbekannt", sizeof(out.color));
    }

    // OpenSpool carries no weight and no serial by design — the station
    // shows the manual Einbuchen/Ausbuchen choice with a weight picker.
    out.weight_grams = 0;
    out.has_serial   = false;
    out.vendor_serial[0] = '\0';
    return true;
}
