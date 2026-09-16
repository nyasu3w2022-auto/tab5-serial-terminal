#pragma once
/*
 * dictionary_transfer.h — Safe import/export of UTF-8/LF SKK user dictionaries.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

enum class dictionary_transfer_mode_t : unsigned char {
    MERGE,
    REPLACE,
};

enum class dictionary_transfer_status_t : unsigned char {
    OK,
    SOURCE_NOT_FOUND,
    TARGET_UNAVAILABLE,
    INVALID_FORMAT,
    IO_ERROR,
};

/** Stage at which a transfer stopped; used for concise UI and serial diagnostics. */
enum class dictionary_transfer_stage_t : unsigned char {
    NONE,
    SOURCE_READ,
    TARGET_READ,
    TEMPORARY_OPEN,
    TEMPORARY_WRITE,
    TEMPORARY_CLOSE,
    TARGET_BACKUP,
    TARGET_RENAME,
    BACKUP_CLEANUP,
    SD_MOUNT,
    SD_DIRECTORY,
};

struct dictionary_transfer_result_t {
    dictionary_transfer_status_t status = dictionary_transfer_status_t::OK;
    size_t source_entries = 0;
    size_t resulting_entries = 0;
    dictionary_transfer_stage_t stage = dictionary_transfer_stage_t::NONE;
    int system_errno = 0;
};

/** Copy a user dictionary to target_path through a temporary file then rename. */
dictionary_transfer_result_t dictionary_transfer_export(const char *user_dictionary_path,
                                                        const char *target_path);

/**
 * Import an UTF-8/LF SKK user dictionary.
 * MERGE retains current candidate order and appends non-duplicate imported candidates.
 * REPLACE discards the current user dictionary only after input validation succeeds.
 */
dictionary_transfer_result_t dictionary_transfer_import(const char *source_path,
                                                        const char *user_dictionary_path,
                                                        dictionary_transfer_mode_t mode);

/** Human-readable status used by the local Dictionary Editor. */
const char *dictionary_transfer_status_text(dictionary_transfer_status_t status);

/** Short processing-stage name for diagnostics. */
const char *dictionary_transfer_stage_text(dictionary_transfer_stage_t stage);
