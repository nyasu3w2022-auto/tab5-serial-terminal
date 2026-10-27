/*
 * sixel_graphics.cpp — Fixed-surface DEC Sixel implementation for TAB5.
 *
 * This is intentionally a small streaming decoder rather than a desktop
 * image-library integration. Its dimensions, palette, repeat count and input
 * length are all bounded before serial input can influence memory use.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sixel_graphics.h"

#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {

static const char *TAG = "sixel";
static constexpr size_t PIXEL_COUNT =
    (size_t)SIXEL_GRAPHICS_WIDTH * (size_t)SIXEL_GRAPHICS_HEIGHT;
static constexpr size_t MASK_BYTES = (PIXEL_COUNT + 7U) / 8U;
static constexpr size_t MASK_ROW_BYTES = (size_t)SIXEL_GRAPHICS_WIDTH / 8U;
static constexpr int SIXEL_PALETTE_SIZE = 256;
static constexpr int SIXEL_MAX_REPEAT = SIXEL_GRAPHICS_WIDTH;
static constexpr int SIXEL_MAX_PARAM = 1000000;

enum class parser_state_t : uint8_t {
    DATA,
    RASTER,
    REPEAT,
    COLOR,
};

// Displayed graphics plane: RGB565 plus a one-bit opacity mask. Keeping the
// mask separate lets a normal terminal-cell background remain visible where
// no Sixel pixel has been drawn.
static uint16_t *s_display_pixels = nullptr;
static uint8_t  *s_display_mask = nullptr;

// One fixed indexed staging image and its opacity mask. It is reused for every
// DCS body and committed only after a terminating ST was received.
static uint8_t *s_stage_indices = nullptr;
static uint8_t *s_stage_mask = nullptr;
static size_t s_display_opaque_pixels = 0;
// Logical Y=0 is stored at this physical row. Advancing the origin makes the
// image follow a terminal scroll without moving the 1.7 MiB RGB565 surface.
static int s_display_row_origin = 0;

static bool s_active = false;
static bool s_finished = false;
static bool s_background_transparent = false;
static size_t s_stream_bytes = 0;
static parser_state_t s_state = parser_state_t::DATA;
static int s_pos_x = 0;
static int s_pos_y = 0;
static int s_repeat = 1;
static int s_value = 0;
static int s_params[8] = {};
static int s_param_count = 0;
static int s_color_index = 15;
static int s_raster_width = 0;
static int s_raster_height = 0;
static int s_draw_width = 0;
static int s_draw_height = 0;
static bool s_has_opaque_pixels = false;
static uint16_t s_palette[SIXEL_PALETTE_SIZE] = {};

static inline bool bit_get(const uint8_t *mask, size_t index)
{
    return (mask[index >> 3U] & (uint8_t)(1U << (index & 7U))) != 0;
}

static inline void bit_set(uint8_t *mask, size_t index)
{
    mask[index >> 3U] |= (uint8_t)(1U << (index & 7U));
}

static inline void bit_clear(uint8_t *mask, size_t index)
{
    mask[index >> 3U] &= (uint8_t)~(1U << (index & 7U));
}

static int display_physical_row(int logical_y)
{
    int row = s_display_row_origin + logical_y;
    if (row >= SIXEL_GRAPHICS_HEIGHT) row %= SIXEL_GRAPHICS_HEIGHT;
    return row;
}

static void clear_display_physical_row(int physical_y)
{
    if (physical_y < 0 || physical_y >= SIXEL_GRAPHICS_HEIGHT || !s_display_mask) return;

    uint8_t *mask_row = s_display_mask + (size_t)physical_y * MASK_ROW_BYTES;
    for (size_t i = 0; i < MASK_ROW_BYTES; ++i) {
        s_display_opaque_pixels -= (size_t)__builtin_popcount((unsigned)mask_row[i]);
    }
    memset(mask_row, 0, MASK_ROW_BYTES);
}

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((uint16_t)(red & 0xf8U) << 8U) |
                      ((uint16_t)(green & 0xfcU) << 3U) |
                      ((uint16_t)blue >> 3U));
}

static uint8_t percent_to_u8(int value)
{
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    return (uint8_t)((value * 255 + 50) / 100);
}

static float hue_component(float p, float q, float hue)
{
    if (hue < 0.0f) hue += 1.0f;
    if (hue > 1.0f) hue -= 1.0f;
    if (hue < (1.0f / 6.0f)) return p + (q - p) * 6.0f * hue;
    if (hue < 0.5f) return q;
    if (hue < (2.0f / 3.0f)) return p + (q - p) * ((2.0f / 3.0f) - hue) * 6.0f;
    return p;
}

// DEC Sixel HLS is rotated by 120 degrees compared with the usual modern HLS
// hue ring. This follows the VT340-compatible conversion used by libsixel.
static uint16_t hls_to_rgb565(int hue, int lightness, int saturation)
{
    if (hue < 0) hue = 0;
    hue %= 360;
    hue = (hue + 240) % 360;
    if (lightness < 0) lightness = 0;
    if (lightness > 100) lightness = 100;
    if (saturation < 0) saturation = 0;
    if (saturation > 100) saturation = 100;

    const float h = (float)hue / 360.0f;
    const float l = (float)lightness / 100.0f;
    const float s = (float)saturation / 100.0f;
    float red = l;
    float green = l;
    float blue = l;
    if (s > 0.0f) {
        const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        const float p = 2.0f * l - q;
        red = hue_component(p, q, h + (1.0f / 3.0f));
        green = hue_component(p, q, h);
        blue = hue_component(p, q, h - (1.0f / 3.0f));
    }
    return rgb565((uint8_t)(red * 255.0f + 0.5f),
                  (uint8_t)(green * 255.0f + 0.5f),
                  (uint8_t)(blue * 255.0f + 0.5f));
}

static void initialise_palette(void)
{
    static const uint8_t basic[16][3] = {
        {0, 0, 0},       {51, 51, 204},  {204, 33, 33},  {51, 204, 51},
        {204, 51, 204},  {51, 204, 204}, {204, 204, 51}, {135, 135, 135},
        {66, 66, 66},    {84, 84, 153},  {153, 66, 66},  {84, 153, 84},
        {153, 84, 153},  {84, 153, 153}, {153, 153, 84}, {204, 204, 204},
    };
    for (int i = 0; i < 16; ++i) {
        s_palette[i] = rgb565(basic[i][0], basic[i][1], basic[i][2]);
    }

    int index = 16;
    for (int red = 0; red < 6; ++red) {
        for (int green = 0; green < 6; ++green) {
            for (int blue = 0; blue < 6; ++blue) {
                s_palette[index++] = rgb565((uint8_t)(red * 51),
                                             (uint8_t)(green * 51),
                                             (uint8_t)(blue * 51));
            }
        }
    }
    for (int i = 0; i < 24; ++i) {
        const uint8_t gray = (uint8_t)(i * 11);
        s_palette[index++] = rgb565(gray, gray, gray);
    }
}

static void reset_control_params()
{
    s_value = 0;
    s_param_count = 0;
    memset(s_params, 0, sizeof(s_params));
}

static bool append_decimal(uint8_t byte)
{
    const int digit = byte - (uint8_t)'0';
    if (s_value > (SIXEL_MAX_PARAM - digit) / 10) {
        ESP_LOGW(TAG, "Rejecting oversized numeric Sixel parameter");
        return false;
    }
    s_value = s_value * 10 + digit;
    return true;
}

static void append_param()
{
    if (s_param_count < (int)(sizeof(s_params) / sizeof(s_params[0]))) {
        s_params[s_param_count++] = s_value;
    }
    s_value = 0;
}

static bool finish_raster()
{
    append_param();
    const int width = s_param_count >= 3 ? s_params[2] : 0;
    const int height = s_param_count >= 4 ? s_params[3] : 0;
    if (width > SIXEL_GRAPHICS_WIDTH || height > SIXEL_GRAPHICS_HEIGHT) {
        ESP_LOGW(TAG, "Rejecting raster %dx%d beyond fixed surface", width, height);
        return false;
    }
    if (width > 0) s_raster_width = width;
    if (height > 0) s_raster_height = height;
    return true;
}

static bool finish_repeat()
{
    if (s_value > SIXEL_MAX_REPEAT) {
        ESP_LOGW(TAG, "Rejecting repeat count %d", s_value);
        return false;
    }
    s_repeat = s_value == 0 ? 1 : s_value;
    return true;
}

static bool finish_color()
{
    append_param();
    if (s_param_count <= 0) return true;

    int index = s_params[0];
    if (index < 0) index = 0;
    if (index >= SIXEL_PALETTE_SIZE) index = SIXEL_PALETTE_SIZE - 1;
    s_color_index = index;

    if (s_param_count >= 5) {
        const int mode = s_params[1];
        if (mode == 1) {
            s_palette[index] = hls_to_rgb565(s_params[2], s_params[3], s_params[4]);
        } else if (mode == 2) {
            s_palette[index] = rgb565(percent_to_u8(s_params[2]),
                                      percent_to_u8(s_params[3]),
                                      percent_to_u8(s_params[4]));
        }
    }
    return true;
}

static bool draw_sixel(uint8_t byte)
{
    const int repeat = s_repeat;
    s_repeat = 1;
    if (repeat <= 0 || repeat > SIXEL_MAX_REPEAT ||
        s_pos_x < 0 || s_pos_y < 0 ||
        s_pos_x + repeat > SIXEL_GRAPHICS_WIDTH ||
        s_pos_y + 6 > SIXEL_GRAPHICS_HEIGHT) {
        ESP_LOGW(TAG, "Rejecting Sixel draw position (%d,%d), repeat=%d",
                 s_pos_x, s_pos_y, repeat);
        return false;
    }

    const uint8_t bits = (uint8_t)(byte - (uint8_t)'?');
    const int end_x = s_pos_x + repeat;
    if (end_x > s_draw_width) s_draw_width = end_x;
    if (s_pos_y + 6 > s_draw_height) s_draw_height = s_pos_y + 6;

    if (bits != 0) {
        for (int bit = 0; bit < 6; ++bit) {
            if ((bits & (1U << bit)) == 0) continue;
            const int y = s_pos_y + bit;
            for (int x = s_pos_x; x < end_x; ++x) {
                const size_t pixel = (size_t)y * SIXEL_GRAPHICS_WIDTH + (size_t)x;
                s_stage_indices[pixel] = (uint8_t)s_color_index;
                bit_set(s_stage_mask, pixel);
            }
            s_has_opaque_pixels = true;
        }
    }
    s_pos_x = end_x;
    return true;
}

static bool process_body_byte(uint8_t byte)
{
    // A completed control sequence must reprocess its terminating byte as
    // normal Sixel data, e.g. '#1~' selects color 1 and then draws '~'.
    for (;;) {
        switch (s_state) {
        case parser_state_t::DATA:
            if (byte == (uint8_t)'"') {
                reset_control_params();
                s_state = parser_state_t::RASTER;
                return true;
            }
            if (byte == (uint8_t)'!') {
                reset_control_params();
                s_state = parser_state_t::REPEAT;
                return true;
            }
            if (byte == (uint8_t)'#') {
                reset_control_params();
                s_state = parser_state_t::COLOR;
                return true;
            }
            if (byte == (uint8_t)'$') {
                s_pos_x = 0;
                return true;
            }
            if (byte == (uint8_t)'-') {
                if (s_pos_y > SIXEL_GRAPHICS_HEIGHT - 6) {
                    ESP_LOGW(TAG, "Rejecting Sixel line advance beyond surface");
                    return false;
                }
                s_pos_x = 0;
                s_pos_y += 6;
                return true;
            }
            if (byte >= (uint8_t)'?' && byte <= (uint8_t)'~') {
                return draw_sixel(byte);
            }
            // Other controls in a Sixel body are ignored by this Basic scope.
            return true;

        case parser_state_t::RASTER:
            if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') return append_decimal(byte);
            if (byte == (uint8_t)';') {
                append_param();
                return true;
            }
            if (!finish_raster()) return false;
            s_state = parser_state_t::DATA;
            continue;

        case parser_state_t::REPEAT:
            if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') return append_decimal(byte);
            if (!finish_repeat()) return false;
            s_state = parser_state_t::DATA;
            continue;

        case parser_state_t::COLOR:
            if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') return append_decimal(byte);
            if (byte == (uint8_t)';') {
                append_param();
                return true;
            }
            if (!finish_color()) return false;
            s_state = parser_state_t::DATA;
            continue;
        }
    }
}

static void release_surfaces()
{
    if (s_display_pixels) heap_caps_free(s_display_pixels);
    if (s_display_mask) heap_caps_free(s_display_mask);
    if (s_stage_indices) heap_caps_free(s_stage_indices);
    if (s_stage_mask) heap_caps_free(s_stage_mask);
    s_display_pixels = nullptr;
    s_display_mask = nullptr;
    s_stage_indices = nullptr;
    s_stage_mask = nullptr;
    s_display_opaque_pixels = 0;
    s_display_row_origin = 0;
}

}  // namespace

bool sixel_graphics_init(void)
{
    if (s_display_pixels && s_display_mask && s_stage_indices && s_stage_mask) return true;

    release_surfaces();
    s_display_pixels = (uint16_t *)heap_caps_malloc(PIXEL_COUNT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_display_mask = (uint8_t *)heap_caps_malloc(MASK_BYTES, MALLOC_CAP_SPIRAM);
    s_stage_indices = (uint8_t *)heap_caps_malloc(PIXEL_COUNT, MALLOC_CAP_SPIRAM);
    s_stage_mask = (uint8_t *)heap_caps_malloc(MASK_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_display_pixels || !s_display_mask || !s_stage_indices || !s_stage_mask) {
        ESP_LOGW(TAG, "Sixel disabled: PSRAM allocation failed (%u bytes requested)",
                 (unsigned)(PIXEL_COUNT * 3U + MASK_BYTES * 2U));
        release_surfaces();
        return false;
    }

    memset(s_display_mask, 0, MASK_BYTES);
    memset(s_stage_mask, 0, MASK_BYTES);
    s_display_opaque_pixels = 0;
    s_display_row_origin = 0;
    ESP_LOGI(TAG, "Sixel surfaces ready: %dx%d, %u bytes PSRAM",
             SIXEL_GRAPHICS_WIDTH, SIXEL_GRAPHICS_HEIGHT,
             (unsigned)(PIXEL_COUNT * 3U + MASK_BYTES * 2U));
    return true;
}

bool sixel_graphics_available(void)
{
    return s_display_pixels && s_display_mask && s_stage_indices && s_stage_mask;
}

bool sixel_graphics_has_displayed_pixels(void)
{
    return s_display_opaque_pixels != 0;
}

void sixel_graphics_clear_all(void)
{
    if (!s_display_mask) return;
    memset(s_display_mask, 0, MASK_BYTES);
    s_display_opaque_pixels = 0;
    s_display_row_origin = 0;
}

void sixel_graphics_scroll_up(int pixels)
{
    if (!s_display_mask || s_display_opaque_pixels == 0 || pixels <= 0) return;
    if (pixels >= SIXEL_GRAPHICS_HEIGHT) {
        sixel_graphics_clear_all();
        return;
    }

    s_display_row_origin = (s_display_row_origin + pixels) % SIXEL_GRAPHICS_HEIGHT;
    // The rows that become visible at the logical bottom previously held the
    // clipped-off logical top. Clear only those physical rows; RGB565 values
    // need not be overwritten because their opacity bits are now zero.
    for (int logical_y = SIXEL_GRAPHICS_HEIGHT - pixels;
         logical_y < SIXEL_GRAPHICS_HEIGHT; ++logical_y) {
        clear_display_physical_row(display_physical_row(logical_y));
    }
}

void sixel_graphics_clear_rect(int x, int y, int width, int height)
{
    if (!s_display_mask || s_display_opaque_pixels == 0 || width <= 0 || height <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width;
    int y1 = y + height;
    if (x1 > SIXEL_GRAPHICS_WIDTH) x1 = SIXEL_GRAPHICS_WIDTH;
    if (y1 > SIXEL_GRAPHICS_HEIGHT) y1 = SIXEL_GRAPHICS_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;

    for (int row = y0; row < y1; ++row) {
        const size_t start = (size_t)display_physical_row(row) * SIXEL_GRAPHICS_WIDTH;
        for (int column = x0; column < x1; ++column) {
            const size_t pixel = start + (size_t)column;
            if (bit_get(s_display_mask, pixel)) {
                bit_clear(s_display_mask, pixel);
                --s_display_opaque_pixels;
            }
        }
    }
}

bool sixel_graphics_begin(bool background_transparent)
{
    if (!sixel_graphics_init()) return false;

    memset(s_stage_mask, 0, MASK_BYTES);
    initialise_palette();
    s_active = true;
    s_finished = false;
    s_background_transparent = background_transparent;
    s_stream_bytes = 0;
    s_state = parser_state_t::DATA;
    s_pos_x = 0;
    s_pos_y = 0;
    s_repeat = 1;
    s_color_index = 15;
    s_raster_width = 0;
    s_raster_height = 0;
    s_draw_width = 0;
    s_draw_height = 0;
    s_has_opaque_pixels = false;
    reset_control_params();
    return true;
}

bool sixel_graphics_feed(uint8_t byte)
{
    if (!s_active) return false;
    ++s_stream_bytes;
    if (s_stream_bytes > SIXEL_MAX_STREAM_BYTES) {
        ESP_LOGW(TAG, "Rejecting Sixel DCS longer than %u bytes", (unsigned)SIXEL_MAX_STREAM_BYTES);
        return false;
    }
    return process_body_byte(byte);
}

bool sixel_graphics_finish(sixel_graphics_image_t *image)
{
    if (!s_active || image == nullptr) return false;
    s_active = false;
    s_finished = true;

    // A body ending immediately after a control introducer is malformed. It is
    // safer to reject it than to invent a parameter or commit partial pixels.
    if (s_state != parser_state_t::DATA) {
        ESP_LOGW(TAG, "Rejecting unterminated Sixel control sequence");
        s_finished = false;
        return false;
    }

    const int width = s_raster_width > s_draw_width ? s_raster_width : s_draw_width;
    const int height = s_raster_height > s_draw_height ? s_raster_height : s_draw_height;
    if (width > SIXEL_GRAPHICS_WIDTH || height > SIXEL_GRAPHICS_HEIGHT) {
        s_finished = false;
        return false;
    }

    image->width = width;
    image->height = height;
    image->clear_background = !s_background_transparent;
    image->has_opaque_pixels = s_has_opaque_pixels;
    return true;
}

void sixel_graphics_abort(void)
{
    s_active = false;
    s_finished = false;
    s_repeat = 1;
    s_state = parser_state_t::DATA;
}

bool sixel_graphics_commit(const sixel_graphics_image_t *image, int origin_x, int origin_y)
{
    if (!s_finished || image == nullptr || !sixel_graphics_available()) return false;
    s_finished = false;

    if (image->width <= 0 || image->height <= 0) return false;
    if (image->clear_background) {
        sixel_graphics_clear_rect(origin_x, origin_y, image->width, image->height);
    }

    bool changed = image->clear_background;
    for (int y = 0; y < image->height; ++y) {
        const int display_y = origin_y + y;
        if (display_y < 0 || display_y >= SIXEL_GRAPHICS_HEIGHT) continue;
        const size_t row = (size_t)y * SIXEL_GRAPHICS_WIDTH;
        const size_t display_row =
            (size_t)display_physical_row(display_y) * SIXEL_GRAPHICS_WIDTH;
        for (int x = 0; x < image->width; ++x) {
            const size_t source = row + (size_t)x;
            if (!bit_get(s_stage_mask, source)) continue;
            const int display_x = origin_x + x;
            if (display_x < 0 || display_x >= SIXEL_GRAPHICS_WIDTH) continue;
            const size_t destination = display_row + (size_t)display_x;
            s_display_pixels[destination] = s_palette[s_stage_indices[source]];
            if (!bit_get(s_display_mask, destination)) ++s_display_opaque_pixels;
            bit_set(s_display_mask, destination);
            changed = true;
        }
    }
    return changed;
}

void sixel_graphics_composite_background(uint16_t *destination, int stride,
                                         int x, int y, int width, int height,
                                         uint16_t background)
{
    if (destination == nullptr || stride <= 0 || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; ++row) {
        uint16_t *out = destination + (size_t)row * (size_t)stride;
        const int source_y = y + row;
        for (int column = 0; column < width; ++column) {
            const int source_x = x + column;
            if (s_display_pixels && s_display_mask &&
                source_x >= 0 && source_x < SIXEL_GRAPHICS_WIDTH &&
                source_y >= 0 && source_y < SIXEL_GRAPHICS_HEIGHT) {
                const size_t source =
                    (size_t)display_physical_row(source_y) * SIXEL_GRAPHICS_WIDTH +
                    (size_t)source_x;
                out[column] = bit_get(s_display_mask, source) ? s_display_pixels[source] : background;
            } else {
                out[column] = background;
            }
        }
    }
}
