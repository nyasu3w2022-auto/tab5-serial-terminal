/*
 * usb_serial.cpp — USB Host CDC-ACM / VCP serial communication.
 *
 * SPDX-License-Identifier: MIT
 */

#include "usb_serial.h"
#include "terminal.h"   // for TERM_ROWS, TERM_COLS (window size notification)

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>
#include <freertos/ringbuf.h>

// USB Host
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/vcp_ch34x.hpp"
#include "usb/vcp_cp210x.hpp"
#include "usb/vcp_ftdi.hpp"
#include "usb/vcp.hpp"
using namespace esp_usb;

// Keyboard
#include "m5_tab5_keyboard.h"

static const char *TAG = "usb_serial";

// ==============================================================
// Task Priorities
// ==============================================================
#define USB_HOST_PRIORITY   20
#define USB_CDC_PRIORITY    19
#define USB_VCP_PRIORITY    18

// ==============================================================
// USB State
// ==============================================================
static CdcAcmDevice *s_vcp_dev       = nullptr;
static bool          s_usb_connected = false;
static uint32_t      s_baud_rate     = 115200;

// ==============================================================
// Queues / Semaphores / Event Groups
// ==============================================================

// USB RX ring buffer: 16 KB to absorb bursts without dropping bytes
#define USB_RX_RINGBUF_SIZE  16384
static RingbufHandle_t   s_usb_rx_ringbuf   = NULL;
static QueueHandle_t     s_screen_log_queue = NULL;
static QueueHandle_t     s_key_queue        = NULL;
static SemaphoreHandle_t s_dev_present_sem  = NULL;

// VID/PID of the most recently detected USB device (set by enum_filter_cb)
static volatile uint16_t s_dev_vid = 0;
static volatile uint16_t s_dev_pid = 0;

// Event group bits
#define USB_DEV_DISCONNECTED_BIT  BIT0
static EventGroupHandle_t s_usb_event_group = NULL;

// Handle of the vcp_task, used by usb_lib_task to notify readiness
static TaskHandle_t s_vcp_task_handle = NULL;

// ==============================================================
// Public Accessors
// ==============================================================

bool usb_is_connected(void)    { return s_usb_connected; }
uint32_t usb_get_baud_rate(void) { return s_baud_rate; }

void usb_set_baud_rate(uint32_t baud)
{
    s_baud_rate = baud;
    if (s_usb_connected && s_vcp_dev) {
        cdc_acm_line_coding_t lc = {
            .dwDTERate   = baud,
            .bCharFormat = 0,
            .bParityType = 0,
            .bDataBits   = 8,
        };
        s_vcp_dev->line_coding_set(&lc);
    }
}

esp_err_t usb_tx(const uint8_t *data, size_t len)
{
    if (!s_usb_connected || s_vcp_dev == nullptr) return ESP_ERR_INVALID_STATE;
    return s_vcp_dev->tx_blocking((uint8_t *)data, len, 1000);
}

RingbufHandle_t usb_get_rx_ringbuf(void)        { return s_usb_rx_ringbuf; }
QueueHandle_t   usb_get_screen_log_queue(void)  { return s_screen_log_queue; }
QueueHandle_t   usb_get_key_queue(void)         { return s_key_queue; }

// ==============================================================
// Screen Log Helper
// ==============================================================

void screen_log(const char *fmt, ...)
{
    if (s_screen_log_queue == NULL) return;
    screen_log_msg_t m = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(m.msg, sizeof(m.msg), fmt, args);
    va_end(args);
    xQueueSend(s_screen_log_queue, &m, 0);
}

// ==============================================================
// Keyboard Event Callback
// ==============================================================

void keyboard_event_cb(m5_tab5_key_event_t event, void *arg)
{
    if (event.type == M5_TAB5_KB_MODE_STRING && event.str_len > 0) {
        key_event_msg_t msg = {};
        msg.modifier = event.str_modifier;
        size_t copy_len = event.str_len < (sizeof(msg.str) - 1)
                          ? event.str_len : (sizeof(msg.str) - 1);
        memcpy(msg.str, event.str_data, copy_len);
        msg.str[copy_len] = '\0';
        if (s_key_queue != NULL) {
            xQueueSend(s_key_queue, &msg, 0);
        }
    }
}

// ==============================================================
// USB VCP Callbacks
// ==============================================================

static bool usb_rx_cb(const uint8_t *data, size_t data_len, void *arg)
{
    if (s_usb_rx_ringbuf == NULL || data_len == 0) return true;
    // Timeout=0: never block in the USB host callback.
    BaseType_t sent = xRingbufferSend(s_usb_rx_ringbuf, data, data_len, 0);
    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "USB RX ringbuf full, dropped %zu bytes", data_len);
    }
    return true;
}

static void usb_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "CDC device disconnected");
        s_usb_connected = false;
        if (s_usb_event_group) {
            xEventGroupSetBits(s_usb_event_group, USB_DEV_DISCONNECTED_BIT);
        }
        break;
    case CDC_ACM_HOST_ERROR:
        // Ignore: fired spuriously from residual async transfer callbacks.
        ESP_LOGD(TAG, "CDC error %d (ignored)", event->data.error);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD(TAG, "CDC serial state: 0x%02X", event->data.serial_state.val);
        break;
    default:
        break;
    }
}

