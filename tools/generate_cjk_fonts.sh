#!/usr/bin/env bash
# Regenerate LVGL CJK font sources from IPA Gothic.
#
# Prerequisites:
#   - IPA Gothic TTF (default path: /usr/share/fonts/truetype/fonts-japanese-gothic.ttf)
#   - lv_font_conv (npm install -g lv_font_conv)
#
# The generated files are derived programs under IPA Font License v1.0.
# See ../IPA_Font_License_Agreement_v1.0.txt.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
FONT_FILE="${1:-/usr/share/fonts/truetype/fonts-japanese-gothic.ttf}"
OUTPUT_DIR="${PROJECT_DIR}/main/fonts"

if ! command -v lv_font_conv >/dev/null 2>&1; then
    echo "error: lv_font_conv is not installed. Run: npm install -g lv_font_conv" >&2
    exit 1
fi

if [[ ! -f "${FONT_FILE}" ]]; then
    echo "error: IPA Gothic TTF not found: ${FONT_FILE}" >&2
    echo "Download IPA fonts from https://moji.or.jp/ipafont/ and pass the TTF path as argument." >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

# Character ranges: ASCII, CJK punctuation, Hiragana, Katakana, CJK Unified
# Ideographs, full-width ASCII, and half-width Katakana.
RANGES=(
    -r 0x0020-0x007E
    -r 0x3000-0x303F
    -r 0x3040-0x309F
    -r 0x30A0-0x30FF
    -r 0x4E00-0x9FFF
    -r 0xFF00-0xFFEF
)

for size in 16 28; do
    output="${OUTPUT_DIR}/lv_font_cjk_${size}.c"
    lv_font_conv \
        --font "${FONT_FILE}" \
        --size "${size}" \
        --bpp 1 \
        --format lvgl \
        --lv-font-name "lv_font_cjk_${size}" \
        "${RANGES[@]}" \
        -o "${output}"

    # ESP-IDF's LVGL component exports lvgl.h directly; it does not export
    # the lvgl/lvgl.h include path emitted by some lv_font_conv versions.
    sed -i 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' "${output}"
    sed -i 's|^#include "lvgl.h"$|/*\\n * Derived from IPA Gothic and licensed under IPA Font License v1.0.\\n * See ../../IPA_Font_License_Agreement_v1.0.txt.\\n * Original font: https://moji.or.jp/ipafont/\\n */\\n#include "lvgl.h"|' "${output}"
done

echo "Generated ${OUTPUT_DIR}/lv_font_cjk_16.c and lv_font_cjk_28.c"
