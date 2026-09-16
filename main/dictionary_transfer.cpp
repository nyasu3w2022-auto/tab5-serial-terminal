/*
 * dictionary_transfer.cpp — Safe import/export of UTF-8/LF SKK user dictionaries.
 *
 * SPDX-License-Identifier: MIT
 */

#include "dictionary_transfer.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr size_t MAX_DICT_LINE_BYTES = 512;
using dictionary_t = std::map<std::string, std::vector<std::string>>;

bool is_safe_field(const std::string &value, bool is_key)
{
    if (value.empty()) return false;
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == '/' || ch == '\\' || ch == ';' || (is_key && ch == ' ')) {
            return false;
        }
    }
    return true;
}

bool add_candidate(dictionary_t *dictionary, const std::string &key, const std::string &candidate)
{
    if (dictionary == nullptr || !is_safe_field(key, true) || !is_safe_field(candidate, false)) {
        return false;
    }
    std::vector<std::string> &values = (*dictionary)[key];
    for (const std::string &existing : values) {
        if (existing == candidate) return true;
    }
    values.push_back(candidate);
    return true;
}

bool parse_dictionary_line(char *line, dictionary_t *dictionary)
{
    if (line == nullptr || dictionary == nullptr) return false;

    char *p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0' || *p == '\r' || *p == '\n' || *p == ';') return true;

    char *separator = strchr(p, ' ');
    if (separator == nullptr) return false;
    *separator = '\0';
    const std::string key(p);
    if (!is_safe_field(key, true)) return false;

    p = separator + 1;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '/') return false;

    bool any_candidate = false;
    while (*p != '\0' && *p != '\r' && *p != '\n') {
        if (*p != '/') return false;
        ++p;
        // A trailing slash terminates the candidate list before newline/EOF.
        if (*p == '\0' || *p == '\r' || *p == '\n') break;
        char *end = strchr(p, '/');
        if (end == nullptr) return false;
        char *annotation = (char *)memchr(p, ';', (size_t)(end - p));
        const char *candidate_end = annotation != nullptr ? annotation : end;
        if (candidate_end > p) {
            if (!add_candidate(dictionary, key, std::string(p, (size_t)(candidate_end - p)))) {
                return false;
            }
            any_candidate = true;
        }
        // Keep the delimiter intact so the next iteration can consume it.
        p = end;
    }
    return any_candidate;
}

dictionary_transfer_status_t load_dictionary(const char *path, dictionary_t *dictionary,
                                             size_t *entry_count, bool missing_is_empty)
{
    if (dictionary == nullptr || entry_count == nullptr || path == nullptr || path[0] == '\0') {
        return dictionary_transfer_status_t::TARGET_UNAVAILABLE;
    }
    dictionary->clear();
    *entry_count = 0;

    FILE *in = fopen(path, "rb");
    if (in == nullptr) {
        return missing_is_empty ? dictionary_transfer_status_t::OK
                                : dictionary_transfer_status_t::SOURCE_NOT_FOUND;
    }

    char line[MAX_DICT_LINE_BYTES + 2] = {};
    while (fgets(line, sizeof(line), in) != nullptr) {
        const size_t length = strlen(line);
        if (length == MAX_DICT_LINE_BYTES + 1 && line[length - 1] != '\n') {
            fclose(in);
            return dictionary_transfer_status_t::INVALID_FORMAT;
        }
        if (!parse_dictionary_line(line, dictionary)) {
            fclose(in);
            return dictionary_transfer_status_t::INVALID_FORMAT;
        }
    }
    if (ferror(in) != 0 || fclose(in) != 0) return dictionary_transfer_status_t::IO_ERROR;

    for (const auto &entry : *dictionary) *entry_count += entry.second.size();
    return dictionary_transfer_status_t::OK;
}

dictionary_transfer_status_t write_dictionary_atomic(const char *path, const dictionary_t &dictionary)
{
    if (path == nullptr || path[0] == '\0') return dictionary_transfer_status_t::TARGET_UNAVAILABLE;

    const std::string temporary_path = std::string(path) + ".tmp";
    FILE *out = fopen(temporary_path.c_str(), "wb");
    if (out == nullptr) return dictionary_transfer_status_t::IO_ERROR;

    bool ok = fputs("; TAB5 SKK user dictionary (UTF-8/LF)\n", out) >= 0;
    for (const auto &entry : dictionary) {
        if (!ok) break;
        if (fprintf(out, "%s ", entry.first.c_str()) < 0) {
            ok = false;
            break;
        }
        for (const std::string &candidate : entry.second) {
            if (fprintf(out, "/%s", candidate.c_str()) < 0) {
                ok = false;
                break;
            }
        }
        if (!ok || fputs("/\n", out) < 0) {
            ok = false;
            break;
        }
    }
    if (fflush(out) != 0 || fclose(out) != 0) ok = false;

    if (!ok || rename(temporary_path.c_str(), path) != 0) {
        remove(temporary_path.c_str());
        return dictionary_transfer_status_t::IO_ERROR;
    }
    return dictionary_transfer_status_t::OK;
}

void merge_dictionary(dictionary_t *destination, const dictionary_t &source)
{
    if (destination == nullptr) return;
    for (const auto &entry : source) {
        for (const std::string &candidate : entry.second) {
            add_candidate(destination, entry.first, candidate);
        }
    }
}

size_t candidate_count(const dictionary_t &dictionary)
{
    size_t count = 0;
    for (const auto &entry : dictionary) count += entry.second.size();
    return count;
}

}  // namespace

dictionary_transfer_result_t dictionary_transfer_export(const char *user_dictionary_path,
                                                        const char *target_path)
{
    dictionary_transfer_result_t result = {};
    dictionary_t source;
    result.status = load_dictionary(user_dictionary_path, &source, &result.source_entries, true);
    if (result.status != dictionary_transfer_status_t::OK) return result;

    result.status = write_dictionary_atomic(target_path, source);
    result.resulting_entries = candidate_count(source);
    return result;
}

dictionary_transfer_result_t dictionary_transfer_import(const char *source_path,
                                                        const char *user_dictionary_path,
                                                        dictionary_transfer_mode_t mode)
{
    dictionary_transfer_result_t result = {};
    dictionary_t imported;
    result.status = load_dictionary(source_path, &imported, &result.source_entries, false);
    if (result.status != dictionary_transfer_status_t::OK) return result;

    dictionary_t target;
    if (mode == dictionary_transfer_mode_t::MERGE) {
        size_t existing_entries = 0;
        result.status = load_dictionary(user_dictionary_path, &target, &existing_entries, true);
        if (result.status != dictionary_transfer_status_t::OK) return result;
        merge_dictionary(&target, imported);
    } else {
        target = imported;
    }

    result.status = write_dictionary_atomic(user_dictionary_path, target);
    result.resulting_entries = candidate_count(target);
    return result;
}

const char *dictionary_transfer_status_text(dictionary_transfer_status_t status)
{
    switch (status) {
    case dictionary_transfer_status_t::OK: return "OK";
    case dictionary_transfer_status_t::SOURCE_NOT_FOUND: return "Dictionary file not found";
    case dictionary_transfer_status_t::TARGET_UNAVAILABLE: return "User dictionary storage unavailable";
    case dictionary_transfer_status_t::INVALID_FORMAT: return "Invalid SKK dictionary format";
    case dictionary_transfer_status_t::IO_ERROR: return "SD card I/O error";
    default: return "Unknown error";
    }
}
