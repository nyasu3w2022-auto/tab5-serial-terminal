#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "main/ime_skk.h"

static ime_result_t type(ime_skk_t &ime, const char *text)
{
    ime_result_t result = ime.input_text(text, strlen(text));
    assert(result.consumed);
    return result;
}

int main()
{
    const char *dict_path = "/tmp/tab5-ime-test-skk.txt";
    FILE *dict = fopen(dict_path, "wb");
    assert(dict != nullptr);
    fputs("; UTF-8 test dictionary\n", dict);
    fputs("かんじ /漢字/幹事/\n", dict);
    fputs("みr /見/診/\n", dict);
    fputs("かk /書/描/\n", dict);
    fputs("わん /腕/碗/湾/椀/\n", dict);
    fclose(dict);

    ime_skk_t ime;
    ime.set_dictionary_path(dict_path);
    assert(ime.dictionary_available());
    assert(ime.state() == ime_state_t::IDLE);
    ime_result_t idle_space = ime.input_text(" ", 1);
    assert(!idle_space.consumed && !idle_space.changed);

    // Direct lowercase romaji remains local until an explicit delimiter.
    // This prevents completed vowels from being sent one kana at a time.
    ime_result_t greeting = type(ime, "konnichiha");
    assert(greeting.commit.empty());
    assert(ime.state() == ime_state_t::COMPOSING);
    assert(ime.preedit_text() == "こんにちは");
    assert(ime.input_key(ime_key_t::ENTER).commit == "こんにちは\r");
    assert(ime.state() == ime_state_t::IDLE);

    // Lowercase direct text before an uppercase initial is committed as a
    // block, then the uppercase key begins the next SKK conversion reading.
    std::string phrase_commit;
    const char *phrase_input = "atamaKara";
    for (size_t i = 0; phrase_input[i] != '\0'; ++i) {
        phrase_commit += ime.input_text(&phrase_input[i], 1).commit;
        assert(phrase_commit.empty());
    }
    assert(ime.is_conversion_active());
    assert(!ime.is_okuri_active());
    assert(ime.preedit_text() == "あたまから");
    assert(ime.input_key(ime_key_t::ENTER).commit == "あたまから");

    // q toggles direct Hiragana/Katakana mode without contacting the peer.
    ime_result_t kata_on = type(ime, "q");
    assert(kata_on.commit.empty());
    assert(ime.kana_mode() == ime_kana_mode_t::KATAKANA);
    ime_result_t kata = type(ime, "katakana");
    assert(kata.commit.empty());
    assert(ime.preedit_text() == "カタカナ");
    assert(ime.input_key(ime_key_t::ENTER).commit == "カタカナ\r");
    type(ime, "q");
    assert(ime.kana_mode() == ime_kana_mode_t::HIRAGANA);

    // Uppercase initial starts plain SKK conversion: Kanji -> 漢字.
    ime_result_t conversion_start = type(ime, "Kanji");
    assert(conversion_start.commit.empty());
    assert(ime.is_conversion_active());
    assert(!ime.is_okuri_active());
    assert(ime.preedit_text() == "かんじ");
    ime_result_t search = ime.input_key(ime_key_t::SPACE);
    assert(search.consumed && search.changed && search.commit.empty());
    assert(ime.state() == ime_state_t::CANDIDATE);
    assert(ime.candidate_count() == 2);
    assert(ime.candidate_at(0) == "漢字");
    assert(ime.candidate_at(1) == "幹事");
    ime.input_key(ime_key_t::RIGHT);
    assert(ime.candidate_index() == 1);
    ime_result_t commit = ime.input_key(ime_key_t::ENTER);
    assert(commit.commit == "幹事");
    assert(ime.state() == ime_state_t::IDLE);

    // Second uppercase starts okurigana: MiRu -> 見る.
    type(ime, "MiRu");
    assert(ime.is_conversion_active());
    assert(ime.is_okuri_active());
    assert(ime.preedit_text() == "み / る");
    ime_result_t okuri_search = ime.input_key(ime_key_t::SPACE);
    assert(okuri_search.consumed && okuri_search.changed);
    assert(ime.candidate_count() == 2);
    assert(ime.candidate_at(0) == "見");
    assert(ime.okuri_text() == "る");
    ime_result_t okuri_commit = ime.input_key(ime_key_t::ENTER);
    assert(okuri_commit.commit == "見る");

    // Another okurigana form: KaKu -> 書く.
    type(ime, "KaKu");
    ime.input_key(ime_key_t::SPACE);
    assert(ime.candidate_at(0) == "書");
    assert(ime.input_key(ime_key_t::ENTER).commit == "書く");

    // Cancel leaves the original SKK reading available for another conversion.
    type(ime, "MiRu");
    ime.input_key(ime_key_t::SPACE);
    ime_result_t cancel = ime.input_key(ime_key_t::ESCAPE);
    assert(cancel.consumed && cancel.changed);
    assert(ime.state() == ime_state_t::COMPOSING);
    assert(ime.preedit_text() == "み / る");
    ime.input_key(ime_key_t::ESCAPE);
    assert(ime.state() == ime_state_t::IDLE);

    // A lowercase reading followed by an uppercase input remains entirely
    // local; neither the initial reading nor the following kana is sent.
    std::string mi_ru_commit;
    const char *mi_ru = "miRu";
    for (size_t i = 0; mi_ru[i] != '\0'; ++i) {
        mi_ru_commit += ime.input_text(&mi_ru[i], 1).commit;
        assert(mi_ru_commit.empty());
    }
    assert(ime.is_conversion_active());
    assert(ime.preedit_text() == "みる");
    assert(ime.input_key(ime_key_t::ENTER).commit == "みる");

    // Romaji edge cases in direct mode remain pending until Enter.
    assert(type(ime, "ssha").commit.empty());
    assert(ime.input_key(ime_key_t::ENTER).commit == "っしゃ\r");
    assert(type(ime, "nna").commit.empty());
    assert(ime.input_key(ime_key_t::ENTER).commit == "んな\r");
    ime_result_t trailing_n = type(ime, "nn");
    assert(trailing_n.commit.empty());
    ime_result_t trailing_n_commit = ime.input_key(ime_key_t::ENTER);
    assert(trailing_n_commit.commit == "ん\r");
    assert(type(ime, "n'").commit.empty());
    assert(ime.input_key(ime_key_t::ENTER).commit == "ん\r");

    // Exercise the actual bundled dictionary and its SKK okurigana key.
    ime.set_dictionary_path("assets/SKK-JISYO.S.txt");
    assert(ime.dictionary_available());
    type(ime, "MiRu");
    ime_result_t full_dict_search = ime.input_key(ime_key_t::SPACE);
    assert(full_dict_search.consumed && full_dict_search.changed);
    assert(ime.candidate_count() >= 1);
    assert(ime.candidate_at(0) == "見");
    assert(ime.input_key(ime_key_t::ENTER).commit == "見る");

    std::remove(dict_path);
    puts("ime_skk tests passed");
    return 0;
}
