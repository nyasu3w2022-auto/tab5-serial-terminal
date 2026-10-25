/*
 * sd_serial_log.cpp — Asynchronous raw serial RX logging to Tab5 microSD.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sd_serial_log.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "sd_dictionary.h"

namespace {

constexpr const char *TAG = "sd_serial_log";
constexpr size_t LOG_RING_BYTES = 256U * 1024U;
constexpr size_t LOG_WRITE_MAX_BYTES = 16U * 1024U;
constexpr size_t LOG_SYNC_BYTES = 64U * 1024U;
constexpr uint32_t LOG_SYNC_MS = 2000;
constexpr uint32_t LOG_SPACE_CHECK_MS = 10000;
constexpr size_t LOG_SPACE_CHECK_BYTES = 1U * 1024U * 1024U;
constexpr uint64_t LOG_FREE_SPACE_RESERVE = 2ULL * 1024ULL * 1024ULL;
constexpr uint32_t LOG_RETRY_MS = 30000;
constexpr uint32_t LOG_PAUSE_POLL_MS = 25;
constexpr UBaseType_t LOG_TASK_PRIORITY = 4;
constexpr uint32_t LOG_TASK_STACK = 4096;

static RingbufHandle_t s_ring = nullptr;
static TaskHandle_t s_task = nullptr;
static FILE *s_file = nullptr;
static volatile bool s_enabled = false;
static volatile bool s_pause_requested = false;
static volatile bool s_pause_acknowledged = false;
static volatile bool s_initialized = false;
static volatile bool s_status_changed = false;
static volatile sd_serial_log_status_t s_status = SD_SERIAL_LOG_OFF;
static volatile bool s_drop_seen = false;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static bool status_is_retryable(sd_serial_log_status_t status)
{
    return status == SD_SERIAL_LOG_NO_SD || status == SD_SERIAL_LOG_IO;
}

static bool status_allows_open(sd_serial_log_status_t status)
{
    // ON with no FILE* is the normal post-dictionary-pause state. It must
    // reopen immediately rather than being treated as a terminal condition.
    return status == SD_SERIAL_LOG_ON || status_is_retryable(status);
}

static bool status_is_terminal_failure(sd_serial_log_status_t status)
{
    return status == SD_SERIAL_LOG_LOW || status == SD_SERIAL_LOG_FULL ||
           status == SD_SERIAL_LOG_RO || status == SD_SERIAL_LOG_IO;
}

static void set_status(sd_serial_log_status_t status)
{
    portENTER_CRITICAL(&s_state_mux);
    if (s_status != status) {
        s_status = status;
        s_status_changed = true;
    }
    portEXIT_CRITICAL(&s_state_mux);
}

static bool is_enabled()
{
    portENTER_CRITICAL(&s_state_mux);
    const bool enabled = s_enabled;
    portEXIT_CRITICAL(&s_state_mux);
    return enabled;
}

static bool is_pause_requested()
{
    portENTER_CRITICAL(&s_state_mux);
    const bool requested = s_pause_requested;
    portEXIT_CRITICAL(&s_state_mux);
    return requested;
}

static void set_pause_acknowledged(bool acknowledged)
{
    portENTER_CRITICAL(&s_state_mux);
    s_pause_acknowledged = acknowledged;
    portEXIT_CRITICAL(&s_state_mux);
}

static bool pause_acknowledged()
{
    portENTER_CRITICAL(&s_state_mux);
    const bool acknowledged = s_pause_acknowledged;
    portEXIT_CRITICAL(&s_state_mux);
    return acknowledged;
}

static void notify_worker()
{
    if (s_task != nullptr) xTaskNotifyGive(s_task);
}

static sd_serial_log_status_t status_from_errno(int error, bool partial_write)
{
    // A short write without an I/O errno is FatFs' disk-full indication. If
    // the filesystem supplied EIO/ETIMEDOUT/etc., preserve that stronger media
    // diagnosis instead of mislabeling every partial result as capacity-full.
    if (error == ENOSPC || (partial_write && error == 0)) return SD_SERIAL_LOG_FULL;
    if (error == EACCES || error == EROFS || error == EPERM) return SD_SERIAL_LOG_RO;
    return SD_SERIAL_LOG_IO;
}

static bool flush_and_sync_locked()
{
    if (s_file == nullptr) return true;
    errno = 0;
    if (fflush(s_file) != 0) {
        const int flush_errno = errno;
        ESP_LOGW(TAG, "Serial log fflush failed: errno=%d", flush_errno);
        set_status(status_from_errno(flush_errno, false));
        return false;
    }
    errno = 0;
    if (fsync(fileno(s_file)) != 0) {
        const int sync_errno = errno;
        ESP_LOGW(TAG, "Serial log fsync failed: errno=%d", sync_errno);
        set_status(status_from_errno(sync_errno, false));
        return false;
    }
    return true;
}

static bool ensure_log_directory_locked(int *system_errno)
{
    if (system_errno != nullptr) *system_errno = 0;
    errno = 0;
    if (mkdir(SD_SERIAL_LOG_DIRECTORY, 0775) == 0 || errno == EEXIST) return true;
    if (system_errno != nullptr) *system_errno = errno;
    return false;
}

static void close_file_locked(bool sync_before_close)
{
    if (s_file == nullptr) return;

    if (sync_before_close) (void)flush_and_sync_locked();
    if (fclose(s_file) != 0) {
        const int close_errno = errno;
        ESP_LOGW(TAG, "close log failed: errno=%d", close_errno);
        // Never hide a prior FULL/RO/IO classification merely because fclose
        // reports a secondary error while releasing the same failed stream.
        if (!status_is_terminal_failure(sd_serial_log_status())) {
            set_status(status_from_errno(close_errno, false));
        }
    }
    s_file = nullptr;
}

static void close_file(bool sync_before_close)
{
    if (!sd_dictionary_lock(1000)) {
        ESP_LOGW(TAG, "Timed out acquiring SD lock while closing log");
        set_status(SD_SERIAL_LOG_IO);
        return;
    }
    close_file_locked(sync_before_close);
    sd_dictionary_unlock();
}

static void close_file_and_unmount(bool sync_before_close)
{
    if (!sd_dictionary_lock(1000)) {
        ESP_LOGW(TAG, "Timed out acquiring SD lock while invalidating log session");
        set_status(SD_SERIAL_LOG_IO);
        return;
    }
    close_file_locked(sync_before_close);
    // A failed FATFS write can leave its FILE object aborted. Dropping the
    // complete mount makes a later NoSD/IO retry re-probe the physical card
    // rather than reusing stale SDMMC/VFS state after removal or I/O failure.
    sd_dictionary_unmount_locked();
    sd_dictionary_unlock();
}

static bool check_free_space_locked()
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    const esp_err_t err = sd_dictionary_get_space_info(&total, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot query SD free space: %s", esp_err_to_name(err));
        set_status(SD_SERIAL_LOG_IO);
        return false;
    }
    (void)total;
    if (free_bytes <= LOG_FREE_SPACE_RESERVE) {
        ESP_LOGW(TAG, "Stopping serial log: only %llu free bytes remain (reserve=%llu)",
                 (unsigned long long)free_bytes,
                 (unsigned long long)LOG_FREE_SPACE_RESERVE);
        set_status(SD_SERIAL_LOG_LOW);
        return false;
    }
    return true;
}

static bool open_log_file()
{
    if (!sd_dictionary_lock(1000)) {
        ESP_LOGW(TAG, "Timed out acquiring SD lock while opening log");
        set_status(SD_SERIAL_LOG_IO);
        return false;
    }

    bool success = false;
    const esp_err_t mount_err = sd_dictionary_mount_locked();
    if (mount_err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount for serial log failed: %s", esp_err_to_name(mount_err));
        set_status(SD_SERIAL_LOG_NO_SD);
    } else {
        int directory_errno = 0;
        if (!ensure_log_directory_locked(&directory_errno)) {
            ESP_LOGW(TAG, "Cannot create %s: errno=%d", SD_SERIAL_LOG_DIRECTORY, directory_errno);
            set_status(status_from_errno(directory_errno, false));
        } else if (!check_free_space_locked()) {
            // check_free_space_locked has already set a user-visible state.
        } else {
            errno = 0;
            s_file = fopen(SD_SERIAL_LOG_FILE_PATH, "ab");
            if (s_file == nullptr) {
                const int open_errno = errno;
                ESP_LOGW(TAG, "Cannot open %s: errno=%d", SD_SERIAL_LOG_FILE_PATH, open_errno);
                set_status(status_from_errno(open_errno, false));
            } else {
                set_status(SD_SERIAL_LOG_ON);
                success = true;
                ESP_LOGI(TAG, "Serial RX logging to %s", SD_SERIAL_LOG_FILE_PATH);
            }
        }
    }

    if (!success) {
        // Directory, capacity, or append-open failures can leave a mounted
        // VFS/card object that is no longer trustworthy (especially after a
        // remove/reinsert race). A later retry must perform a complete mount.
        sd_dictionary_unmount_locked();
    }
    sd_dictionary_unlock();
    return success;
}

static bool write_log_chunk(void *data, size_t len, size_t *bytes_until_sync,
                            size_t *bytes_until_space_check, TickType_t *last_sync,
                            TickType_t *last_space_check)
{
    if (data == nullptr || len == 0 || s_file == nullptr) return false;
    if (!sd_dictionary_lock(1000)) {
        ESP_LOGW(TAG, "Timed out acquiring SD lock for serial log write");
        set_status(SD_SERIAL_LOG_IO);
        return false;
    }

    if (is_pause_requested() || !is_enabled()) {
        sd_dictionary_unlock();
        return false;
    }

    errno = 0;
    const size_t written = fwrite(data, 1, len, s_file);
    const int write_errno = errno;
    if (written != len || ferror(s_file) != 0) {
        const bool partial_write = written < len;
        ESP_LOGW(TAG, "Serial log write failed: requested=%u written=%u errno=%d",
                 (unsigned)len, (unsigned)written, write_errno);
        set_status(status_from_errno(write_errno, partial_write));
        sd_dictionary_unlock();
        return false;
    }

    *bytes_until_sync += written;
    *bytes_until_space_check += written;
    const TickType_t now = xTaskGetTickCount();
    const bool sync_due = *bytes_until_sync >= LOG_SYNC_BYTES ||
                          (TickType_t)(now - *last_sync) >= pdMS_TO_TICKS(LOG_SYNC_MS);
    if (sync_due) {
        if (!flush_and_sync_locked()) {
            sd_dictionary_unlock();
            return false;
        }
        *bytes_until_sync = 0;
        *last_sync = now;
    }

    const bool space_due = *bytes_until_space_check >= LOG_SPACE_CHECK_BYTES ||
                           (TickType_t)(now - *last_space_check) >=
                               pdMS_TO_TICKS(LOG_SPACE_CHECK_MS);
    if (space_due) {
        *bytes_until_space_check = 0;
        *last_space_check = now;
        if (!check_free_space_locked()) {
            sd_dictionary_unlock();
            return false;
        }
    }

    sd_dictionary_unlock();
    return true;
}

static void wait_while_paused()
{
    close_file(true);
    set_pause_acknowledged(true);
    while (is_enabled() && is_pause_requested()) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LOG_PAUSE_POLL_MS));
    }
    set_pause_acknowledged(false);
}

static void serial_log_task(void *)
{
    size_t bytes_until_sync = 0;
    size_t bytes_until_space_check = 0;
    TickType_t last_sync = xTaskGetTickCount();
    TickType_t last_space_check = last_sync;
    TickType_t next_retry = 0;

    while (true) {
        if (!is_enabled()) {
            close_file_and_unmount(true);
            set_pause_acknowledged(false);
            set_status(SD_SERIAL_LOG_OFF);
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
            continue;
        }

        if (is_pause_requested()) {
            wait_while_paused();
            bytes_until_sync = 0;
            bytes_until_space_check = 0;
            last_sync = xTaskGetTickCount();
            last_space_check = last_sync;
            continue;
        }

        if (s_file == nullptr) {
            const sd_serial_log_status_t current = sd_serial_log_status();
            const TickType_t now = xTaskGetTickCount();
            if (!status_allows_open(current)) {
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
                continue;
            }
            if (status_is_retryable(current) && next_retry != 0 &&
                (TickType_t)(now - next_retry) < pdMS_TO_TICKS(LOG_RETRY_MS)) {
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
                continue;
            }
            if (!open_log_file()) {
                next_retry = now;
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
                continue;
            }
            next_retry = 0;
            bytes_until_sync = 0;
            bytes_until_space_check = 0;
            last_sync = now;
            last_space_check = now;
        }

        size_t received_len = 0;
        void *received = xRingbufferReceiveUpTo(s_ring, &received_len,
                                                pdMS_TO_TICKS(100), LOG_WRITE_MAX_BYTES);
        if (received == nullptr || received_len == 0) {
            const TickType_t now = xTaskGetTickCount();
            if ((TickType_t)(now - last_sync) >= pdMS_TO_TICKS(LOG_SYNC_MS)) {
                if (!sd_dictionary_lock(1000)) {
                    set_status(SD_SERIAL_LOG_IO);
                    close_file_and_unmount(false);
                } else {
                    if (!flush_and_sync_locked()) {
                        sd_dictionary_unlock();
                        close_file_and_unmount(false);
                    } else {
                        last_sync = now;
                        sd_dictionary_unlock();
                    }
                }
            }
            continue;
        }

        const bool write_ok = write_log_chunk(received, received_len, &bytes_until_sync,
                                              &bytes_until_space_check, &last_sync,
                                              &last_space_check);
        vRingbufferReturnItem(s_ring, received);
        if (!write_ok) close_file_and_unmount(false);
    }
}

}  // namespace

bool sd_serial_log_init()
{
    if (s_initialized) return s_ring != nullptr && s_task != nullptr;

    s_ring = xRingbufferCreateWithCaps(LOG_RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (s_ring == nullptr) {
        // The terminal itself keeps operating. Do not fall back to internal RAM,
        // where a 256 KiB logger would contend with USB, LVGL, and I2S buffers.
        ESP_LOGE(TAG, "Cannot allocate %u-byte PSRAM serial log ring", (unsigned)LOG_RING_BYTES);
        set_status(SD_SERIAL_LOG_MEM);
        s_initialized = true;
        return false;
    }

    const BaseType_t task_ok = xTaskCreate(serial_log_task, "sd_serial_log", LOG_TASK_STACK,
                                           nullptr, LOG_TASK_PRIORITY, &s_task);
    if (task_ok != pdPASS || s_task == nullptr) {
        ESP_LOGE(TAG, "Cannot create SD serial log worker");
        vRingbufferDeleteWithCaps(s_ring);
        s_ring = nullptr;
        set_status(SD_SERIAL_LOG_MEM);
        s_initialized = true;
        return false;
    }

    s_initialized = true;
    set_status(SD_SERIAL_LOG_OFF);
    ESP_LOGI(TAG, "Serial log worker ready (PSRAM queue=%u bytes)", (unsigned)LOG_RING_BYTES);
    return true;
}

void sd_serial_log_configure(bool enabled)
{
    if (!s_initialized || s_ring == nullptr || s_task == nullptr) {
        if (enabled) set_status(SD_SERIAL_LOG_MEM);
        return;
    }

    portENTER_CRITICAL(&s_state_mux);
    const bool was_enabled = s_enabled;
    s_enabled = enabled;
    s_pause_requested = false;
    s_pause_acknowledged = false;
    if (!enabled || !was_enabled) {
        s_drop_seen = false;
    }
    // ON is published only after the worker has mounted the card, reserved
    // free space, and opened the append file. Until then, enabled means the
    // worker must attempt a mount.
    if (!enabled) {
        s_status = SD_SERIAL_LOG_OFF;
        s_status_changed = true;
    } else if (!was_enabled) {
        s_status = SD_SERIAL_LOG_NO_SD;
        s_status_changed = true;
    }
    portEXIT_CRITICAL(&s_state_mux);
    notify_worker();
}

bool sd_serial_log_is_enabled()
{
    return is_enabled();
}

void sd_serial_log_enqueue_rx(const uint8_t *data, size_t len)
{
    if (data == nullptr || len == 0 || !is_enabled() || s_ring == nullptr) return;
    // Once a media error is known, deliberately stop accepting bytes instead
    // of filling PSRAM with data that cannot be committed. A pause keeps the
    // primary state ON, so short dictionary operations still retain bytes.
    if (sd_serial_log_status() != SD_SERIAL_LOG_ON) return;
    if (xRingbufferSend(s_ring, data, len, 0) != pdTRUE) {
        // The logger is deliberately lossy before the serial terminal is. RX
        // callbacks and the main parser must never wait for SD-card throughput.
        portENTER_CRITICAL(&s_state_mux);
        s_drop_seen = true;
        s_status_changed = true;
        portEXIT_CRITICAL(&s_state_mux);
    }
}

const char *sd_serial_log_status_text()
{
    static const char *const normal_text[] = {
        "OFF", "ON", "NoSD", "LOW", "FULL", "RO", "IO", "MEM"
    };
    const sd_serial_log_status_t status = sd_serial_log_status();
    portENTER_CRITICAL(&s_state_mux);
    const bool dropped = s_drop_seen;
    portEXIT_CRITICAL(&s_state_mux);
    if (dropped && status != SD_SERIAL_LOG_OFF && status != SD_SERIAL_LOG_MEM) {
        // Retain the primary media state too: for example NoSD+DROP must keep
        // retrying card mounts rather than being masked by queue pressure.
        switch (status) {
        case SD_SERIAL_LOG_ON:   return "ON+DROP";
        case SD_SERIAL_LOG_NO_SD:return "NoSD+DROP";
        case SD_SERIAL_LOG_LOW:  return "LOW+DROP";
        case SD_SERIAL_LOG_FULL: return "FULL+DROP";
        case SD_SERIAL_LOG_RO:   return "RO+DROP";
        case SD_SERIAL_LOG_IO:   return "IO+DROP";
        default:                 break;
        }
    }
    const size_t index = (size_t)status;
    return index < sizeof(normal_text) / sizeof(normal_text[0]) ? normal_text[index] : "IO";
}

sd_serial_log_status_t sd_serial_log_status()
{
    portENTER_CRITICAL(&s_state_mux);
    const sd_serial_log_status_t status = s_status;
    portEXIT_CRITICAL(&s_state_mux);
    return status;
}

bool sd_serial_log_take_status_changed()
{
    portENTER_CRITICAL(&s_state_mux);
    const bool changed = s_status_changed;
    s_status_changed = false;
    portEXIT_CRITICAL(&s_state_mux);
    return changed;
}

bool sd_serial_log_pause_for_sd_operation(uint32_t timeout_ms)
{
    if (!s_initialized || !is_enabled() || s_task == nullptr) return true;

    portENTER_CRITICAL(&s_state_mux);
    s_pause_requested = true;
    s_pause_acknowledged = false;
    portEXIT_CRITICAL(&s_state_mux);
    notify_worker();

    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    while (!pause_acknowledged()) {
        if ((TickType_t)(xTaskGetTickCount() - start) >= limit) {
            ESP_LOGW(TAG, "Timed out waiting for serial logger to pause");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

void sd_serial_log_resume_after_sd_operation()
{
    if (!s_initialized || s_task == nullptr) return;
    portENTER_CRITICAL(&s_state_mux);
    s_pause_requested = false;
    portEXIT_CRITICAL(&s_state_mux);
    notify_worker();
}
