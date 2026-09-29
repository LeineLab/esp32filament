#include "bambu_kdf.h"
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

// Fixed master "salt" published by the Bambu-Research-Group reverse-engineering
// effort (deriveKeys.py) — not a secret we're protecting, just the community's
// recovered constant.
static const uint8_t BAMBU_MASTER[16] = {
    0x9a, 0x75, 0x9c, 0xf2, 0xc4, 0xf7, 0xca, 0xff,
    0x22, 0x2c, 0xb9, 0x76, 0x9b, 0x41, 0xbc, 0x96,
};

static const uint8_t BAMBU_INFO[7] = {'R', 'F', 'I', 'D', '-', 'A', '\0'};

void bambu_derive_sector_keys(const uint8_t *uid, uint8_t uid_len, uint8_t num_sectors, uint8_t *out_keys) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_hkdf(
        md,
        BAMBU_MASTER, sizeof(BAMBU_MASTER),
        uid, uid_len,
        BAMBU_INFO, sizeof(BAMBU_INFO),
        out_keys, static_cast<size_t>(num_sectors) * 6
    );
}
