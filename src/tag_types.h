#pragma once
#include <Arduino.h>

// Common result of parsing any of the four supported spool tag formats.
// Produced by tag_bambu.cpp / tag_creality.cpp / tag_openspool.cpp /
// tag_opentag3d.cpp; consumed by ui.cpp and api.cpp — none of those need to
// know which tag format actually produced the data.

enum class TagFormat {
    UNKNOWN,      // detected an NFC target but couldn't identify/parse its format
    BAMBU,
    CREALITY,
    OPENSPOOL,
    OPENTAG3D,
};

struct FilamentTagData {
    TagFormat format = TagFormat::UNKNOWN;

    char brand[32]      = "";
    char type_name[48]  = "";
    char color[24]      = "";   // "#RRGGBB" or a human color name, depending on format
    uint16_t weight_grams = 0;  // 0 = not encoded on the tag — station must ask (see DEFAULT_WEIGHT_GRAMS)

    bool has_serial = false;
    char vendor_serial[40] = "";

    // The physical target's own UID, captured unconditionally by nfc_reader.cpp
    // regardless of whether any format parser above recognized it — normal
    // scan/checkin/checkout never uses this (identity is vendor_serial, never
    // the hardware UID, see CLAUDE.md #69), but the tag-writing wizard
    // (program mode, see ui.cpp/nfc_reader.cpp) needs it to derive Bambu Lab's
    // per-sector MIFARE Classic keys for whichever physical tag is currently
    // on the reader.
    uint8_t uid[7]  = {0};
    uint8_t uid_len = 0;
};

inline const char *tag_format_name(TagFormat f) {
    switch (f) {
        case TagFormat::BAMBU:     return "Bambu Lab";
        case TagFormat::CREALITY:  return "Creality";
        case TagFormat::OPENSPOOL: return "OpenSpool";
        case TagFormat::OPENTAG3D: return "OpenTag3D";
        default:                   return "unbekannt";
    }
}
