/*
 * sd_dictionary.cpp — Tab5 microSD mount and SKK user-dictionary transfers.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sd_dictionary.h"

#include <cerrno>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "m5tab5_pinmap.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

namespace {

constexpr const char *TAG = "sd_dictionary";
sdmmc_card_t *s_card = nullptr;

bool ensure_exchange_directory(int *system_errno)
{
    if (system_errno != nullptr) *system_errno = 0;
    errno = 0;
    if (mkdir(SD_DICTIONARY_DIRECTORY, 0775) == 0) return true;
    if (errno == EEXIST) return true;
    if (system_errno != nullptr) *system_errno = errno;
    return false;
}

dictionary_transfer_result_t sd_failure(dictionary_transfer_stage_t stage, int code)
{
    dictionary_transfer_result_t result = {};
    result.status = dictionary_transfer_status_t::TARGET_UNAVAILABLE;
    result.stage = stage;
    result.system_errno = code;
    return result;
}

void log_transfer_result(const char *operation, const dictionary_transfer_result_t &result)
{
    // Keep the parameter referenced with no-op logging stubs used by host syntax tests.
    (void)operation;
    if (result.status == dictionary_transfer_status_t::OK) {
        ESP_LOGI(TAG, "%s succeeded: %u entries", operation,
                 (unsigned)result.resulting_entries);
        return;
    }
    ESP_LOGE(TAG, "%s failed: status=%s stage=%s code=%d", operation,
             dictionary_transfer_status_text(result.status),
             dictionary_transfer_stage_text(result.stage), result.system_errno);
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

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_DICTIONARY_MOUNT_POINT, &host, &slot_config,
                                                   &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: err=0x%x (%s)", (unsigned)err, esp_err_to_name(err));
        s_card = nullptr;
    } else {
        ESP_LOGI(TAG, "mounted FAT filesystem at %s", SD_DICTIONARY_MOUNT_POINT);
    }
    return err;
}

void sd_dictionary_unmount()
{
    if (s_card == nullptr) return;
    // ESP-IDF's esp_vfs_fat_sdcard_unmount() has no return value.
    esp_vfs_fat_sdcard_unmount(SD_DICTIONARY_MOUNT_POINT, s_card);
    s_card = nullptr;
}

bool sd_dictionary_is_mounted()
{
    return s_card != nullptr;
}

dictionary_transfer_result_t sd_dictionary_export_user(const char *user_dictionary_path)
{
    if (!sd_dictionary_is_mounted()) {
        const esp_err_t err = sd_dictionary_mount();
        if (err != ESP_OK) return sd_failure(dictionary_transfer_stage_t::SD_MOUNT, (int)err);
    }

    int mkdir_errno = 0;
    if (!ensure_exchange_directory(&mkdir_errno)) {
        ESP_LOGE(TAG, "cannot create %s: errno=%d", SD_DICTIONARY_DIRECTORY, mkdir_errno);
        sd_dictionary_unmount();
        dictionary_transfer_result_t result = sd_failure(dictionary_transfer_stage_t::SD_DIRECTORY,
                                                         mkdir_errno);
        result.status = dictionary_transfer_status_t::IO_ERROR;
        return result;
    }

    dictionary_transfer_result_t result = dictionary_transfer_export(
        user_dictionary_path, SD_DICTIONARY_FILE_PATH);
    log_transfer_result("export", result);
    sd_dictionary_unmount();
    return result;
}

dictionary_transfer_result_t sd_dictionary_import_user(const char *user_dictionary_path,
                                                       dictionary_transfer_mode_t mode)
{
    if (!sd_dictionary_is_mounted()) {
        const esp_err_t err = sd_dictionary_mount();
        if (err != ESP_OK) return sd_failure(dictionary_transfer_stage_t::SD_MOUNT, (int)err);
    }

    dictionary_transfer_result_t result = dictionary_transfer_import(
        SD_DICTIONARY_FILE_PATH, user_dictionary_path, mode);
    log_transfer_result(mode == dictionary_transfer_mode_t::MERGE ? "import merge" : "import replace",
                        result);
    sd_dictionary_unmount();
    return result;
}
