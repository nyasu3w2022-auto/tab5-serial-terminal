#pragma once
/*
 * ime_skk.h — Local romaji / SKK Japanese input engine for TAB5.
 *
 * This module intentionally depends only on the C++ standard library so that
 * its conversion and dictionary behaviour can be unit-tested on a host PC.
 * UI rendering and serial transmission are handled by other modules.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include <array>
#include <string>

/** Result produced by an IME key operation. */
struct ime_result_t {
    bool        consumed = false;  /**< true: do not pass the original key to remote */
    bool        changed  = false;  /**< true: redraw the local IME overlay */
    std::string commit;            /**< UTF-8 text to transmit after conversion */
};

/** Non-printable key operations understood by the IME. */
enum class ime_key_t {
    SPACE,
    ENTER,
    ESCAPE,
    BACKSPACE,
    LEFT,
    RIGHT,
};

/** Current local composition state. */
enum class ime_state_t {
    IDLE,
    COMPOSING,
    CANDIDATE,
};

/**
 * @brief Small, bounded SKK-style Japanese input engine.
 *
 * Printable ASCII is accepted as romaji and converted to UTF-8 hiragana.
 * `SPACE` searches the configured UTF-8/LF SKK dictionary, while `ENTER`
 * commits the selected candidate or the hiragana preedit text.  The class
 * never accesses LVGL, ESP-IDF or the serial transport directly.
 */
class ime_skk_t {
public:
    static constexpr size_t MAX_READING_BYTES = 96;
    static constexpr size_t MAX_DICT_LINE_BYTES = 512;
    static constexpr size_t MAX_CANDIDATES = 16;

    ime_skk_t();

    /** Reset unfinished composition and candidate selection. */
    void reset();

    /** Configure the UTF-8/LF SKK dictionary path. Passing NULL disables it. */
    void set_dictionary_path(const char *path);

    /** Returns true when the configured dictionary can be opened for reading. */
    bool dictionary_available() const;

    /** Process a printable ASCII/UTF-8 keyboard event in Japanese mode. */
    ime_result_t input_text(const char *text, size_t len);

    /** Process a special keyboard key in Japanese mode. */
    ime_result_t input_key(ime_key_t key);

    /** Current state for the UI. */
    ime_state_t state() const;

    /** Hiragana and incomplete romaji to show as preedit text. */
    std::string preedit_text() const;

    /** Number of currently available conversion candidates. */
    size_t candidate_count() const;

    /** Index of the highlighted candidate, or 0 with no candidates. */
    size_t candidate_index() const;

    /** Candidate at index, or an empty string for an out-of-range index. */
    const std::string &candidate_at(size_t index) const;

private:
    std::string s_dictionary_path;
    std::string s_kana;
    std::string s_romaji;
    std::array<std::string, MAX_CANDIDATES> s_candidates;
    size_t s_candidate_count = 0;
    size_t s_candidate_index = 0;
    ime_state_t s_state = ime_state_t::IDLE;

    void clear_candidates();
    void update_state();
    void append_hiragana(const char *utf8);
    void flush_romaji(bool literal_fallback);
    void process_romaji();
    void erase_last_preedit();
    bool search_dictionary();
};
