#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "main/ime_skk.h"

static void type(ime_skk_t &ime, const char *text)
{
    ime_result_t result = ime.input_text(text, strlen(text));
    assert(result.consumed);
    assert(result.commit.empty());
}

int main()
{
    const char *dict_path = "/tmp/tab5-ime-test-skk.txt";
    FILE *dict = fopen(dict_path, "wb");
    assert(dict != nullptr);
    fputs("; UTF-8 test dictionary\n", dict);
    fputs("かんじ /漢字/幹事/\n", dict);
    fputs("にほん /日本/二本/\n", dict);
    fputs("わん /腕/碗/湾/椀/\n", dict);
    fclose(dict);

    ime_skk_t ime;
    ime.set_dictionary_path(dict_path);
    assert(ime.dictionary_available());
    assert(ime.state() == ime_state_t::IDLE);
    ime_result_t idle_space = ime.input_text(" ", 1);
    assert(!idle_space.consumed && !idle_space.changed);

    type(ime, "konnichiha");
    assert(ime.preedit_text() == "こんにちは");
    ime_result_t kana = ime.input_key(ime_key_t::ENTER);
    assert(kana.consumed && kana.changed);
    assert(kana.commit == "こんにちは");
    assert(ime.state() == ime_state_t::IDLE);

    type(ime, "kanji");
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

    type(ime, "wan");
    ime.input_key(ime_key_t::SPACE);
    assert(ime.candidate_count() == 4);
    ime_result_t cancel = ime.input_key(ime_key_t::ESCAPE);
    assert(cancel.consumed && cancel.changed);
    assert(ime.state() == ime_state_t::COMPOSING);
    assert(ime.preedit_text() == "わん");
    ime.input_key(ime_key_t::BACKSPACE);
    assert(ime.preedit_text() == "わ");
    ime.input_key(ime_key_t::BACKSPACE);
    assert(ime.preedit_text().empty());
    assert(ime.state() == ime_state_t::IDLE);

    type(ime, "ssha");
    assert(ime.preedit_text() == "っしゃ");
    ime.input_key(ime_key_t::ESCAPE);

    type(ime, "shi chi tsu fu ja");
    assert(ime.preedit_text() == "しちつふじゃ");
    ime.input_key(ime_key_t::ESCAPE);

    type(ime, "nna");
    assert(ime.preedit_text() == "んな");
    ime.input_key(ime_key_t::ESCAPE);

    type(ime, "nn");
    ime_result_t trailing_n = ime.input_key(ime_key_t::ENTER);
    assert(trailing_n.commit == "ん");
    ime.input_key(ime_key_t::ESCAPE);

    type(ime, "n'");
    ime_result_t apostrophe_n = ime.input_key(ime_key_t::ENTER);
    assert(apostrophe_n.commit == "ん");
    ime.input_key(ime_key_t::ESCAPE);

    ime.set_dictionary_path("assets/SKK-JISYO.S.txt");
    assert(ime.dictionary_available());
    type(ime, "kanji");
    ime_result_t full_dict_search = ime.input_key(ime_key_t::SPACE);
    assert(full_dict_search.consumed && full_dict_search.changed);
    assert(ime.candidate_count() >= 2);
    assert(ime.candidate_at(0) == "漢字");
    assert(ime.candidate_at(1) == "幹事");
    ime.input_key(ime_key_t::ESCAPE);

    std::remove(dict_path);
    puts("ime_skk tests passed");
    return 0;
}
