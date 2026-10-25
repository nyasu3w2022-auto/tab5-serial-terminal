/*
 * sd_dictionary.cpp — Shared Tab5 microSD session and SKK dictionary transfers.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sd_dictionary.h"

#include <cerrno>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "m5tab5_pinmap.h"
#include "sd_serial_log.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

namespace {

constexpr const char *TAG = "sd_dictionary";
constexpr uint32_t SD_LOGGER_PAUSE_TIMEOUT_MS = 3000;

sdmmc_card_t *s_card = nullptr;
SemaphoreHandle_t s_sd_mutex = nullptr;

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

bool begin_dictionary_sd_operation()
{
    // The logger owns its FILE*. Ask it to fsync/close before this LVGL task
    // touches the same FAT volume. Incoming RX remains queued in PSRAM.
    if (!sd_serial_log_pause_for_sd_operation(SD_LOGGER_PAUSE_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Serial logger did not pause before dictionary SD operation");
        return false;
    }
    if (!sd_dictionary_lock(SD_LOGGER_PAUSE_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Timed out acquiring shared SD session for dictionary operation");
        sd_serial_log_resume_after_sd_operation();
        return false;
    }
    return true;
}

void end_dictionary_sd_operation()
{
    sd_dictionary_unmount_locked();
    sd_dictionary_unlock();
    sd_serial_log_resume_after_sd_operation();
}

}  // namespace

bool sd_dictionary_init()
{
    if (s_sd_mutex != nullptr) return true;

    // app_main calls this before creating the SD logger task, so no two tasks
    // can allocate the mutex concurrently. Subsequent callers only observe
    // the already-created object.
    s_sd_mutex = xSemaphoreCreateRecursiveMutex();

    if (s_sd_mutex == nullptr) {
        ESP_LOGE(TAG, "Cannot create shared SD mutex");
        return false;
    }
    return true;
}

bool sd_dictionary_lock(uint32_t timeout_ms)
{
    if (!sd_dictionary_init()) return false;
    const TickType_t ticks = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_sd_mutex, ticks) == pdTRUE;
}

void sd_dictionary_unlock()
{
    if (s_sd_mutex != nullptr) xSemaphoreGiveRecursive(s_sd_mutex);
}

esp_err_t sd_dictionary_mount_locked()
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
    // The logger, its append file, and dictionary temporary-file operations
    // can coexist only while explicitly serialized by this module. Four VFS
    // file objects leave headroom for the atomic dictionary transfer path.
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;
    // Tab5 exposes no card-detect GPIO. Ask FatFs to check media status during
    // I/O so a physical removal becomes a detected, recoverable logger error.
    mount_config.disk_status_check_enable = true;

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

esp_err_t sd_dictionary_mount()
{
    if (!sd_dictionary_lock(0)) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = sd_dictionary_mount_locked();
    sd_dictionary_unlock();
    return err;
}

void sd_dictionary_unmount_locked()
{
    if (s_card == nullptr) return;
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_DICTIONARY_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unmount failed: err=0x%x (%s)", (unsigned)err, esp_err_to_name(err));
    }
    s_card = nullptr;
}

void sd_dictionary_unmount()
{
    if (!sd_dictionary_lock(0)) return;
    sd_dictionary_unmount_locked();
    sd_dictionary_unlock();
}

bool sd_dictionary_is_mounted()
{
    if (!sd_dictionary_lock(1000)) return false;
    const bool mounted = s_card != nullptr;
    sd_dictionary_unlock();
    return mounted;
}

esp_err_t sd_dictionary_get_space_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (s_card == nullptr) return ESP_ERR_INVALID_STATE;
    return esp_vfs_fat_info(SD_DICTIONARY_MOUNT_POINT, total_bytes, free_bytes);
}

dictionary_transfer_result_t sd_dictionary_export_user(const char *user_dictionary_path)
{
    if (!begin_dictionary_sd_operation()) {
        return sd_failure(dictionary_transfer_stage_t::SD_MOUNT, ETIMEDOUT);
    }

    dictionary_transfer_result_t result = {};
    const esp_err_t mount_err = sd_dictionary_mount_locked();
    if (mount_err != ESP_OK) {
        result = sd_failure(dictionary_transfer_stage_t::SD_MOUNT, (int)mount_err);
    } else {
        int mkdir_errno = 0;
        if (!ensure_exchange_directory(&mkdir_errno)) {
            ESP_LOGE(TAG, "cannot create %s: errno=%d", SD_DICTIONARY_DIRECTORY, mkdir_errno);
            result = sd_failure(dictionary_transfer_stage_t::SD_DIRECTORY, mkdir_errno);
            result.status = dictionary_transfer_status_t::IO_ERROR;
        } else {
            result = dictionary_transfer_export(user_dictionary_path, SD_DICTIONARY_FILE_PATH);
        }
    }

    log_transfer_result("export", result);
    end_dictionary_sd_operation();
    return result;
}

dictionary_transfer_result_t sd_dictionary_import_user(const char *user_dictionary_path,
                                                       dictionary_transfer_mode_t mode)
{
    if (!begin_dictionary_sd_operation()) {
        return sd_failure(dictionary_transfer_stage_t::SD_MOUNT, ETIMEDOUT);
    }

    dictionary_transfer_result_t result = {};
    const esp_err_t mount_err = sd_dictionary_mount_locked();
    if (mount_err != ESP_OK) {
        result = sd_failure(dictionary_transfer_stage_t::SD_MOUNT, (int)mount_err);
    } else {
        result = dictionary_transfer_import(SD_DICTIONARY_FILE_PATH, user_dictionary_path, mode);
    }

    log_transfer_result(mode == dictionary_transfer_mode_t::MERGE ? "import merge" : "import replace",
                        result);
    end_dictionary_sd_operation();
    return result;
}
