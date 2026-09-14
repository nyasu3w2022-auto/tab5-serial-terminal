/*
 * ime_skk.cpp — Local romaji / SKK Japanese input engine for TAB5.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ime_skk.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

struct romaji_entry_t {
    const char *roma;
    const char *kana;
};

// Longest alternatives are considered first.  The result table is hiragana;
// direct katakana input is generated from this UTF-8 output afterwards.
static const romaji_entry_t ROMAJI_TABLE[] = {
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
    {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"},
    {"sya", "しゃ"}, {"syu", "しゅ"}, {"syo", "しょ"},
    {"zya", "じゃ"}, {"zyu", "じゅ"}, {"zyo", "じょ"},
    {"ja",  "じゃ"}, {"ju",  "じゅ"}, {"jo",  "じょ"},
    {"jya", "じゃ"}, {"jyu", "じゅ"}, {"jyo", "じょ"},
    {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"},
    {"cya", "ちゃ"}, {"cyu", "ちゅ"}, {"cyo", "ちょ"},
    {"tya", "ちゃ"}, {"tyu", "ちゅ"}, {"tyo", "ちょ"},
    {"dya", "ぢゃ"}, {"dyu", "ぢゅ"}, {"dyo", "ぢょ"},
    {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
    {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
    {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
    {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
    {"fya", "ふゃ"}, {"fyu", "ふゅ"}, {"fyo", "ふょ"},
    {"fa",  "ふぁ"}, {"fi",  "ふぃ"}, {"fe",  "ふぇ"}, {"fo",  "ふぉ"},
    {"tsa", "つぁ"}, {"tsi", "つぃ"}, {"tse", "つぇ"}, {"tso", "つぉ"},
    {"thi", "てぃ"}, {"dhi", "でぃ"}, {"twu", "とぅ"}, {"dwu", "どぅ"},
    {"she", "しぇ"}, {"je",  "じぇ"}, {"che", "ちぇ"},
    {"xya", "ゃ"}, {"xyu", "ゅ"}, {"xyo", "ょ"},
    {"lya", "ゃ"}, {"lyu", "ゅ"}, {"lyo", "ょ"},
    {"xtsu", "っ"}, {"ltsu", "っ"}, {"xtu", "っ"}, {"ltu", "っ"},
    {"xwa", "ゎ"}, {"lwa", "ゎ"},
    {"xa",  "ぁ"}, {"xi",  "ぃ"}, {"xu",  "ぅ"}, {"xe",  "ぇ"}, {"xo", "ぉ"},
    {"la",  "ぁ"}, {"li",  "ぃ"}, {"lu",  "ぅ"}, {"le",  "ぇ"}, {"lo", "ぉ"},
    {"ka",  "か"}, {"ki",  "き"}, {"ku", "く"}, {"ke", "け"}, {"ko", "こ"},
    {"ga",  "が"}, {"gi",  "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"}, {"go", "ご"},
    {"sa",  "さ"}, {"shi", "し"}, {"si", "し"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"za",  "ざ"}, {"zi",  "じ"}, {"ji", "じ"}, {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"},
    {"ta",  "た"}, {"chi", "ち"}, {"ti", "ち"}, {"tsu", "つ"}, {"tu", "つ"}, {"te", "て"}, {"to", "と"},
    {"da",  "だ"}, {"di",  "ぢ"}, {"du", "づ"}, {"de", "で"}, {"do", "ど"},
    {"na",  "な"}, {"ni",  "に"}, {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"},
    {"ha",  "は"}, {"hi",  "ひ"}, {"fu", "ふ"}, {"hu", "ふ"}, {"he", "へ"}, {"ho", "ほ"},
    {"ba",  "ば"}, {"bi",  "び"}, {"bu", "ぶ"}, {"be", "べ"}, {"bo", "ぼ"},
    {"pa",  "ぱ"}, {"pi",  "ぴ"}, {"pu", "ぷ"}, {"pe", "ぺ"}, {"po", "ぽ"},
    {"ma",  "ま"}, {"mi",  "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},
    {"ya",  "や"}, {"yu",  "ゆ"}, {"yo", "よ"},
    {"ra",  "ら"}, {"ri",  "り"}, {"ru", "る"}, {"re", "れ"}, {"ro", "ろ"},
    {"wa",  "わ"}, {"wi",  "うぃ"}, {"we", "うぇ"}, {"wo", "を"},
    {"qa",  "くぁ"}, {"qi",  "くぃ"}, {"qu", "く"}, {"qe", "くぇ"}, {"qo", "くぉ"},
    {"va",  "ゔぁ"}, {"vi",  "ゔぃ"}, {"vu", "ゔ"}, {"ve", "ゔぇ"}, {"vo", "ゔぉ"},
    {"a",   "あ"}, {"i",   "い"}, {"u",  "う"}, {"e", "え"}, {"o", "お"},
};

static bool is_consonant(char c)
{
    return (c >= 'a' && c <= 'z') && !strchr("aeioun", c);
}

static bool starts_with(const std::string &value, const char *prefix)
{
    size_t n = strlen(prefix);
    return value.size() >= n && value.compare(0, n, prefix) == 0;
}

static bool is_prefix_of_romaji_entry(const std::string &value)
{
    for (const auto &entry : ROMAJI_TABLE) {
        size_t entry_len = strlen(entry.roma);
        if (value.size() <= entry_len && strncmp(entry.roma, value.c_str(), value.size()) == 0) {
            return true;
        }
    }
    return false;
}

static const romaji_entry_t *find_complete_entry(const std::string &value)
{
    const romaji_entry_t *best = nullptr;
    size_t best_len = 0;
    for (const auto &entry : ROMAJI_TABLE) {
        size_t len = strlen(entry.roma);
        if (len > best_len && starts_with(value, entry.roma)) {
            best = &entry;
            best_len = len;
        }
    }
    return best;
}

static void erase_last_utf8(std::string *text)
{
    if (text == nullptr || text->empty()) return;
    size_t pos = text->size() - 1;
    while (pos > 0 && (((uint8_t)(*text)[pos] & 0xC0) == 0x80)) pos--;
    text->erase(pos);
}

static void append_utf8_codepoint(std::string *out, uint32_t cp)
{
    if (out == nullptr) return;
    if (cp <= 0x7F) {
        out->push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out->push_back((char)(0xC0 | (cp >> 6)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out->push_back((char)(0xE0 | (cp >> 12)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static std::string hiragana_to_katakana(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        unsigned char c = (unsigned char)text[i];
        if (i + 2 < text.size() && c == 0xE3 &&
            (((unsigned char)text[i + 1] & 0xC0) == 0x80) &&
            (((unsigned char)text[i + 2] & 0xC0) == 0x80)) {
            uint32_t cp = ((uint32_t)(c & 0x0F) << 12) |
                          ((uint32_t)((unsigned char)text[i + 1] & 0x3F) << 6) |
                          ((uint32_t)((unsigned char)text[i + 2] & 0x3F));
            if (cp >= 0x3041 && cp <= 0x3096) {
                append_utf8_codepoint(&out, cp + 0x60);
            } else {
                out.append(text, i, 3);
            }
            i += 3;
        } else {
            out.push_back((char)c);
            i++;
        }
    }
    return out;
}

static const std::string EMPTY_STRING;

static const char *z_symbol(char input)
{
    switch (input) {
    case '/': return "・";
    case '-': return "〜";
    case '.': return "…";
    case ',': return "‥";
    case '[': return "『";
    case ']': return "』";
    case '{': return "【";
    case '}': return "】";
    case '!': return "！";
    case '?': return "？";
    default:  return nullptr;
    }
}

static const char *punctuation_text(ime_punctuation_style_t style, char input)
{
    if (style == ime_punctuation_style_t::ASCII) {
        switch (input) {
        case '.': return ".";
        case ',': return ",";
        case '-': return "-";
        default: return nullptr;
        }
    }
    if (style == ime_punctuation_style_t::FULLWIDTH) {
        switch (input) {
        case '.': return "．";
        case ',': return "，";
        case '-': return "－";
        default: return nullptr;
        }
    }
    switch (input) {
    case '.': return "。";
    case ',': return "、";
    case '-': return "ー";
    default: return nullptr;
    }
}

}  // namespace

ime_skk_t::ime_skk_t()
{
    reset();
}

void ime_skk_t::reset()
{
    clear_composition();
    clear_candidates();
    s_ascii_mode = false;
    s_z_prefix = false;
    s_state = ime_state_t::IDLE;
}

void ime_skk_t::set_dictionary_path(const char *path)
{
    s_dictionary_path = path ? path : "";
}

void ime_skk_t::set_user_dictionary_path(const char *path)
{
    s_user_dictionary_path = path ? path : "";
}

void ime_skk_t::set_punctuation_style(ime_punctuation_style_t style)
{
    s_punctuation_style = style;
}

bool ime_skk_t::dictionary_available() const
{
    if (s_dictionary_path.empty()) return false;
    FILE *fp = fopen(s_dictionary_path.c_str(), "rb");
    if (fp == nullptr) return false;
    fclose(fp);
    return true;
}

bool ime_skk_t::user_dictionary_available() const
{
    if (s_user_dictionary_path.empty()) return false;
    FILE *fp = fopen(s_user_dictionary_path.c_str(), "rb");
    if (fp == nullptr) return false;
    fclose(fp);
    return true;
}

ime_state_t ime_skk_t::state() const
{
    return s_state;
}

ime_kana_mode_t ime_skk_t::kana_mode() const
{
    return s_kana_mode;
}

bool ime_skk_t::is_conversion_active() const
{
    return s_conversion_active;
}

bool ime_skk_t::is_okuri_active() const
{
    return s_okuri_active;
}

bool ime_skk_t::is_ascii_mode() const
{
    return s_ascii_mode;
}

std::string ime_skk_t::preedit_text() const
{
    if (s_ascii_mode) return "ASCII";
    if (s_conversion_active) {
        // Conversion state is identified by the overlay status line.  Keep
        // this text ASCII-marker-free because the terminal CJK font does not
        // provide every SKK-specific symbol on every configured font size.
        std::string text = s_kana;
        if (s_okuri_active) {
            text += " / ";
            text += s_okuri_kana;
        }
        text += s_romaji;
        return text;
    }

    std::string text = s_kana;
    text += s_romaji;
    return (s_kana_mode == ime_kana_mode_t::KATAKANA) ? hiragana_to_katakana(text) : text;
}

size_t ime_skk_t::candidate_count() const
{
    return s_candidate_count;
}

size_t ime_skk_t::candidate_index() const
{
    return s_candidate_index;
}

const std::string &ime_skk_t::candidate_at(size_t index) const
{
    return (index < s_candidate_count) ? s_candidates[index] : EMPTY_STRING;
}

const std::string &ime_skk_t::okuri_text() const
{
    return s_okuri_kana;
}

void ime_skk_t::clear_candidates()
{
    for (auto &candidate : s_candidates) candidate.clear();
    s_candidate_count = 0;
    s_candidate_index = 0;
}

void ime_skk_t::clear_composition()
{
    s_kana.clear();
    s_okuri_kana.clear();
    s_romaji.clear();
    s_okuri_initial = '\0';
    s_conversion_active = false;
    s_okuri_active = false;
    s_z_prefix = false;
}

void ime_skk_t::update_state()
{
    if (s_candidate_count > 0) {
        s_state = ime_state_t::CANDIDATE;
    } else if (s_ascii_mode || s_conversion_active || !s_kana.empty() || !s_okuri_kana.empty() || !s_romaji.empty() || s_z_prefix) {
        s_state = ime_state_t::COMPOSING;
    } else {
        s_state = ime_state_t::IDLE;
    }
}

void ime_skk_t::append_hiragana(const char *utf8)
{
    if (utf8 == nullptr) return;
    std::string *target = s_okuri_active ? &s_okuri_kana : &s_kana;
    size_t len = strlen(utf8);
    size_t used = s_kana.size() + s_okuri_kana.size();
    if (used + len <= MAX_READING_BYTES) target->append(utf8, len);
}

void ime_skk_t::flush_romaji(bool literal_fallback)
{
    process_romaji();
    if (literal_fallback && !s_romaji.empty()) {
        while (!s_romaji.empty()) {
            if (s_romaji[0] == 'n') {
                append_hiragana("ん");
                s_romaji.erase(0, s_romaji.size() >= 2 && s_romaji[1] == 'n' ? 2 : 1);
            } else {
                append_hiragana(std::string(1, s_romaji[0]).c_str());
                s_romaji.erase(0, 1);
            }
        }
    }
    update_state();
}

void ime_skk_t::process_romaji()
{
    while (!s_romaji.empty()) {
        if (s_romaji.size() >= 2 && s_romaji[0] == s_romaji[1] &&
            is_consonant(s_romaji[0]) && s_romaji[0] != 'n') {
            append_hiragana("っ");
            s_romaji.erase(0, 1);
            continue;
        }

        if (starts_with(s_romaji, "tch")) {
            append_hiragana("っ");
            s_romaji.erase(0, 1);
            continue;
        }

        if (s_romaji[0] == 'n') {
            if (s_romaji.size() >= 2 && s_romaji[1] == '\'') {
                append_hiragana("ん");
                s_romaji.erase(0, 2);
                continue;
            }
            if (s_romaji.size() >= 2 && s_romaji[1] == 'n') {
                if (s_romaji.size() < 3) return;
                append_hiragana("ん");
                s_romaji.erase(0, strchr("aiueoy", s_romaji[2]) != nullptr ? 1 : 2);
                continue;
            }
            if (s_romaji.size() >= 2 && s_romaji[1] != 'a' && s_romaji[1] != 'i' &&
                s_romaji[1] != 'u' && s_romaji[1] != 'e' && s_romaji[1] != 'o' &&
                s_romaji[1] != 'y') {
                append_hiragana("ん");
                s_romaji.erase(0, 1);
                continue;
            }
        }

        const romaji_entry_t *entry = find_complete_entry(s_romaji);
        if (entry != nullptr) {
            size_t entry_len = strlen(entry->roma);
            bool longer_prefix = false;
            for (const auto &candidate : ROMAJI_TABLE) {
                size_t candidate_len = strlen(candidate.roma);
                if (candidate_len > entry_len && starts_with(std::string(candidate.roma), entry->roma) &&
                    s_romaji.size() < candidate_len &&
                    strncmp(candidate.roma, s_romaji.c_str(), s_romaji.size()) == 0) {
                    longer_prefix = true;
                    break;
                }
            }
            if (!longer_prefix) {
                append_hiragana(entry->kana);
                s_romaji.erase(0, entry_len);
                continue;
            }
        }

        if (is_prefix_of_romaji_entry(s_romaji) || s_romaji == "n") return;

        append_hiragana(std::string(1, s_romaji[0]).c_str());
        s_romaji.erase(0, 1);
    }
}

void ime_skk_t::erase_last_preedit()
{
    clear_candidates();
    if (!s_romaji.empty()) {
        s_romaji.pop_back();
    } else if (s_okuri_active) {
        erase_last_utf8(&s_okuri_kana);
        if (s_okuri_kana.empty()) {
            s_okuri_active = false;
            s_okuri_initial = '\0';
        }
    } else {
        erase_last_utf8(&s_kana);
    }
    update_state();
}

bool ime_skk_t::search_dictionary()
{
    clear_candidates();
    if (s_kana.empty()) return false;

    // A user dictionary is searched first, so learned candidates take
    // precedence. Results from the bundled dictionary are then appended to
    // keep standard SKK fallback candidates available.
    const std::string key = current_dictionary_key();
    if (!s_user_dictionary_path.empty()) search_dictionary_key(s_user_dictionary_path, key);
    if (!s_dictionary_path.empty()) search_dictionary_key(s_dictionary_path, key);
    if (s_candidate_count > 0) return true;

    // Some inflected forms start their okurigana with a sokuon (っ), while
    // SKK-JISYO registers the verb using another inflection's initial letter.
    if (s_okuri_active && s_okuri_kana.rfind("っ", 0) == 0) {
        if (!s_user_dictionary_path.empty()) search_dictionary_okuri_family(s_user_dictionary_path, s_kana);
        if (!s_dictionary_path.empty()) search_dictionary_okuri_family(s_dictionary_path, s_kana);
        if (s_candidate_count > 0) return true;
    }
    if (s_okuri_active) {
        if (!s_user_dictionary_path.empty()) search_dictionary_key(s_user_dictionary_path, s_kana);
        if (!s_dictionary_path.empty()) search_dictionary_key(s_dictionary_path, s_kana);
    }

    update_state();
    return s_candidate_count > 0;
}

bool ime_skk_t::search_dictionary_okuri_family(const std::string &path, const std::string &reading)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (fp == nullptr) return false;

    char line[MAX_DICT_LINE_BYTES + 1] = {};
    while (fgets(line, sizeof(line), fp) != nullptr && s_candidate_count < MAX_CANDIDATES) {
        size_t line_len = strlen(line);
        if (line_len == MAX_DICT_LINE_BYTES && line[line_len - 1] != '\n') {
            int ch = 0;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF) {}
            continue;
        }
        if (line[0] == ';' || line[0] == '\r' || line[0] == '\n') continue;

        char *separator = strchr(line, ' ');
        if (separator == nullptr) continue;
        size_t key_len = (size_t)(separator - line);
        if (key_len != reading.size() + 1 ||
            memcmp(line, reading.data(), reading.size()) != 0 ||
            !std::isalpha((unsigned char)line[key_len - 1])) continue;

        char *p = strchr(separator, '/');
        if (p == nullptr) continue;
        p++;
        while (*p != '\0' && *p != '\r' && *p != '\n' && s_candidate_count < MAX_CANDIDATES) {
            char *end = strchr(p, '/');
            if (end == nullptr) break;
            char *annotation = strchr(p, ';');
            char *candidate_end = (annotation != nullptr && annotation < end) ? annotation : end;
            if (candidate_end > p) {
                append_candidate(p, (size_t)(candidate_end - p));
            }
            p = end + 1;
        }
    }
    fclose(fp);

    update_state();
    return s_candidate_count > 0;
}

bool ime_skk_t::search_dictionary_key(const std::string &path, const std::string &key)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (fp == nullptr) return false;

    char line[MAX_DICT_LINE_BYTES + 1] = {};
    while (fgets(line, sizeof(line), fp) != nullptr) {
        size_t line_len = strlen(line);
        if (line_len == MAX_DICT_LINE_BYTES && line[line_len - 1] != '\n') {
            int ch = 0;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF) {}
            continue;
        }
        if (line[0] == ';' || line[0] == '\r' || line[0] == '\n') continue;

        char *separator = strchr(line, ' ');
        if (separator == nullptr) continue;
        size_t key_len = (size_t)(separator - line);
        if (key_len != key.size() || memcmp(line, key.data(), key_len) != 0) continue;

        char *p = strchr(separator, '/');
        if (p == nullptr) break;
        p++;
        while (*p != '\0' && *p != '\r' && *p != '\n' && s_candidate_count < MAX_CANDIDATES) {
            char *end = strchr(p, '/');
            if (end == nullptr) break;
            char *annotation = strchr(p, ';');
            char *candidate_end = (annotation != nullptr && annotation < end) ? annotation : end;
            if (candidate_end > p) {
                append_candidate(p, (size_t)(candidate_end - p));
            }
            p = end + 1;
        }
        break;
    }
    fclose(fp);

    update_state();
    return s_candidate_count > 0;
}

bool ime_skk_t::append_candidate(const char *candidate, size_t len)
{
    if (candidate == nullptr || len == 0 || s_candidate_count >= MAX_CANDIDATES) return false;
    for (size_t i = 0; i < s_candidate_count; ++i) {
        if (s_candidates[i].size() == len && s_candidates[i].compare(0, len, candidate, len) == 0) {
            return false;
        }
    }
    s_candidates[s_candidate_count].assign(candidate, len);
    s_candidate_count++;
    return true;
}

std::string ime_skk_t::current_dictionary_key() const
{
    std::string key = s_kana;
    if (s_okuri_active && s_okuri_initial != '\0') key.push_back(s_okuri_initial);
    return key;
}

bool ime_skk_t::update_user_dictionary(const std::string &key, const std::string &candidate)
{
    if (s_user_dictionary_path.empty() || key.empty() || candidate.empty()) return false;

    const std::string temporary_path = s_user_dictionary_path + ".tmp";
    FILE *out = fopen(temporary_path.c_str(), "wb");
    if (out == nullptr) return false;

    FILE *in = fopen(s_user_dictionary_path.c_str(), "rb");
    bool found_key = false;
    bool ok = true;
    char line[MAX_DICT_LINE_BYTES + 1] = {};
    while (in != nullptr && fgets(line, sizeof(line), in) != nullptr) {
        const size_t line_len = strlen(line);
        if (line_len == MAX_DICT_LINE_BYTES && line[line_len - 1] != '\n') {
            int ch = 0;
            while ((ch = fgetc(in)) != '\n' && ch != EOF) {}
            continue;
        }
        char *separator = strchr(line, ' ');
        const size_t key_len = separator ? (size_t)(separator - line) : 0;
        if (separator == nullptr || key_len != key.size() || memcmp(line, key.data(), key_len) != 0) {
            if (fputs(line, out) < 0) ok = false;
            continue;
        }

        found_key = true;
        char *values = strchr(separator, '/');
        bool already_present = false;
        if (values != nullptr) {
            char *p = values + 1;
            while (*p != '\0' && *p != '\r' && *p != '\n') {
                char *end = strchr(p, '/');
                if (end == nullptr) break;
                char *annotation = strchr(p, ';');
                char *candidate_end = (annotation != nullptr && annotation < end) ? annotation : end;
                if ((size_t)(candidate_end - p) == candidate.size() &&
                    memcmp(p, candidate.data(), candidate.size()) == 0) {
                    already_present = true;
                    break;
                }
                p = end + 1;
            }
        }
        if (already_present) {
            if (fputs(line, out) < 0) ok = false;
        } else if (values != nullptr) {
            if (fprintf(out, "%s /%s%s", key.c_str(), candidate.c_str(), values) < 0) ok = false;
        } else if (fprintf(out, "%s /%s/\n", key.c_str(), candidate.c_str()) < 0) {
            ok = false;
        }
    }
    if (in != nullptr) fclose(in);
    if (!found_key && fprintf(out, "%s /%s/\n", key.c_str(), candidate.c_str()) < 0) ok = false;
    if (fclose(out) != 0) ok = false;

    if (!ok || std::rename(temporary_path.c_str(), s_user_dictionary_path.c_str()) != 0) {
        remove(temporary_path.c_str());
        return false;
    }
    return true;
}

bool ime_skk_t::learn_current_candidate()
{
    if (s_candidate_count == 0 || s_candidate_index >= s_candidate_count) return false;
    return update_user_dictionary(current_dictionary_key(), s_candidates[s_candidate_index]);
}

std::string ime_skk_t::direct_commit_text() const
{
    if (s_conversion_active) return EMPTY_STRING;
    return (s_kana_mode == ime_kana_mode_t::KATAKANA) ? hiragana_to_katakana(s_kana) : s_kana;
}

std::string ime_skk_t::current_candidate_commit() const
{
    if (s_candidate_count == 0) return EMPTY_STRING;
    return s_candidates[s_candidate_index] + s_okuri_kana;
}

ime_result_t ime_skk_t::input_text(const char *text, size_t len)
{
    ime_result_t result = {};
    if (text == nullptr || len == 0) return result;

    // Temporary ASCII entry is intentionally passed through untouched. The
    // overlay remains visible until Escape or a terminal special key exits it.
    if (s_ascii_mode) return result;

    if (s_state == ime_state_t::CANDIDATE && s_candidate_count > 0) {
        result.consumed = true;
        learn_current_candidate();
        result.commit = current_candidate_commit();
        clear_composition();
        clear_candidates();
        s_state = ime_state_t::IDLE;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char byte = (unsigned char)text[i];
        if (byte == ' ') {
            ime_result_t space_result = input_key(ime_key_t::SPACE);
            if (space_result.consumed) result.consumed = true;
            result.commit += space_result.commit;
            result.changed = result.changed || space_result.changed;
            continue;
        }

        // z is a direct-kana symbol prefix. It is held for one following
        // byte so ordinary romaji such as za continues to work unchanged.
        if (s_z_prefix) {
            s_z_prefix = false;
            const char *symbol = z_symbol((char)byte);
            if (symbol != nullptr) {
                append_hiragana(symbol);
                result.consumed = true;
                result.changed = true;
                update_state();
                continue;
            }
            s_romaji.push_back('z');
        }

        // In direct Japanese input, a lone l enters a temporary ASCII mode.
        // A second l is then sent literally; Escape returns to kana input.
        if (byte == 'l' && !s_conversion_active && s_romaji.empty() && s_kana.empty()) {
            s_ascii_mode = true;
            result.consumed = true;
            result.changed = true;
            update_state();
            continue;
        }

        // z is reserved as a symbol prefix only at an empty direct-kana
        // boundary. All other uses (za, zu, ...) remain normal romaji.
        if (byte == 'z' && !s_conversion_active && s_romaji.empty()) {
            s_z_prefix = true;
            result.consumed = true;
            result.changed = true;
            update_state();
            continue;
        }

        // SKK q behaviour in direct kana state:
        // - With no preedit, toggle the persistent Hiragana/Katakana mode.
        // - With completed kana, convert the current preedit to the opposite
        //   script, commit it locally, and keep the current input mode.
        // This makes JP-HIRA "suittiq" commit "スイッチ" and JP-KATA
        // "suittiq" commit "すいっち" without a trailing CR.
        if (byte == 'q' && !s_conversion_active && s_romaji.empty()) {
            if (s_kana.empty()) {
                s_kana_mode = (s_kana_mode == ime_kana_mode_t::HIRAGANA)
                            ? ime_kana_mode_t::KATAKANA : ime_kana_mode_t::HIRAGANA;
            } else {
                result.commit = (s_kana_mode == ime_kana_mode_t::HIRAGANA)
                              ? hiragana_to_katakana(s_kana) : s_kana;
                clear_composition();
            }
            update_state();
            result.consumed = true;
            result.changed = true;
            continue;
        }

        result.consumed = true;
        if (byte >= 0x80) {
            flush_romaji(true);
            append_hiragana(std::string(1, (char)byte).c_str());
        } else if (std::isalpha(byte) ||
                   (byte == '\'' && s_romaji.size() == 1 && s_romaji[0] == 'n')) {
            bool uppercase = std::isupper(byte) != 0;
            char lower = (char)std::tolower(byte);

            if (uppercase && !s_conversion_active) {
                // First uppercase begins a local SKK conversion.  If direct
                // kana already precedes it, interpret this uppercase as the
                // beginning of its okurigana (atamaKara / miRu); otherwise it
                // starts a regular conversion reading (Kanji / MiRu).
                flush_romaji(true);
                const bool has_preceding_kana = !s_kana.empty();
                s_conversion_active = true;
                s_okuri_active = has_preceding_kana;
                s_okuri_initial = has_preceding_kana ? lower : '\0';
                s_okuri_kana.clear();
                s_romaji.clear();
            } else if (uppercase && s_conversion_active && !s_okuri_active) {
                // The next uppercase starts the okurigana segment.  Its
                // lowercase initial is appended to the SKK dictionary key.
                flush_romaji(true);
                s_okuri_active = true;
                s_okuri_initial = lower;
                s_okuri_kana.clear();
                s_romaji.clear();
            }

            if (s_romaji.size() < MAX_READING_BYTES) {
                s_romaji.push_back(lower);
                process_romaji();
            }
        } else {
            flush_romaji(true);
            const char *punctuation = punctuation_text(s_punctuation_style, (char)byte);
            if (punctuation != nullptr) {
                append_hiragana(punctuation);
            } else {
                append_hiragana(std::string(1, (char)byte).c_str());
            }
        }

        // Keep direct kana local until an explicit delimiter.  A subsequent
        // uppercase starts local conversion and retains preceding kana in the
        // same overlay; it does not transmit the pending text.
        update_state();
        result.changed = true;
    }
    return result;
}

ime_result_t ime_skk_t::input_key(ime_key_t key)
{
    ime_result_t result = {};

    if (s_state == ime_state_t::IDLE) return result;

    if (s_ascii_mode) {
        if (key == ime_key_t::ESCAPE) {
            reset();
            result.consumed = true;
            result.changed = true;
        }
        return result;
    }

    // A standalone z prefix has no symbol suffix. Preserve it as literal
    // ASCII before another IME operation rather than silently dropping it.
    // Escape remains a true cancellation of the pending prefix.
    if (s_z_prefix && key != ime_key_t::ESCAPE) {
        s_z_prefix = false;
        append_hiragana("z");
        update_state();
    }

    result.consumed = true;

    if (s_state == ime_state_t::CANDIDATE) {
        switch (key) {
        case ime_key_t::SPACE:
        case ime_key_t::RIGHT:
            s_candidate_index = (s_candidate_index + 1) % s_candidate_count;
            result.changed = true;
            return result;
        case ime_key_t::LEFT:
            s_candidate_index = (s_candidate_index + s_candidate_count - 1) % s_candidate_count;
            result.changed = true;
            return result;
        case ime_key_t::ENTER:
        case ime_key_t::COMMIT:
            learn_current_candidate();
            result.commit = current_candidate_commit();
            reset();
            result.changed = true;
            return result;
        case ime_key_t::ESCAPE:
            clear_candidates();
            update_state();
            result.changed = true;
            return result;
        case ime_key_t::BACKSPACE:
            clear_candidates();
            erase_last_preedit();
            result.changed = true;
            return result;
        }
    }

    if (s_conversion_active) {
        switch (key) {
        case ime_key_t::SPACE:
            flush_romaji(true);
            search_dictionary();
            result.changed = true;
            break;
        case ime_key_t::ENTER:
        case ime_key_t::COMMIT:
            flush_romaji(true);
            result.commit = s_kana + s_okuri_kana;
            reset();
            result.changed = true;
            break;
        case ime_key_t::ESCAPE:
            reset();
            result.changed = true;
            break;
        case ime_key_t::BACKSPACE:
            erase_last_preedit();
            result.changed = true;
            break;
        case ime_key_t::LEFT:
        case ime_key_t::RIGHT:
            break;
        }
        return result;
    }

    // A direct-mode composition becomes a conversion reading on Space.  The
    // reading remains local regardless of whether the dictionary has a match;
    // Enter can still explicitly commit it as kana if no candidate is wanted.
    switch (key) {
    case ime_key_t::SPACE:
        flush_romaji(true);
        s_conversion_active = true;
        s_okuri_active = false;
        s_okuri_initial = '\0';
        s_okuri_kana.clear();
        search_dictionary();
        result.changed = true;
        break;
    case ime_key_t::ENTER:
    case ime_key_t::COMMIT:
        flush_romaji(true);
        result.commit = direct_commit_text();
        if (key == ime_key_t::ENTER) result.commit += "\r";
        clear_composition();
        result.changed = true;
        break;
    case ime_key_t::ESCAPE:
        reset();
        result.changed = true;
        break;
    case ime_key_t::BACKSPACE:
        erase_last_preedit();
        result.changed = true;
        break;
    case ime_key_t::LEFT:
    case ime_key_t::RIGHT:
        break;
    }
    update_state();
    return result;
}