// ==============================================================
// new_dev_cb: called from USB Host context when a device appears
// NOTE: Cannot open CDC device here; must signal vcp_task instead.
// ==============================================================

static void usb_new_dev_cb(usb_device_handle_t usb_dev)
{
    if (s_dev_present_sem) {
        xSemaphoreGive(s_dev_present_sem);
    }
}

// ==============================================================
// Enumeration Filter Callback
// ==============================================================

#define RPI_G_SERIAL_VID  0x0525u
#define RPI_G_SERIAL_PID  0xa4a7u

static bool usb_enum_filter_cb(const usb_device_desc_t *dev_desc,
                               uint8_t *bConfigurationValue)
{
    uint16_t vid = dev_desc->idVendor;
    uint16_t pid = dev_desc->idProduct;

    s_dev_vid = vid;
    s_dev_pid = pid;

    if (vid == RPI_G_SERIAL_VID && pid == RPI_G_SERIAL_PID) {
        *bConfigurationValue = 2;
        ESP_LOGI(TAG, "enum_filter: Raspberry Pi g_serial detected (VID=%04x PID=%04x), selecting config #2",
                 vid, pid);
    } else {
        ESP_LOGD(TAG, "enum_filter: VID=%04x PID=%04x bNumConfigs=%d, using default config #%d",
                 vid, pid, dev_desc->bNumConfigurations, *bConfigurationValue);
    }
    return true;
}

// ==============================================================
// USB Host Library Task (daemon)
// ==============================================================

static void usb_lib_task(void *arg)
{
    TaskHandle_t notify_target = (TaskHandle_t)arg;
    ESP_LOGI(TAG, "USB lib task started");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LOWMED,
        .enum_filter_cb = usb_enum_filter_cb,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "USB host installed");

    if (notify_target) {
        xTaskNotifyGive(notify_target);
    }

    // Run forever: never call usb_host_uninstall().
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "USB lib: no clients, freeing devices");
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB lib: all devices free, waiting for next connection");
        }
    }
}

// ==============================================================
// VCP Connection Task
// ==============================================================

// Known VCP VIDs: FTDI=0x0403, CP210x=0x10C4, CH34x=0x1A86
static const uint16_t VCP_VIDS[] = {0x0403u, 0x10C4u, 0x1A86u};
static const int      VCP_VIDS_COUNT = (int)(sizeof(VCP_VIDS) / sizeof(VCP_VIDS[0]));

