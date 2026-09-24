#include <assert.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "main/dictionary_transfer.h"

static void write_file(const char *path, const char *text)
{
    FILE *out = fopen(path, "wb");
    assert(out != nullptr);
    assert(fputs(text, out) >= 0);
    assert(fclose(out) == 0);
}

static std::string read_file(const char *path)
{
    FILE *in = fopen(path, "rb");
    assert(in != nullptr);
    std::string value;
    char buffer[256] = {};
    while (fgets(buffer, sizeof(buffer), in) != nullptr) value += buffer;
    assert(fclose(in) == 0);
    return value;
}

int main()
{
    const char *user = "/tmp/tab5-transfer-user.txt";
    const char *sd_export = "/tmp/tab5-transfer-export.txt";
    const char *sd_import = "/tmp/tab5-transfer-import.txt";
    const char *invalid = "/tmp/tab5-transfer-invalid.txt";
    std::remove(user);
    std::remove(sd_export);
    std::remove(sd_import);
    std::remove(invalid);

    // Export an existing user dictionary through the atomic output path.
    write_file(user, "; user dictionary\nきt /来/着/\nかんじ /漢字/\n");
    dictionary_transfer_result_t exported = dictionary_transfer_export(user, sd_export);
    assert(exported.status == dictionary_transfer_status_t::OK);
    assert(exported.source_entries == 3);
    assert(exported.resulting_entries == 3);
    const std::string exported_text = read_file(sd_export);
    assert(exported_text.find("きt /来/着/\n") != std::string::npos);
    assert(exported_text.find("かんじ /漢字/\n") != std::string::npos);

    // FATFS does not replace a destination during rename. Exporting again must
    // therefore publish over an existing exchange file without losing it.
    write_file(user, "きt /来/\n");
    dictionary_transfer_result_t exported_again = dictionary_transfer_export(user, sd_export);
    assert(exported_again.status == dictionary_transfer_status_t::OK);
    const std::string exported_again_text = read_file(sd_export);
    assert(exported_again_text.find("きt /来/\n") != std::string::npos);
    assert(exported_again_text.find("かんじ") == std::string::npos);
    assert(std::remove("/tmp/SKKBAK.BAK") != 0);

    // Restore the multi-entry user dictionary for merge coverage.
    write_file(user, "; user dictionary\nきt /来/着/\nかんじ /漢字/\n");

    // Merge keeps current order and appends only imported non-duplicate values.
    write_file(sd_import, "; imported\nきt /着/帰/\nみr /見/\n");
    dictionary_transfer_result_t merged = dictionary_transfer_import(
        sd_import, user, dictionary_transfer_mode_t::MERGE);
    assert(merged.status == dictionary_transfer_status_t::OK);
    assert(merged.source_entries == 3);
    assert(merged.resulting_entries == 5);
    const std::string merged_text = read_file(user);
    assert(merged_text.find("きt /来/着/帰/\n") != std::string::npos);
    assert(merged_text.find("みr /見/\n") != std::string::npos);

    // Replace discards entries absent from the imported dictionary.
    write_file(sd_import, "きt /来/\n");
    dictionary_transfer_result_t replaced = dictionary_transfer_import(
        sd_import, user, dictionary_transfer_mode_t::REPLACE);
    assert(replaced.status == dictionary_transfer_status_t::OK);
    assert(replaced.resulting_entries == 1);
    const std::string replaced_text = read_file(user);
    assert(replaced_text.find("きt /来/\n") != std::string::npos);
    assert(replaced_text.find("みr") == std::string::npos);

    // ASCII abbreviation keys use the same SKK exchange format and can be
    // added through the documented SD Import Merge path.
    write_file(sd_import, "myhost /MyHost/\nusr/bin /USR-BIN/\n");
    dictionary_transfer_result_t abbrev_merged = dictionary_transfer_import(
        sd_import, user, dictionary_transfer_mode_t::MERGE);
    assert(abbrev_merged.status == dictionary_transfer_status_t::OK);
    assert(abbrev_merged.resulting_entries == 3);
    const std::string abbrev_merged_text = read_file(user);
    assert(abbrev_merged_text.find("myhost /MyHost/\n") != std::string::npos);
    assert(abbrev_merged_text.find("usr/bin /USR-BIN/\n") != std::string::npos);

    // Malformed input must be rejected without altering the current dictionary.
    const std::string before_invalid = abbrev_merged_text;
    write_file(invalid, "きt /来\n");
    dictionary_transfer_result_t bad = dictionary_transfer_import(
        invalid, user, dictionary_transfer_mode_t::MERGE);
    assert(bad.status == dictionary_transfer_status_t::INVALID_FORMAT);
    assert(read_file(user) == before_invalid);

    dictionary_transfer_result_t missing = dictionary_transfer_import(
        "/tmp/no-such-tab5-skk-file.txt", user, dictionary_transfer_mode_t::MERGE);
    assert(missing.status == dictionary_transfer_status_t::SOURCE_NOT_FOUND);

    std::remove(user);
    std::remove(sd_export);
    std::remove(sd_import);
    std::remove(invalid);
    puts("dictionary_transfer tests passed");
    return 0;
}
