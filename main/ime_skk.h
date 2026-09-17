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
#include <stdint.h>

#include <array>
#include <string>
#include <vector>

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

/** Persistence policy for automatically learned candidate priority. */
enum class ime_learning_mode_t {
    OFF,       /**< Do not change candidate priority after confirmation. */
    DEFERRED,  /**< Keep changes in RAM; application flushes them in batches. */
    MANUAL,    /**< Keep changes in RAM until the user explicitly saves. */
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

    /** Configure the read-only UTF-8/LF supplementary dictionary path. */
    void set_supplement_dictionary_path(const char *path);

    /** Add a candidate to the user dictionary and make it the first candidate for this key. */
    bool register_user_candidate(const std::string &reading, char okuri_initial,
                                 const std::string &candidate);

    /** Remove one exact candidate from the user dictionary; empty entries are removed. */
    bool remove_user_candidate(const std::string &reading, char okuri_initial,
                               const std::string &candidate);

    /** Set the punctuation conversion profile used in Japanese input mode. */
    void set_punctuation_style(ime_punctuation_style_t style);

    /** Configure how automatically learned candidate priority is persisted. */
    void set_learning_mode(ime_learning_mode_t mode);

    /** Current candidate-priority persistence policy. */
    ime_learning_mode_t learning_mode() const;

    /** Number of learned priority changes held in RAM but not yet saved. */
    size_t pending_learning_count() const;

    /** Changes whenever the RAM-pending learning state is modified. */
    uint32_t learning_generation() const;

    /** Save all pending learned candidate priorities to the user dictionary. */
    bool flush_pending_learning();

    /** Returns true when the configured system dictionary can be opened for reading. */
    bool dictionary_available() const;

    /** Returns true when a user dictionary file currently exists. */
    bool user_dictionary_available() const;

    /** Returns true when a writable user-dictionary path was configured. */
    bool user_dictionary_writable() const;

    /** Current writable user-dictionary path, empty when dictionary updates are disabled. */
    const std::string &user_dictionary_path() const;

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
    std::string s_supplement_dictionary_path;
    std::string s_user_dictionary_path;

    struct learning_entry_t {
        std::string key;
        std::string candidate;
    };
    std::vector<learning_entry_t> s_pending_learning;
    ime_learning_mode_t s_learning_mode = ime_learning_mode_t::DEFERRED;
    uint32_t s_learning_generation = 0;

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
    bool queue_learned_candidate(const std::string &key, const std::string &candidate);
    void apply_pending_learning(const std::string &key);
    bool user_candidate_is_first(const std::string &key, const std::string &candidate) const;
    bool update_user_dictionary(const std::string &key, const std::string &candidate);
    bool update_user_dictionary_batch(const std::vector<learning_entry_t> &updates);
    bool remove_user_dictionary_candidate(const std::string &key, const std::string &candidate);
    static bool make_dictionary_key(const std::string &reading, char okuri_initial, std::string *key);
    std::string current_dictionary_key() const;
    std::string direct_commit_text() const;
    std::string current_candidate_commit() const;
};
