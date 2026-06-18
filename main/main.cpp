/*
 * M5Stack TAB5 Serial Terminal - Step 2: USB Host Serial Communication
 *
 * Builds on Step 1 (display + keyboard) and adds:
 *  - USB Host library initialization
 *  - CDC-ACM driver with VCP support (CH34x, CP210x, FTDI)
 *  - Standard CDC-ACM fallback for devices like Raspberry Pi USB gadget
 *  - Bidirectional communication: keyboard -> USB TX, USB RX -> screen
 *  - ENTER key fix (send CR+LF, display newline)
 *  - Baud rate configurable via Ctrl+B
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

#include "m5_tab5_component.h"
#include "m5_tab5_keyboard.h"
#include "m5tab5_pinmap.h"
#include "lvgl_port.h"
#include "lvgl_port_disp.h"
#include "lvgl_port_touch.h"
#include "lvgl.h"

// USB Host
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/vcp_ch34x.hpp"
#include "usb/vcp_cp210x.hpp"
#include "usb/vcp_ftdi.hpp"
#include "usb/vcp.hpp"
using namespace esp_usb;

static const char *TAG = "terminal";

// ==============================================================
// Terminal Screen Configuration
// ==============================================================
#define TERM_FONT_W     8
#define TERM_FONT_H     8
#define STATUS_BAR_H    10
#define TERM_COLS       (720 / TERM_FONT_W)
#define TERM_ROWS       ((1280 - STATUS_BAR_H) / TERM_FONT_H)

static char term_buffer[TERM_ROWS][TERM_COLS + 1];
static int cursor_row = 0;
static int cursor_col = 0;

static lv_obj_t *term_label   = NULL;
static lv_obj_t *status_label = NULL;

static m5::tab5::m5tab5_component s_tab5_board;
static m5::M5Tab5Keyboard         s_keyboard;

// ==============================================================
// USB Serial state
// ==============================================================
#define USB_HOST_PRIORITY   20
#define USB_CDC_PRIORITY    19
#define USB_VCP_PRIORITY    18

static CdcAcmDevice *s_vcp_dev       = nullptr;
static bool          s_usb_connected = false;
static uint32_t      s_baud_rate     = 115200;

// ==============================================================
// Message queues / event groups
// ==============================================================
typedef struct {
    uint8_t modifier;
    char    str[16];
} key_event_msg_t;
static QueueHandle_t s_key_queue = NULL;

typedef struct {
    uint8_t data[64];
    size_t  len;
} usb_rx_msg_t;
static QueueHandle_t s_usb_rx_queue = NULL;

// USB events
#define USB_DEV_DISCONNECTED_BIT  BIT0
#define USB_LIB_STOPPED_BIT       BIT1
static EventGroupHandle_t s_usb_event_group = NULL;

static TaskHandle_t s_main_task_handle = NULL;

// ==============================================================
// Screen logging helper (thread-safe)
// ==============================================================
static QueueHandle_t s_screen_log_queue = NULL;
typedef struct {
    char msg[128];
} screen_log_msg_t;

static void screen_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void screen_log(const char *fmt, ...)
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
// Terminal Buffer Management
// ==============================================================

static void term_clear(void)
{
    for (int r = 0; r < TERM_ROWS; r++) {
        memset(term_buffer[r], ' ', TERM_COLS);
        term_buffer[r][TERM_COLS] = '\0';
    }
    cursor_row = 0;
    cursor_col = 0;
}

static void term_scroll_up(void)
{
    for (int r = 0; r < TERM_ROWS - 1; r++) {
        memcpy(term_buffer[r], term_buffer[r + 1], TERM_COLS);
    }
    memset(term_buffer[TERM_ROWS - 1], ' ', TERM_COLS);
    term_buffer[TERM_ROWS - 1][TERM_COLS] = '\0';
}

static void term_newline(void)
{
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= TERM_ROWS) {
        term_scroll_up();
        cursor_row = TERM_ROWS - 1;
    }
}

static void term_put_char(char c)
{
    if (c == '\n') {
        term_newline();
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\b' || c == 0x7F) {
        if (cursor_col > 0) {
            cursor_col--;
            term_buffer[cursor_row][cursor_col] = ' ';
        }
    } else if (c == '\t') {
        int next_tab = (cursor_col + 8) & ~7;
        if (next_tab >= TERM_COLS) {
            term_newline();
        } else {
            cursor_col = next_tab;
        }
    } else if (c >= 0x20 && c < 0x7F) {
        term_buffer[cursor_row][cursor_col] = c;
        cursor_col++;
        if (cursor_col >= TERM_COLS) {
            term_newline();
        }
    }
}

static void term_put_string(const char *str)
{
    while (*str) {
        term_put_char(*str++);
    }
}

// ==============================================================
// LVGL Display Update
// ==============================================================

static void term_refresh_display(void)
{
    if (term_label == NULL) return;

    static char display_buf[TERM_ROWS * (TERM_COLS + 1) + 1];
    char *p = display_buf;

    for (int r = 0; r < TERM_ROWS; r++) {
        memcpy(p, term_buffer[r], TERM_COLS);
        p += TERM_COLS;
        *p++ = '\n';
    }
    *p = '\0';

    lvgl_port_lock(0);
    lv_label_set_text(term_label, display_buf);
    lvgl_port_unlock();
}

static void term_update_status(const char *msg)
{
    if (status_label == NULL) return;
    lvgl_port_lock(0);
    lv_label_set_text(status_label, msg);
    lvgl_port_unlock();
}

static void update_status_bar(void)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             " USB:%s  Baud:%"PRIu32"  Ctrl+C=Clear  Ctrl+B=Baud",
             s_usb_connected ? "Connected" : "Waiting...",
             s_baud_rate);
    term_update_status(buf);
}

// ==============================================================
// LVGL UI Setup
// ==============================================================

static void ui_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    term_label = lv_label_create(scr);
    lv_obj_set_style_text_font(term_label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(term_label, lv_color_make(0, 255, 0), 0);
    lv_obj_set_style_text_letter_space(term_label, 0, 0);
    lv_obj_set_style_text_line_space(term_label, 0, 0);
    lv_obj_set_pos(term_label, 0, 0);
    lv_obj_set_size(term_label, 720, 1280 - STATUS_BAR_H);
    lv_label_set_long_mode(term_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(term_label, "");

    status_label = lv_label_create(scr);
    lv_obj_set_style_text_font(status_label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(status_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_color(status_label, lv_color_make(0, 200, 0), 0);
    lv_obj_set_style_bg_opa(status_label, LV_OPA_COVER, 0);
    lv_obj_set_pos(status_label, 0, 1280 - STATUS_BAR_H);
    lv_obj_set_size(status_label, 720, STATUS_BAR_H);
    lv_label_set_text(status_label, " TAB5 Serial Terminal - Initializing...");

    lvgl_port_unlock();
}

// ==============================================================
// LCD/LVGL Initialization
// ==============================================================

static constexpr uint32_t LCD_H_RES = 720;
static constexpr uint32_t LCD_V_RES = 1280;

static lv_display_t *s_lvgl_disp        = nullptr;
static lv_indev_t   *s_lvgl_touch_indev = nullptr;

static esp_err_t app_lcd_lvgl_init(m5::tab5::m5tab5_component &board)
{
    if (s_lvgl_disp != nullptr) {
        return ESP_OK;
    }

    esp_lcd_panel_handle_t panel_handle = board.lcd_panel();
    if (panel_handle == nullptr) {
        ESP_LOGE(TAG, "LCD panel handle unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_port_cfg_t lvgl_cfg   = {};
    lvgl_cfg.task_priority     = 6;
    lvgl_cfg.task_stack        = 16384;
    lvgl_cfg.task_affinity     = 1;
    lvgl_cfg.task_max_sleep_ms = 500;
    lvgl_cfg.timer_period_ms   = 5;

    esp_err_t ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    lvgl_disp_cfg_t disp_cfg    = {};
    disp_cfg.panel_handle       = panel_handle;
    disp_cfg.hres               = LCD_H_RES;
    disp_cfg.vres               = LCD_V_RES;
    disp_cfg.buffer_size        = LCD_H_RES * LCD_V_RES;
    disp_cfg.color_format       = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.full_refresh = 0;
    disp_cfg.flags.direct_mode  = 1;
    disp_cfg.flags.buff_spiram  = 1;
    disp_cfg.flags.sw_rotate    = 1;

    lvgl_disp_dsi_cfg_t dsi_cfg = {};
    dsi_cfg.sw_rotation         = LV_DISPLAY_ROTATION_90;
    dsi_cfg.flags.avoid_tearing = 1;
    dsi_cfg.flags.use_ppa       = 1;

    s_lvgl_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (s_lvgl_disp == nullptr) {
        ESP_LOGE(TAG, "Failed to add LVGL DSI display");
        return ESP_FAIL;
    }

    esp_lcd_touch_handle_t touch_handle = board.touch_panel();
    if (touch_handle != nullptr) {
        lvgl_touch_cfg_t touch_cfg = {};
        touch_cfg.disp             = s_lvgl_disp;
        touch_cfg.handle           = touch_handle;
        s_lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
        if (s_lvgl_touch_indev != nullptr) {
            lvgl_port_set_touch_rotation(s_lvgl_touch_indev, LV_DISPLAY_ROTATION_90);
        }
    }

    return ESP_OK;
}

// ==============================================================
// Keyboard Event Handling
// ==============================================================

static void keyboard_event_cb(m5_tab5_key_event_t event, void *arg)
{
    if (event.type == M5_TAB5_KB_MODE_STRING && event.str_len > 0) {
        key_event_msg_t msg = {};
        msg.modifier = event.str_modifier;
        size_t copy_len = event.str_len < (sizeof(msg.str) - 1) ? event.str_len : (sizeof(msg.str) - 1);
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
    if (s_usb_rx_queue == NULL || data_len == 0) return true;

    size_t offset = 0;
    while (offset < data_len) {
        usb_rx_msg_t msg = {};
        msg.len = data_len - offset;
        if (msg.len > sizeof(msg.data)) msg.len = sizeof(msg.data);
        memcpy(msg.data, data + offset, msg.len);
        xQueueSend(s_usb_rx_queue, &msg, 0);
        offset += msg.len;
    }
    return true;
}

static void usb_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "VCP device disconnected");
        s_usb_connected = false;
        if (s_usb_event_group) {
            xEventGroupSetBits(s_usb_event_group, USB_DEV_DISCONNECTED_BIT);
        }
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "VCP error: %d", event->data.error);
        // On error, treat as disconnect to trigger reconnect
        s_usb_connected = false;
        if (s_usb_event_group) {
            xEventGroupSetBits(s_usb_event_group, USB_DEV_DISCONNECTED_BIT);
        }
        break;
    default:
        break;
    }
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
    };

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        if (s_usb_event_group) {
            xEventGroupSetBits(s_usb_event_group, USB_LIB_STOPPED_BIT);
        }
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "USB host installed");

    // Notify caller that host is ready
    if (notify_target) {
        xTaskNotifyGive(notify_target);
    }

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            if (ESP_OK == usb_host_device_free_all()) {
                break;
            }
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            break;
        }
    }

    ESP_LOGI(TAG, "USB lib task ending, uninstalling host");
    usb_host_uninstall();

    if (s_usb_event_group) {
        xEventGroupSetBits(s_usb_event_group, USB_LIB_STOPPED_BIT);
    }
    vTaskDelete(NULL);
}

// ==============================================================
// VCP Connection Task
// ==============================================================

static void vcp_task(void *arg)
{
    // ---- Start USB host lib ----
    screen_log("[USB] Starting USB host...\n");
    ESP_LOGI(TAG, "Starting USB host lib task");

    xEventGroupClearBits(s_usb_event_group, USB_LIB_STOPPED_BIT | USB_DEV_DISCONNECTED_BIT);

    xTaskCreate(usb_lib_task, "usb_lib", 4096,
                xTaskGetCurrentTaskHandle(),
                USB_HOST_PRIORITY, NULL);

    // Wait for USB host to be installed (timeout 5s)
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    if (notified == 0) {
        screen_log("[USB] Host init timeout! Rebooting...\n");
        ESP_LOGE(TAG, "USB host init timeout, rebooting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // ---- Install CDC-ACM driver ----
    const cdc_acm_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority   = USB_CDC_PRIORITY,
        .xCoreID                = 0,
        .new_dev_cb             = NULL,
    };
    esp_err_t err = cdc_acm_host_install(&driver_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(err));
        screen_log("[USB] CDC driver install failed!\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    // ---- Register VCP drivers (FTDI, CP210x, CH34x) ----
    VCP::register_driver<FT23x>();
    VCP::register_driver<CP210x>();
    VCP::register_driver<CH34x>();
    ESP_LOGI(TAG, "VCP drivers registered");

    // Notify main task that USB is ready
    if (s_main_task_handle) {
        xTaskNotifyGive(s_main_task_handle);
    }

    // ---- Connection loop ----
    while (1) {
        // Check if USB lib has stopped (fatal error -> reboot)
        EventBits_t bits = xEventGroupGetBits(s_usb_event_group);
        if (bits & USB_LIB_STOPPED_BIT) {
            screen_log("[USB] Host stopped. Rebooting...\n");
            ESP_LOGE(TAG, "USB lib stopped unexpectedly, rebooting");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }

        screen_log("[USB] Waiting for device...\n");
        ESP_LOGI(TAG, "Waiting for VCP device...");

        // Device config for VCP drivers (FTDI/CP210x/CH34x):
        //   in_buffer_size=0  -> use endpoint MPS (required for FTDI)
        //   out_buffer_size=512 -> TX buffer
        //   connection_timeout_ms=5000 -> 5s timeout, then try standard CDC
        cdc_acm_host_device_config_t dev_config = {};
        dev_config.connection_timeout_ms = 5000;
        dev_config.out_buffer_size       = 512;
        dev_config.in_buffer_size        = 0;   // 0 = use EP MPS (critical for FTDI)
        dev_config.user_arg              = NULL;
        dev_config.event_cb              = usb_event_cb;
        dev_config.data_cb               = usb_rx_cb;

        xEventGroupClearBits(s_usb_event_group, USB_DEV_DISCONNECTED_BIT);

        CdcAcmDevice *dev = nullptr;
        bool is_standard_cdc = false;

        // --- Try 1: VCP drivers (FTDI, CP210x, CH34x) ---
        dev = VCP::open(&dev_config);

        if (dev == nullptr) {
            // --- Try 2: Standard CDC-ACM (e.g. Raspberry Pi USB gadget) ---
            ESP_LOGI(TAG, "VCP::open timed out, trying standard CDC-ACM...");
            screen_log("[USB] Trying standard CDC-ACM...\n");

            // Use CdcAcmDevice::open() directly with open_config
            cdc_acm_host_open_config_t open_cfg = {};
            open_cfg.vid                   = CDC_HOST_ANY_VID;
            open_cfg.pid                   = CDC_HOST_ANY_PID;
            open_cfg.interface_idx         = 0;
            open_cfg.dev_addr              = CDC_HOST_ANY_DEV_ADDR;
            open_cfg.connection_timeout_ms = 5000;
            open_cfg.out_buffer_size       = 512;
            open_cfg.in_buffer_size        = 0;
            open_cfg.event_cb              = usb_event_cb;
            open_cfg.data_cb               = usb_rx_cb;
            open_cfg.user_arg              = NULL;

            CdcAcmDevice *cdc_dev = new CdcAcmDevice();
            if (cdc_dev != nullptr) {
                err = cdc_dev->open(&open_cfg);
                if (err == ESP_OK) {
                    dev = cdc_dev;
                    is_standard_cdc = true;
                    ESP_LOGI(TAG, "Standard CDC-ACM device opened");
                    screen_log("[USB] Standard CDC-ACM opened\n");
                } else {
                    ESP_LOGE(TAG, "Standard CDC-ACM open failed: %s", esp_err_to_name(err));
                    delete cdc_dev;
                }
            }
        }

        if (dev == nullptr) {
            // Both failed: check if lib stopped
            bits = xEventGroupGetBits(s_usb_event_group);
            if (bits & USB_LIB_STOPPED_BIT) {
                screen_log("[USB] Host stopped. Rebooting...\n");
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }
            // Normal timeout: retry
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // ---- Device connected ----
        // IMPORTANT: Clear DISCONNECTED_BIT here, because VCP::open() timeout
        // may have triggered usb_event_cb(CDC_ACM_HOST_ERROR) internally,
        // setting the bit BEFORE our open() succeeded. If we don't clear it,
        // xEventGroupWaitBits() below returns immediately and we disconnect.
        xEventGroupClearBits(s_usb_event_group, USB_DEV_DISCONNECTED_BIT);

        // Set line coding and control line state ONLY for VCP devices (FTDI/CP210x/CH34x).
        // Standard CDC-ACM devices (e.g. Raspberry Pi USB gadget) do NOT support
        // SET_LINE_CODING or SET_CONTROL_LINE_STATE - they respond with STALL,
        // which the cdc_acm driver then reports as CDC_ACM_HOST_ERROR asynchronously,
        // causing an immediate disconnect even after open() succeeded.
        if (!is_standard_cdc) {
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
        } else {
            ESP_LOGI(TAG, "Standard CDC-ACM: skipping line_coding_set and set_control_line_state");
        }

        s_vcp_dev       = dev;
        s_usb_connected = true;
        screen_log("[USB] Connected! Baud:%"PRIu32" %s\n",
                   s_baud_rate, is_standard_cdc ? "(CDC)" : "(VCP)");
        ESP_LOGI(TAG, "VCP connected, baud=%"PRIu32" %s",
                 s_baud_rate, is_standard_cdc ? "(CDC)" : "(VCP)");
        update_status_bar();

        // Wait for disconnect or lib-stopped
        xEventGroupWaitBits(s_usb_event_group,
                            USB_DEV_DISCONNECTED_BIT | USB_LIB_STOPPED_BIT,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        s_vcp_dev       = nullptr;
        s_usb_connected = false;
        delete dev;
        screen_log("[USB] Disconnected.\n");
        ESP_LOGI(TAG, "VCP device closed");
        update_status_bar();

        // Check if lib stopped (fatal -> reboot)
        bits = xEventGroupGetBits(s_usb_event_group);
        if (bits & USB_LIB_STOPPED_BIT) {
            screen_log("[USB] Host stopped. Rebooting...\n");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ==============================================================
// Main Application
// ==============================================================

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== TAB5 Serial Terminal ===");

    s_main_task_handle = xTaskGetCurrentTaskHandle();

    // ---- Board init ----
    m5::tab5::m5tab5_component_config_t board_cfg = {};
    esp_err_t ret = s_tab5_board.begin(board_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(ret));
        return;
    }

    // Enable USB-A 5V power
    s_tab5_board.usb5v_enable(true);
    ESP_LOGI(TAG, "USB-A 5V power enabled");

    // ---- LCD/LVGL init ----
    ret = app_lcd_lvgl_init(s_tab5_board);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD/LVGL init failed: %s", esp_err_to_name(ret));
        return;
    }
    ui_create();
    term_clear();
    term_put_string("M5Stack TAB5 Serial Terminal\n");
    term_put_string("============================\n");
    term_put_string("Initializing USB host...\n");
    term_refresh_display();

    // ---- Queues and event groups ----
    s_key_queue        = xQueueCreate(32, sizeof(key_event_msg_t));
    s_usb_rx_queue     = xQueueCreate(64, sizeof(usb_rx_msg_t));
    s_screen_log_queue = xQueueCreate(32, sizeof(screen_log_msg_t));
    s_usb_event_group  = xEventGroupCreate();

    // ---- VCP task ----
    xTaskCreate(vcp_task, "vcp_task", 8192, NULL, USB_VCP_PRIORITY, NULL);

    // Wait for USB host to be ready (first time)
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
    ESP_LOGI(TAG, "USB ready, starting main loop");
    term_put_string("Connect a USB-serial device to the USB-A port.\n\n");
    term_refresh_display();

    // ---- Keyboard init ----
    m5_tab5_kb_err_t kb_err = s_keyboard.begin(
        I2C_NUM_1,
        M5_TAB5_KB_DEFAULT_ADDR,
        M5_TAB5_KB_DEFAULT_SDA,
        M5_TAB5_KB_DEFAULT_SCL,
        M5_TAB5_KB_I2C_FREQ_400K,
        M5_TAB5_KB_DEFAULT_INT,
        M5_TAB5_KB_INT_MODE_HARDWARE
    );
    if (kb_err == M5_TAB5_KB_OK) {
        uint8_t version = 0;
        s_keyboard.getVersion(&version);
        ESP_LOGI(TAG, "Keyboard OK, FW: 0x%02X", version);
        s_keyboard.enableStringMode(keyboard_event_cb, NULL);
    } else {
        ESP_LOGW(TAG, "Keyboard not detected (err=%d)", kb_err);
        term_put_string("[WARNING] Keyboard not detected!\n");
        term_refresh_display();
    }

    update_status_bar();

    // ---- Main loop ----
    ESP_LOGI(TAG, "Entering main loop");

    while (1) {
        // Process screen log messages from other tasks
        screen_log_msg_t log_msg;
        bool need_refresh = false;
        while (xQueueReceive(s_screen_log_queue, &log_msg, 0) == pdTRUE) {
            term_put_string(log_msg.msg);
            need_refresh = true;
        }

        // Process USB RX data
        usb_rx_msg_t rx_msg;
        while (xQueueReceive(s_usb_rx_queue, &rx_msg, 0) == pdTRUE) {
            for (size_t i = 0; i < rx_msg.len; i++) {
                term_put_char((char)rx_msg.data[i]);
            }
            need_refresh = true;
        }

        if (need_refresh) {
            term_refresh_display();
            update_status_bar();
        }

        // Process keyboard input
        key_event_msg_t key_msg;
        if (xQueueReceive(s_key_queue, &key_msg, pdMS_TO_TICKS(20)) == pdTRUE) {
            bool ctrl = (key_msg.modifier & 0x01) != 0;

            if (ctrl) {
                char k = key_msg.str[0];
                if (k == 'c' || k == 'C') {
                    term_clear();
                    term_put_string("[Screen cleared]\n");
                    term_refresh_display();
                    continue;
                }
                if (k == 'l' || k == 'L') {
                    term_refresh_display();
                    continue;
                }
                if (k == 'b' || k == 'B') {
                    static const uint32_t baud_rates[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
                    static int baud_idx = 4;
                    baud_idx = (baud_idx + 1) % (sizeof(baud_rates) / sizeof(baud_rates[0]));
                    s_baud_rate = baud_rates[baud_idx];
                    if (s_usb_connected && s_vcp_dev) {
                        cdc_acm_line_coding_t lc = {
                            .dwDTERate   = s_baud_rate,
                            .bCharFormat = 0,
                            .bParityType = 0,
                            .bDataBits   = 8,
                        };
                        s_vcp_dev->line_coding_set(&lc);
                    }
                    char msg[64];
                    snprintf(msg, sizeof(msg), "\n[Baud: %"PRIu32"]\n", s_baud_rate);
                    term_put_string(msg);
                    term_refresh_display();
                    update_status_bar();
                    continue;
                }
                // Other Ctrl+key: send as control character to USB
                if (s_usb_connected && s_vcp_dev && k >= '@' && k <= '_') {
                    uint8_t ctrl_char = k - '@';
                    s_vcp_dev->tx_blocking(&ctrl_char, 1, 1000);
                }
                continue;
            }

            // Normal character processing
            for (int i = 0; key_msg.str[i] != '\0'; i++) {
                char c = key_msg.str[i];

                if (c == '\r' || c == '\n') {
                    if (s_usb_connected && s_vcp_dev) {
                        uint8_t crlf[2] = {'\r', '\n'};
                        s_vcp_dev->tx_blocking(crlf, 2, 1000);
                    }
                    term_put_char('\n');
                } else if (c == '\b' || c == 0x7F) {
                    if (s_usb_connected && s_vcp_dev) {
                        uint8_t bs = '\b';
                        s_vcp_dev->tx_blocking(&bs, 1, 1000);
                    }
                    term_put_char('\b');
                } else {
                    if (s_usb_connected && s_vcp_dev) {
                        s_vcp_dev->tx_blocking((uint8_t *)&c, 1, 1000);
                    }
                    term_put_char(c);
                }
            }
            term_refresh_display();
        }
    }
}
