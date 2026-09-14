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
    ENTER,        /**< Commit direct kana and append terminal CR. */
    COMMIT,       /**< Commit locally without appending a terminal CR (Ctrl+J). */
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

/** Direct kana output mode, following the standard SKK q toggle. */
enum class ime_kana_mode_t {
    HIRAGANA,
    KATAKANA,
};

/** Punctuation conversion applied during Japanese input. */
enum class ime_punctuation_style_t {
    JAPANESE,
    ASCII,
    FULLWIDTH,
};

/**
 * @brief Small, bounded romaji SKK input engine.
 *
 * Lowercase romaji creates direct hiragana or katakana text.  An uppercase
 * initial starts SKK conversion; a subsequent uppercase initial begins an
 * okurigana segment (`MiRu` -> dictionary key `みr` -> `見る`).  `q` toggles
 * direct hiragana/katakana input while there is no unfinished composition.
 * The class never accesses LVGL, ESP-IDF or serial transport directly.
 */
class ime_skk_t {
public:
    static constexpr size_t MAX_READING_BYTES = 96;
    static constexpr size_t MAX_DICT_LINE_BYTES = 512;
    static constexpr size_t MAX_CANDIDATES = 16;

    ime_skk_t();

    /** Reset unfinished composition and candidates; retain the kana mode. */
    void reset();

    /** Configure the read-only UTF-8/LF system SKK dictionary path. Passing NULL disables it. */
    void set_dictionary_path(const char *path);

    /** Configure the writable UTF-8/LF user dictionary path. Passing NULL disables learning. */
    void set_user_dictionary_path(const char *path);

    /** Set the punctuation conversion profile used in Japanese input mode. */
    void set_punctuation_style(ime_punctuation_style_t style);

    /** Returns true when the configured system dictionary can be opened for reading. */
    bool dictionary_available() const;

    /** Returns true when a user dictionary file currently exists. */
    bool user_dictionary_available() const;

    /** Process a printable ASCII/UTF-8 keyboard event in Japanese mode. */
    ime_result_t input_text(const char *text, size_t len);

    /** Process a special keyboard key in Japanese mode. */
    ime_result_t input_key(ime_key_t key);

    /** Current state for the UI. */
    ime_state_t state() const;

    /** Current direct kana output mode. */
    ime_kana_mode_t kana_mode() const;

    /** True while an SKK conversion reading or candidate is active. */
    bool is_conversion_active() const;

    /** True while entering an SKK okurigana tail. */
    bool is_okuri_active() const;

    /** True in temporary ASCII entry mode, entered by l with no preedit. */
    bool is_ascii_mode() const;

    /** Text to show as preedit. Direct katakana is rendered in katakana. */
    std::string preedit_text() const;

    /** Number of currently available conversion candidates. */
    size_t candidate_count() const;

    /** Index of the highlighted candidate, or 0 with no candidates. */
    size_t candidate_index() const;

    /** Candidate stem at index, or an empty string for an out-of-range index. */
    const std::string &candidate_at(size_t index) const;

    /** Okurigana tail for UI display and candidate confirmation. */
    const std::string &okuri_text() const;

private:
    std::string s_dictionary_path;
    std::string s_user_dictionary_path;

    // Internal readings are always hiragana.  Katakana conversion is applied
    // only to direct (non-SKK-conversion) display and commit output.
    std::string s_kana;
    std::string s_okuri_kana;
    std::string s_romaji;
    char        s_okuri_initial = '\0';
    bool        s_conversion_active = false;
    bool        s_okuri_active = false;
    ime_kana_mode_t s_kana_mode = ime_kana_mode_t::HIRAGANA;
    ime_punctuation_style_t s_punctuation_style = ime_punctuation_style_t::JAPANESE;
    bool        s_ascii_mode = false;
    bool        s_z_prefix = false;

    std::array<std::string, MAX_CANDIDATES> s_candidates;
    size_t s_candidate_count = 0;
    size_t s_candidate_index = 0;
    ime_state_t s_state = ime_state_t::IDLE;

    void clear_candidates();
    void clear_composition();
    void update_state();
    void append_hiragana(const char *utf8);
    void flush_romaji(bool literal_fallback);
    void process_romaji();
    void erase_last_preedit();
    bool search_dictionary();
    bool search_dictionary_key(const std::string &path, const std::string &key);
    bool search_dictionary_okuri_family(const std::string &path, const std::string &reading);
    bool append_candidate(const char *candidate, size_t len);
    bool learn_current_candidate();
    bool update_user_dictionary(const std::string &key, const std::string &candidate);
    std::string current_dictionary_key() const;
    std::string direct_commit_text() const;
    std::string current_candidate_commit() const;
};
