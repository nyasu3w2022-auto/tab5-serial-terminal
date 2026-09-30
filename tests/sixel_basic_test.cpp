#include <cassert>
#include <cstdint>
#include <cstring>

#include "main/sixel_graphics.h"
#include "main/terminal.h"

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((uint16_t)(red & 0xf8U) << 8U) |
                      ((uint16_t)(green & 0xfcU) << 3U) |
                      ((uint16_t)blue >> 3U));
}

static void feed_byte(uint8_t byte)
{
    vt100_process_byte(byte);
}

static void feed(const char *text)
{
    for (const char *p = text; *p != '\0'; ++p) feed_byte((uint8_t)*p);
}

static char s_tx_response[32] = {};
static size_t s_tx_response_length = 0;

static void capture_tx(const uint8_t *data, size_t length)
{
    assert(length < sizeof(s_tx_response));
    memcpy(s_tx_response, data, length);
    s_tx_response[length] = '\0';
    s_tx_response_length = length;
}

static void reset_terminal()
{
    term_set_font_size(14, 28);
    term_clear_all();
    assert(sixel_graphics_init());
    assert(!sixel_graphics_has_displayed_pixels());
}

static uint16_t pixel_at(int x, int y, uint16_t background = 0x39e7)
{
    uint16_t pixel = 0;
    sixel_graphics_composite_background(&pixel, 1, x, y, 1, 1, background);
    return pixel;
}

int main()
{
    reset_terminal();

    // Primary DA declares Sixel only after the fixed graphics surface has
    // allocated successfully, allowing a peer to feature-detect safely.
    vt100_set_tx_cb(capture_tx);
    feed("\x1b[c");
    assert(s_tx_response_length == strlen("\x1b[?1;4c"));
    assert(strcmp(s_tx_response, "\x1b[?1;4c") == 0);
    vt100_set_tx_cb(nullptr);

    // 7-bit DCS/ST, an RGB palette definition and a six-dot column. The
    // terminal cursor moves one physical terminal row after a 6-pixel image
    // in the default DECSDM scrolling mode.
    feed("\x1bP0;0;0q#1;2;100;0;0!3~\x1b\\");
    const uint16_t red = rgb565(255, 0, 0);
    assert(sixel_graphics_has_displayed_pixels());
    for (int y = 0; y < 6; ++y) assert(pixel_at(0, y) == red);
    assert(pixel_at(0, 6) == 0x39e7);
    assert(cursor_row == 1);
    assert(cursor_col == 0);

    // P2=1 keeps the existing image outside newly opaque dots. Fourteen empty
    // dots advance the Sixel cursor, so green is drawn at x=14 and x=15 while
    // the original red at x=2 remains untouched.
    feed("\x1b[H");
    feed("\x1bP0;1;0q#2;2;0;100;0!14?!2@\x1b\\");
    const uint16_t green = rgb565(0, 255, 0);
    assert(pixel_at(14, 0) == green);
    assert(pixel_at(15, 0) == green);
    assert(pixel_at(2, 0) == red);

    // An opaque image clears only its own declared/drawn rectangle before it
    // commits. This image changes x=0 and leaves x=1 from the transparent one.
    feed("\x1b[H");
    feed("\x1bP0;0;0q#3;2;0;0;100@\x1b\\");
    const uint16_t blue = rgb565(0, 0, 255);
    assert(pixel_at(0, 0) == blue);
    assert(pixel_at(14, 0) == green);

    // A following terminal character is above the image plane. The parser
    // clears its full cell rectangle so the text background cannot mix with
    // stale Sixel pixels.
    feed("\x1b[H");
    feed("A");
    assert(pixel_at(0, 0) == 0x39e7);
    assert(pixel_at(13, 27) == 0x39e7);
    assert(pixel_at(14, 0) == green);

    // 8-bit DCS/ST uses exactly the same bounded decoder path.
    reset_terminal();
    const uint8_t eight_bit_sixel[] = {
        0x90, '0', ';', '0', ';', '0', 'q',
        '#', '4', ';', '2', ';', '1', '0', '0', ';', '1', '0', '0', ';', '0',
        '@', 0x9c,
    };
    for (uint8_t byte : eight_bit_sixel) feed_byte(byte);
    assert(sixel_graphics_has_displayed_pixels());
    assert(pixel_at(0, 0) == rgb565(255, 255, 0));

    // Excessive repeat counts abort the whole DCS. A following normal byte is
    // parsed as text only after ST resynchronises the parser.
    reset_terminal();
    feed("\x1bP0;0;0q!1281~\x1b\\Z");
    assert(!sixel_graphics_has_displayed_pixels());
    assert(term_buffer[0][0].codepoint == 'Z');

    // An invalid second DCS cannot damage already committed graphics. The
    // parser discards bytes through ST and resynchronises for normal text.
    reset_terminal();
    feed("\x1bP0;0;0q#1~\x1b\\");
    const uint16_t default_palette_one = pixel_at(0, 0);
    feed("\x1bP0;0;0q!1281~\x1b\\Q");
    assert(pixel_at(0, 0) == default_palette_one);
    assert(term_buffer[1][0].codepoint == 'Q');

    // Explicit terminal clearing removes live pixels, while CSI 3 J clears
    // text history only and therefore deliberately leaves an image unchanged.
    reset_terminal();
    feed("\x1bP0;0;0q#1~\x1b\\");
    assert(sixel_graphics_has_displayed_pixels());
    feed("\x1b[3J");
    assert(sixel_graphics_has_displayed_pixels());
    feed("\x1b[2J");
    assert(!sixel_graphics_has_displayed_pixels());
    feed("\x1bP0;0;0q#1~\x1b\\");
    assert(sixel_graphics_has_displayed_pixels());
    // Any VT100 scroll drops the live graphic by the documented text-only
    // history policy.
    feed("\x1b[1S");
    assert(!sixel_graphics_has_displayed_pixels());
    term_clear_all();
    assert(!sixel_graphics_has_displayed_pixels());

    // A transport overflow may remove the DCS ST. The recovery path discards
    // queued body bytes through a later ST, then permits ordinary text again;
    // the partial image must never be committed.
    reset_terminal();
    feed("\x1bP0;0;0q#1~");
    assert(vt100_is_processing_control_string());
    vt100_recover_from_rx_overflow();
    assert(vt100_is_processing_control_string());
    feed("ignored-image-body\x1b\\R");
    assert(!vt100_is_processing_control_string());
    assert(!sixel_graphics_has_displayed_pixels());
    assert(term_buffer[0][0].codepoint == 'R');

    // The local Ctrl+Alt+C recovery path uses the stronger abort operation and
    // therefore returns to ordinary parsing even when the terminating ST itself
    // never arrives.
    reset_terminal();
    feed("\x1bP0;0;0q#1~");
    assert(vt100_is_processing_control_string());
    vt100_abort_control_sequence();
    assert(!vt100_is_processing_control_string());
    feed("C");
    assert(term_buffer[0][0].codepoint == 'C');

    return 0;
}
