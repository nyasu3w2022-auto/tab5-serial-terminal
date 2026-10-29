#include <cassert>
#include <cstdint>
#include <cstring>

#include "main/sd_dictionary.h"
#include "main/sd_serial_log.h"

// The state test links the logger without an ESP-IDF SDMMC implementation. The
// worker task is deliberately not run by its task stub; these fakes document
// that configuration/status changes do not synchronously touch media.
bool sd_dictionary_lock(uint32_t) { return true; }
void sd_dictionary_unlock() {}
esp_err_t sd_dictionary_mount_locked() { return ESP_OK; }
void sd_dictionary_unmount_locked() {}
esp_err_t sd_dictionary_get_space_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (total_bytes != nullptr) *total_bytes = 8ULL * 1024ULL * 1024ULL;
    if (free_bytes != nullptr) *free_bytes = 4ULL * 1024ULL * 1024ULL;
    return ESP_OK;
}

int main()
{
    assert(sd_serial_log_init());
    assert(sd_serial_log_status() == SD_SERIAL_LOG_OFF);
    assert(std::strcmp(sd_serial_log_status_text(), "OFF") == 0);

    // Enabling is only a nonblocking request. The worker publishes NoSD until
    // it has independently mounted the card and opened the append file.
    sd_serial_log_configure(true);
    assert(sd_serial_log_is_enabled());
    assert(sd_serial_log_status() == SD_SERIAL_LOG_NO_SD);
    assert(std::strcmp(sd_serial_log_status_text(), "NoSD") == 0);
    assert(sd_serial_log_take_status_changed());
    assert(!sd_serial_log_take_status_changed());

    // Raw binary input reaches the API as bytes rather than text. While the
    // worker is still NoSD it is intentionally rejected without side effects.
    const uint8_t raw[] = {0x1b, 'P', 0x00, 0xff, '\\'};
    sd_serial_log_enqueue_rx(raw, sizeof(raw));

    // Disabling is a nonblocking drain request. New RX is rejected immediately,
    // but OFF is intentionally withheld until the worker has drained every
    // accepted ring item and completed fflush/fsync/close/unmount.
    sd_serial_log_configure(false);
    assert(!sd_serial_log_is_enabled());
    assert(sd_serial_log_status() == SD_SERIAL_LOG_FLUSH);
    assert(std::strcmp(sd_serial_log_status_text(), "FLUSH") == 0);
    assert(sd_serial_log_take_status_changed());
    assert(!sd_serial_log_take_status_changed());

    // Re-enabling cancels only an unfinished drain request and starts a new
    // nonblocking mount attempt. The worker owns the eventual ON transition.
    sd_serial_log_configure(true);
    assert(sd_serial_log_is_enabled());
    assert(sd_serial_log_status() == SD_SERIAL_LOG_NO_SD);
    assert(std::strcmp(sd_serial_log_status_text(), "NoSD") == 0);
    return 0;
}
