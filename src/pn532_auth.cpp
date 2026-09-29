#include "pn532_auth.h"
#include "config.h"
#include <Wire.h>

// arduino-esp32 3.x's I2C-NG driver has a known, maintainer-acknowledged bug
// (espressif/esp-idf#19041) that can cause intermittent
// "i2c_master_receive failed: ESP_ERR_INVALID_STATE" errors, including
// during plain idle polling with no PN532 command in flight — not just under
// heavy command traffic. The ESP-IDF maintainers' own confirmed workaround
// is to call i2c_master_bus_reset() after a failure, which isn't exposed
// through Arduino's Wire API — a full Wire.end()+Wire.begin() cycle
// re-initializes the same underlying I2C peripheral/driver state and is the
// closest equivalent reachable through the public API.
void i2c_bus_recover() {
    Wire.end();
    vTaskDelay(pdMS_TO_TICKS(5));
    Wire.begin(PN532_SDA, PN532_SCL);
}

// Recovering the I2C bus BETWEEN distinct PN532 commands only protects the
// NEXT command from a glitch that already happened — it does nothing for a
// glitch that hits the very attempt it's meant to protect, and the I2C-NG
// bug above can affect any single transaction, including one with no other
// commands nearby. These wrappers instead retry the SAME operation itself,
// recovering the bus between attempts, before reporting a genuine failure.
// A real wrong key fails identically on every retry (cheap — MIFARE's own
// crypto1 handshake is a handful of milliseconds); a transient I2C glitch
// gets a real second/third chance to clear. Applied uniformly to auth, read,
// and write — all three are equally exposed to the underlying I2C driver bug.
bool pn532_authenticate_retry(Adafruit_PN532 &nfc, const uint8_t *uid, uint8_t uid_len,
                               uint8_t block, const uint8_t *key6, uint8_t retries) {
    uint8_t uid_copy[7];
    memcpy(uid_copy, uid, uid_len);
    for (uint8_t attempt = 0; attempt < retries; attempt++) {
        uint8_t key_copy[6];
        memcpy(key_copy, key6, 6);
        if (nfc.mifareclassic_AuthenticateBlock(uid_copy, uid_len, block, /*keyNumber=*/0, key_copy)) {
            return true;
        }
        if (attempt + 1 < retries) i2c_bus_recover();
    }
    return false;
}

bool pn532_read_block_retry(Adafruit_PN532 &nfc, uint8_t block, uint8_t *out16, uint8_t retries) {
    for (uint8_t attempt = 0; attempt < retries; attempt++) {
        if (nfc.mifareclassic_ReadDataBlock(block, out16) == 1) return true;
        if (attempt + 1 < retries) i2c_bus_recover();
    }
    return false;
}

bool pn532_write_block_retry(Adafruit_PN532 &nfc, uint8_t block, const uint8_t *data16, uint8_t retries) {
    uint8_t buf[16];
    for (uint8_t attempt = 0; attempt < retries; attempt++) {
        memcpy(buf, data16, 16);
        if (nfc.mifareclassic_WriteDataBlock(block, buf) == 1) return true;
        if (attempt + 1 < retries) i2c_bus_recover();
    }
    return false;
}
