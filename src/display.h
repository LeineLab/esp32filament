#pragma once

// LovyanGFX display configuration for the "Cheap Yellow Display" (CYD,
// ESP32-2432S028). Unlike the esp32rental board, display and touch sit on
// two SEPARATE SPI buses here (HSPI for the panel, VSPI for XPT2046) — no
// bus_shared coordination needed.
//
// Sources (see README.md for full citations): randomnerdtutorials.com and
// witnessmenow/ESP32-Cheap-Yellow-Display for pin assignments; the two
// documented CYD display-driver revisions (ILI9341 / ST7789) share this
// exact pinout and resolution, differing only in the panel driver class
// selected below via -DCYD_PANEL_ST7789 (see platformio.ini).

#include <LovyanGFX.hpp>
#include "config.h"

class LGFX : public lgfx::LGFX_Device {
#ifdef CYD_PANEL_ST7789
    lgfx::Panel_ST7789   _panel;
#else
    lgfx::Panel_ILI9341  _panel;
#endif
    lgfx::Bus_SPI        _bus;
    lgfx::Light_PWM      _light;
    lgfx::Touch_XPT2046  _touch;

public:
    LGFX() {
        // ── SPI bus for the display (HSPI) ─────────────────────────────────────
        {
            auto cfg = _bus.config();
            cfg.spi_host    = HSPI_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = TFT_CLK;
            cfg.pin_mosi    = TFT_MOSI;
            cfg.pin_miso    = TFT_MISO;
            cfg.pin_dc      = TFT_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }

        // ── Panel (native portrait 240×320) ────────────────────────────────────
        {
            auto cfg = _panel.config();
            cfg.pin_cs           = TFT_CS;
            cfg.pin_rst          = TFT_RST;
            cfg.pin_busy         = -1;
            cfg.panel_width      = 240;   // native portrait width
            cfg.panel_height     = 320;   // native portrait height
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = true;
            cfg.invert           = false;  // ST7789 CYD boards sometimes need true — try both if colors look wrong
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;  // display and touch are on independent buses on the CYD
            _panel.config(cfg);
        }

        // ── Backlight ─────────────────────────────────────────────────────────
        {
            auto cfg = _light.config();
            cfg.pin_bl      = TFT_BL;
            cfg.invert      = TFT_BL_ACTIVE_LOW;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }

        // ── XPT2046 touch — its own SPI bus (VSPI), not shared with the panel ──
        {
            auto cfg = _touch.config();
            cfg.pin_cs          = TOUCH_CS;
            cfg.pin_int         = TOUCH_IRQ;  // GPIO36 is input-only; polling, not interrupt-driven
            cfg.pin_sclk        = TOUCH_CLK;
            cfg.pin_mosi        = TOUCH_MOSI;
            cfg.pin_miso        = TOUCH_MISO;
            cfg.bus_shared      = false;      // genuinely independent bus on this board
            cfg.spi_host        = VSPI_HOST;
            cfg.freq            = 2500000;
            cfg.offset_rotation = 0;
            // TOUCH_{X,Y}_INVERTED (config.h) swap the calibration min/max to
            // flip that axis — confirmed necessary for Y on a real ST7789 CYD
            // board (see config.h.example's comment).
#if TOUCH_X_INVERTED
            cfg.x_min           = TOUCH_CAL_X_MAX;
            cfg.x_max           = TOUCH_CAL_X_MIN;
#else
            cfg.x_min           = TOUCH_CAL_X_MIN;
            cfg.x_max           = TOUCH_CAL_X_MAX;
#endif
#if TOUCH_Y_INVERTED
            cfg.y_min           = TOUCH_CAL_Y_MAX;
            cfg.y_max           = TOUCH_CAL_Y_MIN;
#else
            cfg.y_min           = TOUCH_CAL_Y_MIN;
            cfg.y_max           = TOUCH_CAL_Y_MAX;
#endif
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }

        setPanel(&_panel);
    }
};

extern LGFX gfx;
