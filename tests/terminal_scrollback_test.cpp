#include <cassert>
#include <cstdint>

#include "main/terminal.h"

static void feed_byte(uint8_t byte)
{
    vt100_process_byte(byte);
}

static void feed(const char *text)
{
    for (const char *p = text; *p != '\0'; ++p) feed_byte((uint8_t)*p);
}

static void emit_line(char marker)
{
    feed_byte((uint8_t)marker);
    feed("\r\n");
}

static void reset_terminal()
{
    term_set_font_size(14, 28);
    assert(term_scrollback_init());
    term_clear_all();
    assert(term_scrollback_available());
    assert(term_scrollback_history_count() == 0);
    assert(!term_scrollback_is_viewing());
}

int main()
{
    reset_terminal();

    // Twenty-five visible rows plus six extra physical lines leave A..F in
    // history at the default 91x25 geometry.
    for (int i = 0; i < 30; ++i) emit_line((char)('A' + i));
    assert(term_scrollback_history_count() == 6);
    assert(term_scrollback_display_row(0) == term_buffer[0]);

    // Three rows back spans history rows D..F then the live screen's G row.
    assert(term_scrollback_move(3));
    assert(term_scrollback_is_viewing());
    assert(term_scrollback_view_offset() == 3);
    assert(term_scrollback_display_row(0)[0].codepoint == 'D');
    assert(term_scrollback_display_row(2)[0].codepoint == 'F');
    assert(term_scrollback_display_row(3)[0].codepoint == 'G');

    // Appending while viewing must keep the same logical line visible rather
    // than shifting it under the user's finger.
    emit_line('e');
    assert(term_scrollback_history_count() == 7);
    assert(term_scrollback_view_offset() == 4);
    assert(term_scrollback_display_row(0)[0].codepoint == 'D');
    assert(term_scrollback_return_live());
    assert(!term_scrollback_is_viewing());

    // Full-screen scrolls are retained, but a partial VT100 scroll region is
    // screen editing and must not add a history row.
    const int before_partial = term_scrollback_history_count();
    feed("\033[2;24r\033[24;1H\n");
    assert(term_scrollback_history_count() == before_partial);
    feed("\033[r\033[1S");
    assert(term_scrollback_history_count() == before_partial + 1);

    // Even at the top of a full region, CSI M is a delete-line screen edit
    // rather than newly emitted terminal output.
    const int before_delete_line = term_scrollback_history_count();
    feed("\033[1;1H\033[1M");
    assert(term_scrollback_history_count() == before_delete_line);

    // Cell attributes remain in the retained physical row.
    reset_terminal();
    feed("\033[31mR\r\n");
    for (int i = 0; i < 24; ++i) emit_line('x');
    assert(term_scrollback_history_count() == 1);
    assert(term_scrollback_move(1));
    const TermCell *red_row = term_scrollback_display_row(0);
    assert(red_row != nullptr);
    assert(red_row[0].codepoint == 'R');
    assert(red_row[0].fg == 1);
    assert(term_scrollback_return_live());

    // CSI 3 J removes history without altering the visible terminal contents.
    const uint32_t live_cell_before_clear = term_buffer[0][0].codepoint;
    feed("\033[3J");
    assert(term_scrollback_history_count() == 0);
    assert(!term_scrollback_is_viewing());
    assert(term_buffer[0][0].codepoint == live_cell_before_clear);

    // The fixed ring drops only the oldest rows once it is full and remains
    // safe to inspect at the oldest retained edge.
    reset_terminal();
    for (int i = 0; i < TERM_SCROLLBACK_MAX_LINES + TERM_ROWS + 16; ++i) {
        emit_line((char)('0' + (i % 10)));
    }
    assert(term_scrollback_history_count() == TERM_SCROLLBACK_MAX_LINES);
    assert(term_scrollback_move(TERM_SCROLLBACK_MAX_LINES * 2));
    assert(term_scrollback_view_offset() == TERM_SCROLLBACK_MAX_LINES);
    assert(term_scrollback_display_row(0) != nullptr);
    assert(term_scrollback_display_row(TERM_ROWS - 1) != nullptr);

    // Font changes intentionally discard history because physical rows from a
    // different terminal width cannot be faithfully reflowed.
    term_set_font_size(8, 16);
    assert(term_scrollback_history_count() == 0);
    assert(!term_scrollback_is_viewing());

    return 0;
}