static void vcp_task(void *arg)
{
    screen_log("[USB] Starting USB host...\r\n");
    ESP_LOGI(TAG, "Starting USB host lib task");

    xEventGroupClearBits(s_usb_event_group, USB_DEV_DISCONNECTED_BIT);

    xTaskCreate(usb_lib_task, "usb_lib", 4096,
                xTaskGetCurrentTaskHandle(),
                USB_HOST_PRIORITY, NULL);

    // Wait for USB host to be installed (timeout 5s)
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    if (notified == 0) {
        screen_log("[USB] Host init timeout! Rebooting...\r\n");
        ESP_LOGE(TAG, "USB host init timeout, rebooting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // ---- Install CDC-ACM driver ----
    const cdc_acm_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority   = USB_CDC_PRIORITY,
        .xCoreID                = 0,
        .new_dev_cb             = usb_new_dev_cb,
    };
    esp_err_t err = cdc_acm_host_install(&driver_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(err));
        screen_log("[USB] CDC driver install failed!\r\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // ---- Register VCP drivers (FTDI, CP210x, CH34x) ----
    VCP::register_driver<FT23x>();
    VCP::register_driver<CP210x>();
    VCP::register_driver<CH34x>();
    ESP_LOGI(TAG, "VCP drivers registered");

    // Notify main task that USB is ready
    if (s_vcp_task_handle) {
        xTaskNotifyGive(s_vcp_task_handle);
    }

    // ---- Connection loop ----
    while (1) {
        screen_log("[USB] Waiting for device...\r\n");
        ESP_LOGI(TAG, "Waiting for USB device...");

        BaseType_t sem_taken = xSemaphoreTake(s_dev_present_sem, pdMS_TO_TICKS(3000));
        if (sem_taken != pdTRUE) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(200));

        // Drain extra semaphore counts
        while (xSemaphoreTake(s_dev_present_sem, 0) == pdTRUE) {}

        xEventGroupClearBits(s_usb_event_group, USB_DEV_DISCONNECTED_BIT);

        uint16_t dev_vid = s_dev_vid;
        uint16_t dev_pid = s_dev_pid;
        ESP_LOGI(TAG, "Opening device VID=%04x PID=%04x", dev_vid, dev_pid);
        (void)dev_pid;

        bool is_known_vcp_vid = false;
        for (int i = 0; i < VCP_VIDS_COUNT; i++) {
            if (dev_vid == VCP_VIDS[i]) { is_known_vcp_vid = true; break; }
        }

        CdcAcmDevice *dev = nullptr;
        bool is_vcp = false;

        if (is_known_vcp_vid) {
            cdc_acm_host_device_config_t vcp_cfg = {};
            vcp_cfg.connection_timeout_ms = 100;
            vcp_cfg.out_buffer_size       = 512;
            vcp_cfg.in_buffer_size        = 512;
            vcp_cfg.event_cb              = usb_event_cb;
            vcp_cfg.data_cb               = usb_rx_cb;
            vcp_cfg.user_arg              = NULL;

            CdcAcmDevice *vcp_dev = VCP::open(&vcp_cfg);
            if (vcp_dev != nullptr) {
                dev    = vcp_dev;
                is_vcp = true;
                ESP_LOGI(TAG, "VCP device opened (FTDI/CP210x/CH34x)");
            }
        } else {
            ESP_LOGI(TAG, "VID=%04x not a known VCP vendor, trying standard CDC-ACM", dev_vid);
        }

        if (dev == nullptr) {
            cdc_acm_host_open_config_t cdc_cfg = {};
            cdc_cfg.vid                   = CDC_HOST_ANY_VID;
            cdc_cfg.pid                   = CDC_HOST_ANY_PID;
            cdc_cfg.interface_idx         = 0;
            cdc_cfg.dev_addr              = CDC_HOST_ANY_DEV_ADDR;
            cdc_cfg.connection_timeout_ms = 1000;
            cdc_cfg.out_buffer_size       = 512;
            cdc_cfg.in_buffer_size        = 512;
            cdc_cfg.event_cb              = usb_event_cb;
            cdc_cfg.data_cb               = usb_rx_cb;
            cdc_cfg.user_arg              = NULL;

            CdcAcmDevice *cdc_dev = new CdcAcmDevice();
            err = cdc_dev->open(&cdc_cfg);
            if (err == ESP_OK) {
                dev = cdc_dev;
                ESP_LOGI(TAG, "Standard CDC-ACM device opened");
            } else {
                ESP_LOGE(TAG, "CDC-ACM open failed: %s", esp_err_to_name(err));
                delete cdc_dev;
            }
        }

        if (dev == nullptr) {
            screen_log("[USB] Open failed, retrying...\r\n");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (is_vcp) {
            cdc_acm_line_coding_t line_coding = {
                .dwDTERate   = s_baud_rate,
                .bCharFormat = 0,
                .bParityType = 0,
                .bDataBits   = 8,
            };
            err = dev->line_coding_set(&line_coding);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "line_coding_set: %s (ignored)", esp_err_to_name(err));
            }
            err = dev->set_control_line_state(true, false);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "set_control_line_state: %s (ignored)", esp_err_to_name(err));
            }
        }

        s_vcp_dev       = dev;
        s_usb_connected = true;
        screen_log("[USB] Connected! Baud:%"PRIu32" %s\r\n",
                   s_baud_rate, is_vcp ? "(VCP)" : "(CDC)");
        ESP_LOGI(TAG, "USB connected, baud=%"PRIu32" %s",
                 s_baud_rate, is_vcp ? "(VCP)" : "(CDC)");

        // Notify the remote device of our terminal window size via xterm sequence:
        // ESC[8;<rows>;<cols>t  (set text area size in chars)
        {
            char winsz_seq[32];
            int winsz_len = snprintf(winsz_seq, sizeof(winsz_seq),
                                     "\033[8;%d;%dt", TERM_ROWS, TERM_COLS);
            if (winsz_len > 0) {
                dev->tx_blocking((uint8_t *)winsz_seq, (size_t)winsz_len, 1000);
            }
        }

        xEventGroupWaitBits(s_usb_event_group,
                            USB_DEV_DISCONNECTED_BIT,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        s_vcp_dev       = nullptr;
        s_usb_connected = false;
        delete dev;
        screen_log("[USB] Disconnected.\r\n");
        ESP_LOGI(TAG, "USB device closed");

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ==============================================================
// Public Initialization
// ==============================================================

void usb_init(void)
{
    s_key_queue        = xQueueCreate(32, sizeof(key_event_msg_t));
    s_usb_rx_ringbuf   = xRingbufferCreate(USB_RX_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (s_usb_rx_ringbuf == NULL) {
        ESP_LOGE(TAG, "Failed to create USB RX ring buffer (OOM?), rebooting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    s_screen_log_queue = xQueueCreate(32, sizeof(screen_log_msg_t));
    s_usb_event_group  = xEventGroupCreate();
    s_dev_present_sem  = xSemaphoreCreateCounting(8, 0);
}

void usb_start_vcp_task(void)
{
    // vcp_task will notify s_vcp_task_handle when USB is ready
    s_vcp_task_handle = xTaskGetCurrentTaskHandle();
    xTaskCreate(vcp_task, "vcp_task", 8192, NULL, USB_VCP_PRIORITY, NULL);
}
