#pragma once
/*
 * sd_dictionary.h — Tab5 microSD mount and SKK user-dictionary transfer API.
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_err.h"
#include "dictionary_transfer.h"

/** microSD mount point and portable user dictionary exchange path. */
constexpr const char *SD_DICTIONARY_MOUNT_POINT = "/sd";
constexpr const char *SD_DICTIONARY_DIRECTORY = "/sd/TAB5-SKK";
constexpr const char *SD_DICTIONARY_FILE_PATH = "/sd/TAB5-SKK/SKK-JISYO.user.txt";

/** Mount Tab5's microSD card as a FAT filesystem, without formatting it. */
esp_err_t sd_dictionary_mount();

/** Unmount the microSD card if it is currently mounted. */
void sd_dictionary_unmount();

/** True if the FAT filesystem is mounted at SD_DICTIONARY_MOUNT_POINT. */
bool sd_dictionary_is_mounted();

/** Export current SPIFFS user dictionary to the standard SD exchange file. */
dictionary_transfer_result_t sd_dictionary_export_user(const char *user_dictionary_path);

/** Import the standard SD exchange file into the SPIFFS user dictionary. */
dictionary_transfer_result_t sd_dictionary_import_user(const char *user_dictionary_path,
                                                       dictionary_transfer_mode_t mode);
