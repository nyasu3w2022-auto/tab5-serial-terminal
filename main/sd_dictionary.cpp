/*
 * sd_dictionary.cpp — Tab5 microSD mount and SKK user-dictionary transfers.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sd_dictionary.h"

#include <errno.h>
#include <sys/stat.h>

#include "esp_vfs_fat.h"
#include "m5tab5_pinmap.h"
#include "sdmmc_cmd.h"
#include "sdmmc_host.h"

namespace {

sdmmc_card_t *s_card = nullptr;

bool ensure_exchange_directory()
{
    if (mkdir(SD_DICTIONARY_DIRECTORY, 0775) == 0) return true;
    return errno == EEXIST;
}

}  // namespace

esp_err_t sd_dictionary_mount()
{
    if (s_card != nullptr) return ESP_OK;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = m5::tab5::M5TAB5_PIN_SDMMC_CLK;
    slot_config.cmd = m5::tab5::M5TAB5_PIN_SDMMC_CMD;
    slot_config.d0 = m5::tab5::M5TAB5_PIN_SDMMC_D0;
    slot_config.d1 = m5::tab5::M5TAB5_PIN_SDMMC_D1;
    slot_config.d2 = m5::tab5::M5TAB5_PIN_SDMMC_D2;
    slot_config.d3 = m5::tab5::M5TAB5_PIN_SDMMC_D3;
    slot_config.width = 4;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 2;
    mount_config.allocation_unit_size = 16 * 1024;

    return esp_vfs_fat_sdmmc_mount(SD_DICTIONARY_MOUNT_POINT, &host, &slot_config,
                                   &mount_config, &s_card);
}

void sd_dictionary_unmount()
{
    if (s_card == nullptr) return;
    esp_vfs_fat_sdcard_unmount(SD_DICTIONARY_MOUNT_POINT, s_card);
    s_card = nullptr;
}

bool sd_dictionary_is_mounted()
{
    return s_card != nullptr;
}

dictionary_transfer_result_t sd_dictionary_export_user(const char *user_dictionary_path)
{
    if (!sd_dictionary_is_mounted() && sd_dictionary_mount() != ESP_OK) {
        return {dictionary_transfer_status_t::TARGET_UNAVAILABLE, 0, 0};
    }
    if (!ensure_exchange_directory()) {
        sd_dictionary_unmount();
        return {dictionary_transfer_status_t::IO_ERROR, 0, 0};
    }
    dictionary_transfer_result_t result = dictionary_transfer_export(
        user_dictionary_path, SD_DICTIONARY_FILE_PATH);
    sd_dictionary_unmount();
    return result;
}

dictionary_transfer_result_t sd_dictionary_import_user(const char *user_dictionary_path,
                                                       dictionary_transfer_mode_t mode)
{
    if (!sd_dictionary_is_mounted() && sd_dictionary_mount() != ESP_OK) {
        return {dictionary_transfer_status_t::TARGET_UNAVAILABLE, 0, 0};
    }
    dictionary_transfer_result_t result = dictionary_transfer_import(
        SD_DICTIONARY_FILE_PATH, user_dictionary_path, mode);
    sd_dictionary_unmount();
    return result;
}
