#pragma once
#include <Arduino.h>

// Bambu Lab RFID tag MIFARE Classic sector key derivation.
//
// Source: Bambu-Research-Group/RFID-Tag-Guide, deriveKeys.py:
//   HKDF-SHA256(ikm=<tag UID bytes>, salt=<fixed 16-byte master below>,
//               info=b"RFID-A\0", key_len=6 bytes per key)
// i.e. HKDF-Expand output is sliced into consecutive 6-byte MIFARE Classic
// Key A values, one per sector (sector N uses the Nth derived key).
//
// Derives only `num_sectors` keys (this project only ever needs sectors 0–2 —
// see tag_bambu.cpp) rather than all 16, to keep this fast and simple.
// out_keys must have room for num_sectors * 6 bytes.
void bambu_derive_sector_keys(const uint8_t *uid, uint8_t uid_len, uint8_t num_sectors, uint8_t *out_keys);
