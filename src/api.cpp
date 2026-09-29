#include "api.h"
#include "config.h"
#include "certs.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFi.h>

static bool g_use_https = false;

// ── Helpers ───────────────────────────────────────────────────────────────────

static void build_url(char *buf, size_t len, const char *path) {
    snprintf(buf, len, "%s%s", API_BASE_URL, path);
}

static void set_headers(HTTPClient &http) {
    http.addHeader("Authorization", "Bearer " API_TOKEN);
    http.addHeader("Content-Type",  "application/json");
    http.setTimeout(HTTP_TIMEOUT_MS);
}

static bool begin_request(HTTPClient &http, WiFiClientSecure &secure, const char *url) {
    if (g_use_https) {
        secure.setCACert(ISRG_ROOT_X1);
        return http.begin(secure, url);
    }
    return http.begin(url);
}

// Builds the shared request body {brand_name, type_name, weight_grams, color,
// vendor_serial?} used by /scan, /checkin and /checkout alike.
static void build_tag_body(const FilamentTagData &tag, uint16_t weight_grams, String &out) {
    JsonDocument doc;
    doc["brand_name"]    = tag.brand;
    doc["type_name"]     = tag.type_name;
    doc["weight_grams"]  = weight_grams;
    doc["color"]         = tag.color;
    if (tag.has_serial) doc["vendor_serial"] = tag.vendor_serial;
    serializeJson(doc, out);
}

// ── Public API ────────────────────────────────────────────────────────────────

void api_init() {
    g_use_https = (strncmp(API_BASE_URL, "https://", 8) == 0);
    Serial.printf("API: %s mode — %s\n", g_use_https ? "HTTPS" : "HTTP", API_BASE_URL);
}

bool api_scan(const FilamentTagData &tag, ApiScanResult &out) {
    char url[128];
    build_url(url, sizeof(url), "/api/v1/filament/scan");

    String body;
    build_tag_body(tag, tag.weight_grams, body);

    WiFiClientSecure secure;
    HTTPClient http;
    if (!begin_request(http, secure, url)) {
        strlcpy(out.error, "connection failed", sizeof(out.error));
        return false;
    }
    set_headers(http);
    int code = http.POST(body);
    String resp_body = http.getString();
    http.end();

    if (code != 200) {
        JsonDocument resp;
        deserializeJson(resp, resp_body);
        strlcpy(out.error, resp["detail"] | "HTTP error", sizeof(out.error));
        return false;
    }

    JsonDocument resp;
    if (deserializeJson(resp, resp_body)) {
        strlcpy(out.error, "invalid response", sizeof(out.error));
        return false;
    }

    out.ok = true;
    out.identify_required = resp["identify_required"] | false;
    strlcpy(out.action, resp["action"] | "", sizeof(out.action));
    if (!out.identify_required) {
        JsonObject roll = resp["roll"];
        strlcpy(out.brand_name, roll["brand_name"] | "", sizeof(out.brand_name));
        strlcpy(out.type_name,  roll["type_name"]  | "", sizeof(out.type_name));
        strlcpy(out.color,      roll["color"]      | "", sizeof(out.color));
        out.weight_grams = roll["weight_grams"] | 0;
    } else {
        out.current_stock = resp["current_stock"] | 0;
    }
    return true;
}

static bool post_action(const char *path, const FilamentTagData &tag, uint16_t weight_grams, ApiActionResult &out) {
    char url[128];
    build_url(url, sizeof(url), path);

    String body;
    build_tag_body(tag, weight_grams, body);

    WiFiClientSecure secure;
    HTTPClient http;
    if (!begin_request(http, secure, url)) {
        strlcpy(out.error, "connection failed", sizeof(out.error));
        return false;
    }
    set_headers(http);
    int code = http.POST(body);
    String resp_body = http.getString();
    http.end();

    if (code != 200) {
        JsonDocument resp;
        deserializeJson(resp, resp_body);
        strlcpy(out.error, resp["detail"] | "HTTP error", sizeof(out.error));
        return false;
    }

    JsonDocument resp;
    if (deserializeJson(resp, resp_body)) {
        strlcpy(out.error, "invalid response", sizeof(out.error));
        return false;
    }

    out.ok = true;
    strlcpy(out.brand_name, resp["brand_name"] | "", sizeof(out.brand_name));
    strlcpy(out.type_name,  resp["type_name"]  | "", sizeof(out.type_name));
    strlcpy(out.color,      resp["color"]      | "", sizeof(out.color));
    out.weight_grams = resp["weight_grams"] | 0;
    return true;
}

bool api_checkin(const FilamentTagData &tag, uint16_t weight_grams, ApiActionResult &out) {
    return post_action("/api/v1/filament/checkin", tag, weight_grams, out);
}

bool api_checkout(const FilamentTagData &tag, uint16_t weight_grams, ApiActionResult &out) {
    return post_action("/api/v1/filament/checkout", tag, weight_grams, out);
}

bool api_status(const FilamentTagData &tag, ApiStatusResult &out) {
    char url[128];
    build_url(url, sizeof(url), "/api/v1/filament/status");

    JsonDocument doc;
    doc["vendor_serial"] = tag.vendor_serial;
    String body;
    serializeJson(doc, body);

    WiFiClientSecure secure;
    HTTPClient http;
    if (!begin_request(http, secure, url)) {
        strlcpy(out.error, "connection failed", sizeof(out.error));
        return false;
    }
    set_headers(http);
    int code = http.POST(body);
    String resp_body = http.getString();
    http.end();

    if (code != 200) {
        JsonDocument resp;
        deserializeJson(resp, resp_body);
        strlcpy(out.error, resp["detail"] | "HTTP error", sizeof(out.error));
        return false;
    }

    JsonDocument resp;
    if (deserializeJson(resp, resp_body)) {
        strlcpy(out.error, "invalid response", sizeof(out.error));
        return false;
    }

    out.ok = true;
    out.in_stock = resp["in_stock"] | false;
    return true;
}
