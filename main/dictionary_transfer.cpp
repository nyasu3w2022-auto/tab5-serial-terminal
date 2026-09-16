/*
 * dictionary_transfer.cpp — Safe import/export of UTF-8/LF SKK user dictionaries.
 *
 * SPDX-License-Identifier: MIT
 */

#include "dictionary_transfer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr size_t MAX_DICT_LINE_BYTES = 512;
using dictionary_t = std::map<std::string, std::vector<std::string>>;

void set_failure(dictionary_transfer_result_t *result, dictionary_transfer_status_t status,
                 dictionary_transfer_stage_t stage, int system_errno)
{
    if (result == nullptr) return;
    result->status = status;
    result->stage = stage;
    result->system_errno = system_errno;
}

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
                                             size_t *entry_count, bool missing_is_empty,
                                             int *system_errno)
{
    if (system_errno != nullptr) *system_errno = 0;
    if (dictionary == nullptr || entry_count == nullptr || path == nullptr || path[0] == '\0') {
        return dictionary_transfer_status_t::TARGET_UNAVAILABLE;
    }
    dictionary->clear();
    *entry_count = 0;

    errno = 0;
    FILE *in = fopen(path, "rb");
    if (in == nullptr) {
        if (system_errno != nullptr) *system_errno = errno;
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
    const int read_errno = ferror(in) != 0 ? errno : 0;
    const int close_result = fclose(in);
    if (read_errno != 0 || close_result != 0) {
        if (system_errno != nullptr) *system_errno = read_errno != 0 ? read_errno : errno;
        return dictionary_transfer_status_t::IO_ERROR;
    }

    for (const auto &entry : *dictionary) *entry_count += entry.second.size();
    return dictionary_transfer_status_t::OK;
}

std::string sidecar_path(const char *path, const char *filename)
{
    const std::string target(path ? path : "");
    const size_t separator = target.find_last_of('/');
    return separator == std::string::npos ? std::string(filename)
                                           : target.substr(0, separator + 1) + filename;
}

dictionary_transfer_result_t write_dictionary_atomic(const char *path, const dictionary_t &dictionary)
{
    dictionary_transfer_result_t result = {};
    if (path == nullptr || path[0] == '\0') {
        set_failure(&result, dictionary_transfer_status_t::TARGET_UNAVAILABLE,
                    dictionary_transfer_stage_t::NONE, 0);
        return result;
    }

    // FATFS long-file-name support is optional. Keep recovery names 8.3-compatible.
    const std::string temporary_path = sidecar_path(path, "SKKTEMP.TMP");
    const std::string backup_path = sidecar_path(path, "SKKBAK.BAK");
    remove(temporary_path.c_str());

    errno = 0;
    FILE *out = fopen(temporary_path.c_str(), "wb");
    if (out == nullptr) {
        set_failure(&result, dictionary_transfer_status_t::IO_ERROR,
                    dictionary_transfer_stage_t::TEMPORARY_OPEN, errno);
        return result;
    }

    bool write_ok = fputs("; TAB5 SKK user dictionary (UTF-8/LF)\n", out) >= 0;
    for (const auto &entry : dictionary) {
        if (!write_ok) break;
        if (fprintf(out, "%s ", entry.first.c_str()) < 0) {
            write_ok = false;
            break;
        }
        for (const std::string &candidate : entry.second) {
            if (fprintf(out, "/%s", candidate.c_str()) < 0) {
                write_ok = false;
                break;
            }
        }
        if (!write_ok || fputs("/\n", out) < 0) {
            write_ok = false;
            break;
        }
    }

    const int write_errno = write_ok ? 0 : errno;
    const int flush_result = fflush(out);
    const int flush_errno = flush_result == 0 ? 0 : errno;
    const int close_result = fclose(out);
    const int close_errno = close_result == 0 ? 0 : errno;
    if (!write_ok || flush_result != 0 || close_result != 0) {
        remove(temporary_path.c_str());
        const dictionary_transfer_stage_t stage = !write_ok
            ? dictionary_transfer_stage_t::TEMPORARY_WRITE
            : (flush_result != 0 ? dictionary_transfer_stage_t::TEMPORARY_WRITE
                                 : dictionary_transfer_stage_t::TEMPORARY_CLOSE);
        const int error = write_errno != 0 ? write_errno
                        : (flush_errno != 0 ? flush_errno : close_errno);
        set_failure(&result, dictionary_transfer_status_t::IO_ERROR, stage, error);
        return result;
    }

    struct stat existing = {};
    errno = 0;
    const bool target_exists = stat(path, &existing) == 0;
    if (!target_exists && errno != ENOENT) {
        remove(temporary_path.c_str());
        set_failure(&result, dictionary_transfer_status_t::IO_ERROR,
                    dictionary_transfer_stage_t::TARGET_BACKUP, errno);
        return result;
    }

    bool moved_old_target = false;
    if (target_exists) {
        errno = 0;
        if (remove(backup_path.c_str()) != 0 && errno != ENOENT) {
            remove(temporary_path.c_str());
            set_failure(&result, dictionary_transfer_status_t::IO_ERROR,
                        dictionary_transfer_stage_t::TARGET_BACKUP, errno);
            return result;
        }
        errno = 0;
        if (rename(path, backup_path.c_str()) != 0) {
            remove(temporary_path.c_str());
            set_failure(&result, dictionary_transfer_status_t::IO_ERROR,
                        dictionary_transfer_stage_t::TARGET_BACKUP, errno);
            return result;
        }
        moved_old_target = true;
    }

    errno = 0;
    if (rename(temporary_path.c_str(), path) != 0) {
        const int rename_errno = errno;
        if (moved_old_target) rename(backup_path.c_str(), path);
        remove(temporary_path.c_str());
        set_failure(&result, dictionary_transfer_status_t::IO_ERROR,
                    dictionary_transfer_stage_t::TARGET_RENAME, rename_errno);
        return result;
    }

    if (moved_old_target) {
        errno = 0;
        if (remove(backup_path.c_str()) != 0) {
            set_failure(&result, dictionary_transfer_status_t::IO_ERROR,
                        dictionary_transfer_stage_t::BACKUP_CLEANUP, errno);
            return result;
        }
    }
    return result;
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
    int system_errno = 0;
    result.status = load_dictionary(user_dictionary_path, &source, &result.source_entries, true,
                                    &system_errno);
    if (result.status != dictionary_transfer_status_t::OK) {
        result.stage = dictionary_transfer_stage_t::SOURCE_READ;
        result.system_errno = system_errno;
        return result;
    }

    result = write_dictionary_atomic(target_path, source);
    result.source_entries = candidate_count(source);
    result.resulting_entries = result.status == dictionary_transfer_status_t::OK
        ? result.source_entries : 0;
    return result;
}

dictionary_transfer_result_t dictionary_transfer_import(const char *source_path,
                                                        const char *user_dictionary_path,
                                                        dictionary_transfer_mode_t mode)
{
    dictionary_transfer_result_t result = {};
    dictionary_t imported;
    int system_errno = 0;
    result.status = load_dictionary(source_path, &imported, &result.source_entries, false,
                                    &system_errno);
    if (result.status != dictionary_transfer_status_t::OK) {
        result.stage = dictionary_transfer_stage_t::SOURCE_READ;
        result.system_errno = system_errno;
        return result;
    }

    dictionary_t target;
    if (mode == dictionary_transfer_mode_t::MERGE) {
        size_t existing_entries = 0;
        result.status = load_dictionary(user_dictionary_path, &target, &existing_entries, true,
                                        &system_errno);
        if (result.status != dictionary_transfer_status_t::OK) {
            result.stage = dictionary_transfer_stage_t::TARGET_READ;
            result.system_errno = system_errno;
            return result;
        }
        merge_dictionary(&target, imported);
    } else {
        target = imported;
    }

    result = write_dictionary_atomic(user_dictionary_path, target);
    result.source_entries = candidate_count(imported);
    result.resulting_entries = result.status == dictionary_transfer_status_t::OK
        ? candidate_count(target) : 0;
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

const char *dictionary_transfer_stage_text(dictionary_transfer_stage_t stage)
{
    switch (stage) {
    case dictionary_transfer_stage_t::NONE: return "none";
    case dictionary_transfer_stage_t::SOURCE_READ: return "source read";
    case dictionary_transfer_stage_t::TARGET_READ: return "target read";
    case dictionary_transfer_stage_t::TEMPORARY_OPEN: return "temporary open";
    case dictionary_transfer_stage_t::TEMPORARY_WRITE: return "temporary write";
    case dictionary_transfer_stage_t::TEMPORARY_CLOSE: return "temporary close";
    case dictionary_transfer_stage_t::TARGET_BACKUP: return "backup old file";
    case dictionary_transfer_stage_t::TARGET_RENAME: return "publish new file";
    case dictionary_transfer_stage_t::BACKUP_CLEANUP: return "cleanup backup";
    case dictionary_transfer_stage_t::SD_MOUNT: return "SD mount";
    case dictionary_transfer_stage_t::SD_DIRECTORY: return "create exchange directory";
    default: return "unknown";
    }
}
