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
    const char *supplement_dict_path = "/tmp/tab5-ime-test-supplement.txt";
    const char *user_dict_path = "/tmp/tab5-ime-test-user.txt";
    std::remove(user_dict_path);
    FILE *supplement = fopen(supplement_dict_path, "wb");
    assert(supplement != nullptr);
    fputs("; TAB5 supplement dictionary\n", supplement);
    fputs("きt /来/\n", supplement);
    fclose(supplement);
    FILE *dict = fopen(dict_path, "wb");
    assert(dict != nullptr);
    fputs("; UTF-8 test dictionary\n", dict);
    fputs("かんじ /漢字/幹事/\n", dict);
    fputs("みr /見/診/\n", dict);
    fputs("かk /書/描/\n", dict);
    fputs("あたま /頭/\n", dict);
    fputs("はしr /走/\n", dict);
    fputs("いu /言/云/\n", dict);
    fputs("わん /腕/碗/湾/椀/\n", dict);
    fclose(dict);

    ime_skk_t ime;
    ime.set_dictionary_path(dict_path);
    ime.set_supplement_dictionary_path(supplement_dict_path);
    ime.set_user_dictionary_path(user_dict_path);
    assert(ime.dictionary_available());
    assert(ime.user_dictionary_writable());
    assert(!ime.user_dictionary_available());
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

    // Ctrl+J maps to COMMIT: direct kana is confirmed without a terminal CR.
    type(ime, "arigatou");
    ime_result_t direct_ctrl_j = ime.input_key(ime_key_t::COMMIT);
    assert(direct_ctrl_j.consumed && direct_ctrl_j.changed);
    assert(direct_ctrl_j.commit == "ありがとう");
    assert(ime.state() == ime_state_t::IDLE);
    assert(!ime.input_key(ime_key_t::COMMIT).consumed);

    // Japanese, ASCII and fullwidth punctuation profiles affect . , - only.
    type(ime, "a.su,-");
    assert(ime.input_key(ime_key_t::COMMIT).commit == "あ。す、ー");
    ime.set_punctuation_style(ime_punctuation_style_t::ASCII);
    type(ime, "a.su,-");
    assert(ime.input_key(ime_key_t::COMMIT).commit == "あ.す,-");
    ime.set_punctuation_style(ime_punctuation_style_t::FULLWIDTH);
    type(ime, "a.su,-");
    assert(ime.input_key(ime_key_t::COMMIT).commit == "あ．す，－");
    ime.set_punctuation_style(ime_punctuation_style_t::JAPANESE);

    // z-prefix symbols do not interfere with normal z romaji sequences.
    type(ime, "z/");
    assert(ime.preedit_text() == "・");
    assert(ime.input_key(ime_key_t::COMMIT).commit == "・");
    type(ime, "z-");
    assert(ime.input_key(ime_key_t::COMMIT).commit == "〜");
    type(ime, "z.");
    assert(ime.input_key(ime_key_t::COMMIT).commit == "…");
    type(ime, "za");
    assert(ime.input_key(ime_key_t::COMMIT).commit == "ざ");

    // l at an empty direct-kana boundary enters temporary ASCII input. ASCII
    // text is not consumed by the IME; Escape returns to kana input.
    ime_result_t ascii_enter = type(ime, "l");
    assert(ascii_enter.commit.empty());
    assert(ime.is_ascii_mode());
    ime_result_t ascii_text = ime.input_text("ls -l", 5);
    assert(!ascii_text.consumed && !ascii_text.changed);
    assert(ime.input_key(ime_key_t::ESCAPE).consumed);
    assert(!ime.is_ascii_mode());
    assert(ime.state() == ime_state_t::IDLE);

    // Space starts candidate selection even when the reading was entered in
    // lowercase.  It must not transmit the raw hiragana plus a space.
    ime_result_t atama_direct = type(ime, "atama");
    assert(atama_direct.commit.empty());
    assert(!ime.is_conversion_active());
    ime_result_t atama_search = ime.input_key(ime_key_t::SPACE);
    assert(atama_search.consumed && atama_search.changed);
    assert(atama_search.commit.empty());
    assert(ime.state() == ime_state_t::CANDIDATE);
    assert(ime.candidate_count() == 1);
    assert(ime.candidate_at(0) == "頭");
    assert(ime.input_key(ime_key_t::ENTER).commit == "頭");
    assert(ime.state() == ime_state_t::IDLE);

    // Ctrl+J also confirms a selected candidate without a terminal CR.
    type(ime, "atama");
    ime.input_key(ime_key_t::SPACE);
    ime_result_t candidate_ctrl_j = ime.input_key(ime_key_t::COMMIT);
    assert(candidate_ctrl_j.commit == "頭");
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
    assert(ime.is_okuri_active());
    assert(ime.preedit_text() == "あたま / から");
    // あたまk is absent, so lookup falls back to あたま and appends から.
    ime.input_key(ime_key_t::SPACE);
    assert(ime.candidate_count() == 1);
    assert(ime.candidate_at(0) == "頭");
    assert(ime.input_key(ime_key_t::ENTER).commit == "頭から");
    assert(ime.state() == ime_state_t::IDLE);

    // The same reading must have identical state when the keyboard groups
    // adjacent characters into multiple string events.
    ime_skk_t chunked_ime;
    chunked_ime.set_dictionary_path(dict_path);
    const char *chunks[] = {"ata", "maK", "ara"};
    std::string chunked_commit;
    for (const char *chunk : chunks) {
        ime_result_t r = chunked_ime.input_text(chunk, strlen(chunk));
        chunked_commit += r.commit;
    }
    assert(chunked_commit.empty());
    assert(chunked_ime.is_conversion_active());
    assert(chunked_ime.is_okuri_active());
    assert(chunked_ime.preedit_text() == "あたま / から");
    chunked_ime.input_key(ime_key_t::SPACE);
    assert(chunked_ime.candidate_at(0) == "頭");
    assert(chunked_ime.input_key(ime_key_t::ENTER).commit == "頭から");
    assert(chunked_ime.state() == ime_state_t::IDLE);

    // In direct kana state, q with no preedit still toggles the input mode.
    // With a preedit it commits the opposite script, as in SKK.
    assert(ime.kana_mode() == ime_kana_mode_t::HIRAGANA);
    ime_result_t hira_q = type(ime, "suittiq");
    assert(hira_q.commit == "スイッチ");
    assert(ime.state() == ime_state_t::IDLE);
    assert(ime.kana_mode() == ime_kana_mode_t::HIRAGANA);

    ime_result_t kata_on = type(ime, "q");
    assert(kata_on.commit.empty());
    assert(ime.kana_mode() == ime_kana_mode_t::KATAKANA);

    // Verify the same operation when each physical key is delivered as a
    // distinct keyboard string event.
    std::string kata_q_commit;
    const char *kata_q_input = "suittiq";
    for (size_t i = 0; kata_q_input[i] != '\0'; ++i) {
        kata_q_commit += ime.input_text(&kata_q_input[i], 1).commit;
    }
    assert(kata_q_commit == "すいっち");
    assert(ime.state() == ime_state_t::IDLE);
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
    // Earlier 「頭」選択に加え、送り仮名キー「あたまk」と今回の
    // 「幹事」が別のSKKキーとしてRAMに保留される。再選択は同一キー
    // の保留を重複させない。
    assert(ime.pending_learning_count() == 3);
    assert(!ime.user_dictionary_available());

    // Deferred learning changes candidate priority in RAM immediately, but a
    // reboot only sees it after an explicit or scheduled flush.
    assert(ime.flush_pending_learning());
    assert(ime.pending_learning_count() == 0);
    assert(ime.user_dictionary_available());

    // A fresh engine must prefer the saved learned candidate without
    // duplicating the bundled candidate list. This models a reboot after a
    // user choice followed by a successful deferred flush.
    ime_skk_t learned_ime;
    learned_ime.set_dictionary_path(dict_path);
    learned_ime.set_user_dictionary_path(user_dict_path);
    type(learned_ime, "Kanji");
    learned_ime.input_key(ime_key_t::SPACE);
    assert(learned_ime.candidate_count() == 2);
    assert(learned_ime.candidate_at(0) == "幹事");
    assert(learned_ime.candidate_at(1) == "漢字");
    assert(learned_ime.input_key(ime_key_t::COMMIT).commit == "幹事");

    // The supplementary dictionary sits between user and system dictionaries.
    // Its きt /来/ entry makes standard SKK input KiTa -> 来た available even
    // though the small system test dictionary does not contain that key.
    type(ime, "KiTa");
    ime.input_key(ime_key_t::SPACE);
    assert(ime.candidate_count() == 1);
    assert(ime.candidate_at(0) == "来");
    assert(ime.input_key(ime_key_t::COMMIT).commit == "来た");

    // Manual registration inserts the candidate at the head of the user
    // dictionary. A new engine must prioritize it over the supplement entry.
    assert(ime.register_user_candidate("き", 't', "着"));
    ime_skk_t manual_ime;
    manual_ime.set_dictionary_path(dict_path);
    manual_ime.set_supplement_dictionary_path(supplement_dict_path);
    manual_ime.set_user_dictionary_path(user_dict_path);
    type(manual_ime, "KiTa");
    manual_ime.input_key(ime_key_t::SPACE);
    assert(manual_ime.candidate_count() == 2);
    assert(manual_ime.candidate_at(0) == "着");
    assert(manual_ime.candidate_at(1) == "来");
    assert(manual_ime.input_key(ime_key_t::COMMIT).commit == "着た");

    // Re-registering an existing candidate promotes it without duplicate rows.
    assert(manual_ime.register_user_candidate("き", 't', "来"));
    ime_skk_t promoted_ime;
    promoted_ime.set_dictionary_path(dict_path);
    promoted_ime.set_supplement_dictionary_path(supplement_dict_path);
    promoted_ime.set_user_dictionary_path(user_dict_path);
    type(promoted_ime, "KiTa");
    promoted_ime.input_key(ime_key_t::SPACE);
    assert(promoted_ime.candidate_count() == 2);
    assert(promoted_ime.candidate_at(0) == "来");
    assert(promoted_ime.candidate_at(1) == "着");

    // Deletion removes only the specified user candidate. The supplemental
    // entry remains available; the final user entry removes the user line.
    assert(promoted_ime.remove_user_candidate("き", 't', "来"));
    assert(promoted_ime.remove_user_candidate("き", 't', "着"));
    ime_skk_t deleted_ime;
    deleted_ime.set_dictionary_path(dict_path);
    deleted_ime.set_supplement_dictionary_path(supplement_dict_path);
    deleted_ime.set_user_dictionary_path(user_dict_path);
    type(deleted_ime, "KiTa");
    deleted_ime.input_key(ime_key_t::SPACE);
    assert(deleted_ime.candidate_count() == 1);
    assert(deleted_ime.candidate_at(0) == "来");
    assert(!deleted_ime.remove_user_candidate("き", 't', "着"));
    assert(!deleted_ime.register_user_candidate("き/", 't', "不正"));
    assert(!deleted_ime.register_user_candidate("き", 't', "不/正"));
    assert(!deleted_ime.register_user_candidate("き", 't', "不;正"));
    ime_skk_t no_user_storage;
    assert(!no_user_storage.user_dictionary_writable());
    assert(!no_user_storage.register_user_candidate("き", 't', "来"));

    // Ctrl+J confirms a conversion reading as kana without a terminal CR.
    type(ime, "Kanji");
    ime_result_t reading_ctrl_j = ime.input_key(ime_key_t::COMMIT);
    assert(reading_ctrl_j.commit == "かんじ");
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
    assert(ime.state() == ime_state_t::IDLE);

    // Another okurigana form: KaKu -> 書く.
    type(ime, "KaKu");
    ime.input_key(ime_key_t::SPACE);
    assert(ime.candidate_at(0) == "書");
    assert(ime.input_key(ime_key_t::ENTER).commit == "書く");
    assert(ime.state() == ime_state_t::IDLE);

    // Sokuon-leading okurigana must match the same reading's registered
    // okuri-family.  HashiTta uses はしr /走/ and commits 走った.
    type(ime, "HashiTta");
    assert(ime.is_okuri_active());
    assert(ime.preedit_text() == "はし / った");
    ime.input_key(ime_key_t::SPACE);
    assert(ime.candidate_at(0) == "走");
    assert(ime.input_key(ime_key_t::ENTER).commit == "走った");
    assert(ime.state() == ime_state_t::IDLE);

    // ITta likewise finds いu /言/ by its reading's okuri-family.
    type(ime, "ITta");
    assert(ime.is_okuri_active());
    assert(ime.preedit_text() == "い / った");
    ime.input_key(ime_key_t::SPACE);
    assert(ime.candidate_at(0) == "言");
    assert(ime.input_key(ime_key_t::ENTER).commit == "言った");
    assert(ime.state() == ime_state_t::IDLE);

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
    assert(ime.is_okuri_active());
    assert(ime.preedit_text() == "み / る");
    ime.input_key(ime_key_t::SPACE);
    assert(ime.candidate_count() >= 1);
    assert(ime.candidate_at(0) == "見");
    assert(ime.input_key(ime_key_t::ENTER).commit == "見る");
    assert(ime.state() == ime_state_t::IDLE);

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

    // Exercise the actual bundled dictionary and TAB5 supplementary entries.
    ime.set_dictionary_path("assets/SKK-JISYO.S.txt");
    ime.set_supplement_dictionary_path("assets/SKK-JISYO.TAB5.txt");
    assert(ime.dictionary_available());
    type(ime, "KiTa");
    ime.input_key(ime_key_t::SPACE);
    assert(ime.candidate_count() >= 1);
    assert(ime.candidate_at(0) == "来");
    assert(ime.input_key(ime_key_t::ENTER).commit == "来た");
    type(ime, "MiRu");
    ime_result_t full_dict_search = ime.input_key(ime_key_t::SPACE);
    assert(full_dict_search.consumed && full_dict_search.changed);
    assert(ime.candidate_count() >= 1);
    assert(ime.candidate_at(0) == "見");
    assert(ime.input_key(ime_key_t::ENTER).commit == "見る");

    type(ime, "HashiTta");
    ime.input_key(ime_key_t::SPACE);
    assert(ime.candidate_count() >= 1);
    assert(ime.candidate_at(0) == "走");
    assert(ime.input_key(ime_key_t::ENTER).commit == "走った");

    type(ime, "ITta");
    ime.input_key(ime_key_t::SPACE);
    assert(ime.candidate_count() >= 1);
    assert(ime.candidate_at(0) == "言");
    assert(ime.input_key(ime_key_t::ENTER).commit == "言った");

    // Manual-save mode learns in RAM immediately, affects the next lookup,
    // and writes nothing until the explicit flush operation.
    const char *manual_learning_path = "/tmp/tab5-ime-test-manual-learning.txt";
    std::remove(manual_learning_path);
    ime_skk_t manual_learning;
    manual_learning.set_dictionary_path(dict_path);
    manual_learning.set_user_dictionary_path(manual_learning_path);
    manual_learning.set_learning_mode(ime_learning_mode_t::MANUAL);
    type(manual_learning, "Kanji");
    manual_learning.input_key(ime_key_t::SPACE);
    manual_learning.input_key(ime_key_t::RIGHT);
    assert(manual_learning.input_key(ime_key_t::COMMIT).commit == "幹事");
    assert(manual_learning.pending_learning_count() == 1);
    assert(!manual_learning.user_dictionary_available());
    type(manual_learning, "Kanji");
    manual_learning.input_key(ime_key_t::SPACE);
    assert(manual_learning.candidate_at(0) == "幹事");
    assert(manual_learning.flush_pending_learning());
    assert(manual_learning.pending_learning_count() == 0);
    assert(manual_learning.user_dictionary_available());

    // Off disables automatic priority learning altogether; selecting a later
    // candidate neither queues nor writes a user-dictionary change.
    const char *off_learning_path = "/tmp/tab5-ime-test-off-learning.txt";
    std::remove(off_learning_path);
    ime_skk_t off_learning;
    off_learning.set_dictionary_path(dict_path);
    off_learning.set_user_dictionary_path(off_learning_path);
    off_learning.set_learning_mode(ime_learning_mode_t::OFF);
    type(off_learning, "Kanji");
    off_learning.input_key(ime_key_t::SPACE);
    off_learning.input_key(ime_key_t::RIGHT);
    assert(off_learning.input_key(ime_key_t::COMMIT).commit == "幹事");
    assert(off_learning.pending_learning_count() == 0);
    assert(!off_learning.user_dictionary_available());

    std::remove(manual_learning_path);
    std::remove(off_learning_path);
    std::remove(dict_path);
    std::remove(supplement_dict_path);
    std::remove(user_dict_path);
    puts("ime_skk tests passed");
    return 0;
}
