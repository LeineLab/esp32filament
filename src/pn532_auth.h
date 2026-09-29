#pragma once
#include <Adafruit_PN532.h>

// See pn532_auth.cpp for the full investigation this is built on: a real,
// maintainer-acknowledged bug in arduino-esp32 3.x's I2C-NG driver
// (espressif/esp-idf#19041) causes intermittent ESP_ERR_INVALID_STATE
// failures on this project's own hardware, confirmed via real logs showing
// the identical error recurring during plain idle polling — not just around
// PN532 command sequences. mifareclassic_AuthenticateBlock()/
// ReadDataBlock()/WriteDataBlock() all return failure identically whether
// this happened or a real MIFARE-level rejection occurred, so every
// "Authentifizierung fehlgeschlagen" seen throughout this project's real-
// hardware testing could have been this, not a wrong key.

// Resets the ESP32 side's I2C driver (Wire.end()+Wire.begin()) — the closest
// achievable equivalent to i2c_master_bus_reset() (the ESP-IDF maintainers'
// own confirmed workaround for #19041), which isn't exposed through
// Arduino's Wire API. Only resets the ESP32 side; the PN532 module itself
// stays powered/configured (its SAMConfig() doesn't need repeating).
void i2c_bus_recover();

// Retries a MIFARE Classic block authentication up to `retries` times,
// calling i2c_bus_recover() between attempts. A genuine wrong-key failure
// just fails again immediately on retry (cheap — MIFARE's crypto1 handshake
// is a handful of milliseconds); a transient I2C glitch — which real logs
// show CAN and DOES happen with no PN532 command in flight at all — gets a
// real second/third chance to clear before this reports a genuine failure.
bool pn532_authenticate_retry(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len,
                               uint8_t block, const uint8_t *key6, uint8_t retries = 3);

// Same retry-with-recovery pattern for a 16-byte block read/write, once
// authentication (above) has already succeeded — the read/write step is
// just as exposed to the same I2C driver bug as the auth step itself.
bool pn532_read_block_retry(Adafruit_PN532 &nfc, uint8_t block, uint8_t *out16, uint8_t retries = 3);
bool pn532_write_block_retry(Adafruit_PN532 &nfc, uint8_t block, const uint8_t *data16, uint8_t retries = 3);
