#include "ndef.h"

static const int NDEF_START_PAGE = 4;   // first user-memory page on a Type 2 Tag

bool ndef_read_first_record(Adafruit_PN532 &nfc, uint8_t *buffer, size_t buffer_cap, NdefRecord &out) {
    if (buffer_cap < 8) return false;

    // Read the first page to find the NDEF Message TLV and its length.
    if (!nfc.ntag2xx_ReadPage(NDEF_START_PAGE, buffer)) return false;

    if (buffer[0] != 0x03) return false;  // not an NDEF Message TLV — unsupported layout

    size_t msg_len;
    size_t msg_offset;  // offset of the NDEF message itself, relative to buffer[0]
    if (buffer[1] != 0xFF) {
        msg_len    = buffer[1];
        msg_offset = 2;
    } else {
        // 3-byte length form: 0xFF, then a 16-bit big-endian length. Only one
        // more byte (buffer[3]) is available from this first page read — read
        // one more page to get the rest before computing the length.
        uint8_t page5[4];
        if (!nfc.ntag2xx_ReadPage(NDEF_START_PAGE + 1, page5)) return false;
        msg_len    = (static_cast<size_t>(buffer[2]) << 8) | buffer[3];
        msg_offset = 4;
        // Splice page5 in — everything from here on reads sequentially from page 4.
        // (page5 bytes occupy buffer[4..7], which we're about to fill properly below anyway.)
        memcpy(buffer + 4, page5, 4);
    }

    size_t total_needed = msg_offset + msg_len;
    if (total_needed > buffer_cap) return false;  // message too large for our buffer

    // Read whatever additional pages are needed (4 bytes each), continuing
    // from wherever the initial read(s) above left off.
    size_t have_bytes = (buffer[1] == 0xFF) ? 8 : 4;
    int next_page = NDEF_START_PAGE + (have_bytes / 4);
    while (have_bytes < total_needed) {
        if (!nfc.ntag2xx_ReadPage(next_page, buffer + have_bytes)) return false;
        have_bytes += 4;
        next_page++;
    }

    // ── Parse the first NDEF record ────────────────────────────────────────
    size_t off = msg_offset;
    size_t end = msg_offset + msg_len;
    if (off >= end) return false;

    uint8_t header = buffer[off++];
    bool sr = header & 0x10;  // Short Record — 1-byte payload length
    bool il = header & 0x08;  // ID Length field present

    if (off >= end) return false;
    uint8_t type_len = buffer[off++];

    uint32_t payload_len;
    if (sr) {
        if (off >= end) return false;
        payload_len = buffer[off++];
    } else {
        if (off + 4 > end) return false;
        payload_len = (static_cast<uint32_t>(buffer[off]) << 24) | (static_cast<uint32_t>(buffer[off + 1]) << 16) |
                      (static_cast<uint32_t>(buffer[off + 2]) << 8) | buffer[off + 3];
        off += 4;
    }

    uint8_t id_len = 0;
    if (il) {
        if (off >= end) return false;
        id_len = buffer[off++];
    }

    if (off + type_len > end) return false;
    size_t type_copy_len = type_len < sizeof(out.type) - 1 ? type_len : sizeof(out.type) - 1;
    memcpy(out.type, buffer + off, type_copy_len);
    out.type[type_copy_len] = '\0';
    off += type_len;

    off += id_len;  // ID field, unused — skip

    if (off + payload_len > end) return false;
    out.payload     = buffer + off;
    out.payload_len = payload_len;
    return true;
}

bool ndef_write_message(Adafruit_PN532 &nfc, const char *mime_type, const uint8_t *payload, size_t payload_len) {
    size_t type_len = strlen(mime_type);
    // NDEF record: header(1) + type_len(1) + payload_len(1, short record) + type + payload.
    // NDEF Message TLV: tag(1) + length(1, the 1-byte form only) + record + terminator(1).
    size_t record_len = 3 + type_len + payload_len;
    if (type_len > 255 || record_len > 254) return false;  // would need the 3-byte TLV length form — not implemented, not needed by any format this project writes

    uint8_t buf[300];
    size_t total = 2 + record_len + 1;  // TLV tag+length, record, terminator
    if (total > sizeof(buf)) return false;

    size_t off = 0;
    buf[off++] = 0x03;                  // NDEF Message TLV
    buf[off++] = (uint8_t)record_len;
    buf[off++] = 0xD2;                  // MB=1 ME=1 CF=0 SR=1 IL=0 TNF=0x02 (media-type record)
    buf[off++] = (uint8_t)type_len;
    buf[off++] = (uint8_t)payload_len;
    memcpy(buf + off, mime_type, type_len);
    off += type_len;
    memcpy(buf + off, payload, payload_len);
    off += payload_len;
    buf[off++] = 0xFE;                  // Terminator TLV

    size_t padded = ((off + 3) / 4) * 4;
    if (padded > sizeof(buf)) return false;
    for (size_t i = off; i < padded; i++) buf[i] = 0x00;

    for (size_t page_off = 0; page_off < padded; page_off += 4) {
        if (!nfc.ntag2xx_WritePage(NDEF_START_PAGE + (int)(page_off / 4), buf + page_off)) return false;
    }
    return true;
}
