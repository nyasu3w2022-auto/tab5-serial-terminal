#pragma once
/*
 * sixel_graphics.h — Bounded, streaming DEC Sixel image decoder and
 *                    terminal graphics surface.
 *
 * The decoder deliberately supports only the fixed TAB5 logical terminal
 * surface. It never allocates from values supplied by the serial peer.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>

// Logical TAB5 terminal drawing area. The 20-pixel status bar is deliberately
// outside of the Sixel surface.
#define SIXEL_GRAPHICS_WIDTH  1280
#define SIXEL_GRAPHICS_HEIGHT 700

// The encoded DCS body is parsed in-place, but this cap prevents a peer that
// never sends ST from monopolising the RX parser indefinitely.
#define SIXEL_MAX_STREAM_BYTES (1024U * 1024U)

typedef struct {
    int width;
    int height;
    bool clear_background;
    bool has_opaque_pixels;
} sixel_graphics_image_t;

/** Allocate the fixed PSRAM graphics and staging surfaces. Idempotent. */
bool sixel_graphics_init(void);

/** Return true only when the fixed graphics/staging surfaces are usable. */
bool sixel_graphics_available(void);

/** Return true when the live graphics plane contains one or more pixels. */
bool sixel_graphics_has_displayed_pixels(void);

/** Clear every displayed Sixel pixel. Staging state is left untouched. */
void sixel_graphics_clear_all(void);

/** Clear a clipped logical-pixel rectangle from the displayed graphics plane. */
void sixel_graphics_clear_rect(int x, int y, int width, int height);

/**
 * Begin one Sixel DCS body. background_transparent corresponds to P2=1;
 * false (P2=0/2) means the declared/drawn image rectangle clears earlier
 * graphics before the decoded opaque pixels are committed.
 */
bool sixel_graphics_begin(bool background_transparent);

/** Feed one byte from the already-recognised Sixel body (after final 'q'). */
bool sixel_graphics_feed(uint8_t byte);

/**
 * Finish the current body without altering the displayed plane. The caller
 * chooses its final origin after applying DECSDM scrolling policy, then calls
 * sixel_graphics_commit().
 */
bool sixel_graphics_finish(sixel_graphics_image_t *image);

/** Discard an incomplete or invalid Sixel body without changing displayed pixels. */
void sixel_graphics_abort(void);

/** Commit the last successfully finished staged image at a logical-pixel origin. */
bool sixel_graphics_commit(const sixel_graphics_image_t *image, int origin_x, int origin_y);

/**
 * Fill a destination RGB565 rectangle with either the displayed Sixel pixel
 * or the supplied terminal-cell background. The destination has stride pixels
 * per scanline. Used by the terminal's existing row canvases.
 */
void sixel_graphics_composite_background(uint16_t *destination, int stride,
                                         int x, int y, int width, int height,
                                         uint16_t background);
