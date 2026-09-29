#!/usr/bin/env bash
# Generate LVGL 8.x Montserrat font C files with German umlaut support.
#
# Outputs 3 font files to src/fonts/ covering:
#   - Basic Latin (U+0020-U+007E)
#   - Latin-1 Supplement (U+00A0-U+00FF)  <- ä ö ü Ä Ö Ü ß and other accents
#
# No LVGL symbol glyphs (LV_SYMBOL_*) — this UI doesn't use any, unlike
# esp32rental/scripts/gen_fonts.sh, which is why this script is simpler.
#
# Requirements:
#   - Node.js + npm (tested with Node 18+)
#   - Internet access (first run only — downloads the Montserrat TTF)
#
# After running this script:
#   1. -DLVGL_EXT_FONTS is already on in platformio.ini — just rebuild.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
OUT_DIR="$PROJECT_DIR/src/fonts"
FONT_FILE="$SCRIPT_DIR/Montserrat-Regular.ttf"

RANGE="0x20-0x7E,0xA0-0xFF"
SIZES=(14 16 20)

if ! command -v node &> /dev/null; then
    echo "Error: Node.js not found. Install from https://nodejs.org/" >&2
    exit 1
fi

if ! command -v lv_font_conv &> /dev/null; then
    echo "Installing lv_font_conv..."
    npm install -g lv_font_conv
fi

if [ ! -f "$FONT_FILE" ]; then
    echo "Downloading Montserrat-Regular.ttf..."
    curl -fLk \
        "https://github.com/JulietaUla/Montserrat/raw/master/fonts/ttf/Montserrat-Regular.ttf" \
        -o "$FONT_FILE"
fi

mkdir -p "$OUT_DIR"

for SIZE in "${SIZES[@]}"; do
    OUT="$OUT_DIR/montserrat_ext_${SIZE}.c"
    echo "Generating montserrat_ext_${SIZE}..."
    lv_font_conv \
        --font "$FONT_FILE" \
        --size "$SIZE" \
        --bpp 4 \
        --range "$RANGE" \
        --format lvgl \
        --no-compress \
        --lv-include "lvgl.h" \
        -o "$OUT"
done

echo ""
echo "Done. Font files written to src/fonts/:"
ls -lh "$OUT_DIR/"montserrat_ext_*.c
