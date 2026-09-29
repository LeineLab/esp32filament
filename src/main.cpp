#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <lvgl.h>

#include "config.h"
#include "display.h"
#include "api.h"
#include "nfc_reader.h"
#include "ui.h"
#include "led.h"
#include "strings.h"

// ── Global display instance ───────────────────────────────────────────────────
LGFX gfx;

// ── LVGL display buffer ───────────────────────────────────────────────────────
static const int LVGL_BUF_LINES = 20;
static lv_color_t buf1[DISP_WIDTH * LVGL_BUF_LINES];
static lv_color_t buf2[DISP_WIDTH * LVGL_BUF_LINES];
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t      disp_drv;
static lv_indev_drv_t     touch_drv;

// ── LVGL callbacks ────────────────────────────────────────────────────────────

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    gfx.startWrite();
    gfx.setAddrWindow(area->x1, area->y1, w, h);
    gfx.writePixels((lgfx::rgb565_t *)color_p, w * h);
    gfx.endWrite();
    lv_disp_flush_ready(drv);
}

static void lvgl_touch_cb(lv_indev_drv_t *, lv_indev_data_t *data) {
    uint16_t x, y;
    if (gfx.getTouch(&x, &y)) {
        data->state   = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
        // Raw touch detection, independent of which widget (if any) it
        // actually lands on — wakes the dimmed IDLE screen even from a touch
        // that misses prog_enter_btn entirely (see ui_on_touch_activity()'s
        // own comment for why that matters).
        ui_on_touch_activity();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ── Boot screen ──────────────────────────────────────────────────────────────
//
// Without this, nothing is drawn until ui_init() runs, so the screen stays
// solid black for the entire boot sequence (WiFi connect alone can take up
// to ~15s) with zero indication the device is doing anything at all. This
// draws directly via LovyanGFX rather than LVGL, since LVGL isn't
// initialized yet at this point in setup() — `gfx.color888(r,g,b)` is
// LovyanGFX's own portable way to specify a color regardless of the panel's
// actual bit depth, mirrored from colors.h's CLR_BG/CLR_TEXT_PRIMARY (those
// constants are formatted for lv_color_hex(), not directly usable here).
static void draw_boot_screen(const char *line2) {
    gfx.fillScreen(gfx.color888(0x1a, 0x1a, 0x2e));  // CLR_BG
    gfx.setTextColor(gfx.color888(0xe0, 0xe0, 0xe0), gfx.color888(0x1a, 0x1a, 0x2e));  // CLR_TEXT_PRIMARY on CLR_BG
    gfx.setTextDatum(lgfx::textdatum_t::middle_center);
    gfx.setTextSize(2);
    gfx.drawString(STR_APP_TITLE, gfx.width() / 2, gfx.height() / 2 - 12);
    gfx.setTextSize(1);
    gfx.drawString(line2, gfx.width() / 2, gfx.height() / 2 + 16);
}

// ── WiFi ──────────────────────────────────────────────────────────────────────

static void wifi_connect() {
    draw_boot_screen(STR_CONNECTING);  // previously defined but never actually shown anywhere
    Serial.print("Connecting to WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print('.');
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWiFi OK - IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\nWiFi FAILED - continuing without network");
        draw_boot_screen(STR_NO_CONNECTION);
        delay(800);  // long enough to actually be readable before boot continues
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("ESP32 Filament Station booting...");

    // ── Display ──────────────────────────────────────────────────────────────
    gfx.init();
    gfx.setRotation(1);   // landscape
    gfx.setBrightness(BRIGHTNESS_FULL);
    draw_boot_screen(STR_BOOTING);

    // ── LVGL ─────────────────────────────────────────────────────────────────
    lv_init();

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, DISP_WIDTH * LVGL_BUF_LINES);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = DISP_WIDTH;
    disp_drv.ver_res  = DISP_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&touch_drv);
    touch_drv.type    = LV_INDEV_TYPE_POINTER;
    touch_drv.read_cb = lvgl_touch_cb;
    lv_indev_drv_register(&touch_drv);

    // ── LED ──────────────────────────────────────────────────────────────────
    led_init();
    led_blue();   // blue = booting

    // ── WiFi ─────────────────────────────────────────────────────────────────
    wifi_connect();

    // ── OTA ──────────────────────────────────────────────────────────────────
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { led_blue(); ui_show_ota(0); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        ui_show_ota((int)((progress * 100u) / total));
    });
    ArduinoOTA.onEnd([]() { ui_show_ota(100); led_green(); delay(400); });
    ArduinoOTA.onError([](ota_error_t error) {
        led_red();
        Serial.printf("OTA error [%u]\n", error);
    });
    ArduinoOTA.begin();
    Serial.printf("OTA ready - hostname: %s\n", OTA_HOSTNAME);

    // ── API ──────────────────────────────────────────────────────────────────
    api_init();

    // ── NFC ──────────────────────────────────────────────────────────────────
    if (!nfc_init()) {
        Serial.println("WARNING: NFC reader not found - check I2C wiring (SDA/SCL)");
    }

    // ── UI ───────────────────────────────────────────────────────────────────
    ui_init(disp);
    ui_set_wifi_connected(WiFi.status() == WL_CONNECTED);

    led_off();
    Serial.println("Boot complete.");
}

// ── Loop ─────────────────────────────────────────────────────────────────────

void loop() {
    ArduinoOTA.handle();

    lv_timer_handler();

    static unsigned long last_wifi_check = 0;
    if (millis() - last_wifi_check > 2000) {
        ui_set_wifi_connected(WiFi.status() == WL_CONNECTED);
        last_wifi_check = millis();
    }

    if (nfc_available()) {
        FilamentTagData tag;
        if (nfc_read(tag)) {
            ui_on_tag_scanned(tag);
        }
    }

    ui_tick();

    delay(5);
}
