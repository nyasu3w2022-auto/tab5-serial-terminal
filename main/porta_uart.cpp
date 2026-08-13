/*
 * porta_uart.cpp — M5Stack Tab5 Port A TTL UART transport.
 *
 * SPDX-License-Identifier: MIT
 */

#include "porta_uart.h"
#include "usb_serial.h"  // screen_log()

#include <inttypes.h>
#include <esp_log.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "porta_uart";

// M5Stack Tab5 HY2.0-4P Port A:
// yellow = GPIO53 (Tab5 TX), white = GPIO54 (Tab5 RX)
static constexpr uart_port_t PORTA_UART_NUM    = UART_NUM_1;
static constexpr int         PORTA_UART_TX_PIN = 53;
static constexpr int         PORTA_UART_RX_PIN = 54;
static constexpr int         PORTA_UART_RX_BUF_SIZE = 4096;
static constexpr int         PORTA_UART_TX_BUF_SIZE = 2048;
static constexpr int         PORTA_RX_CHUNK_SIZE    = 512;

static volatile bool     s_running = false;
static bool              s_ready = false;
static uint32_t          s_baud_rate = 115200;
static TaskHandle_t      s_rx_task_handle = NULL;
static RingbufHandle_t   s_shared_rx_ringbuf = NULL;

static void porta_uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[PORTA_RX_CHUNK_SIZE];

    ESP_LOGI(TAG, "Port A RX task started");
    while (s_running) {
        // A bounded timeout lets porta_uart_stop() complete without forcing
        // asynchronous task deletion while uart_read_bytes() is active.
        int read_len = uart_read_bytes(PORTA_UART_NUM, buf, sizeof(buf),
                                       pdMS_TO_TICKS(100));
        if (read_len <= 0 || !s_running) continue;

        if (s_shared_rx_ringbuf == NULL) continue;
        BaseType_t sent = xRingbufferSend(s_shared_rx_ringbuf, buf,
                                          (size_t)read_len, 0);
        if (sent != pdTRUE) {
            ESP_LOGW(TAG, "Shared RX ring buffer full, dropped %d UART bytes", read_len);
        }
    }

    s_rx_task_handle = NULL;
    ESP_LOGI(TAG, "Port A RX task stopped");
    vTaskDelete(NULL);
}

esp_err_t porta_uart_start(uint32_t baud, RingbufHandle_t shared_rx_ringbuf)
{
    if (shared_rx_ringbuf == NULL) {
        ESP_LOGE(TAG, "Shared RX ring buffer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ready) {
        porta_uart_set_baud_rate(baud);
        return ESP_OK;
    }

    // Use field assignments after zero-initialization so the code remains
    // compatible with ESP-IDF UART configuration structure revisions.
    uart_config_t cfg = {};
    cfg.baud_rate = (int)baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.rx_flow_ctrl_thresh = 0;

    esp_err_t err = uart_param_config(PORTA_UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(PORTA_UART_NUM,
                       PORTA_UART_TX_PIN, PORTA_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(PORTA_UART_NUM,
                              PORTA_UART_RX_BUF_SIZE, PORTA_UART_TX_BUF_SIZE,
                              0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_shared_rx_ringbuf = shared_rx_ringbuf;
    s_baud_rate = baud;
    s_running = true;

    BaseType_t task_ok = xTaskCreate(porta_uart_rx_task, "porta_uart_rx", 4096,
                                     NULL, 18, &s_rx_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Port A RX task");
        s_running = false;
        s_shared_rx_ringbuf = NULL;
        uart_driver_delete(PORTA_UART_NUM);
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    screen_log("[PortA] UART ready: TX=GPIO53 RX=GPIO54 Baud:%" PRIu32 "\r\n", baud);
    ESP_LOGI(TAG, "Port A UART ready: TX=GPIO53 RX=GPIO54 baud=%" PRIu32, baud);
    return ESP_OK;
}

void porta_uart_stop(void)
{
    if (!s_ready && !s_running) return;

    s_running = false;
    // uart_read_bytes() has a 100ms timeout; allow its task to exit cleanly.
    for (int i = 0; i < 15 && s_rx_task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_rx_task_handle != NULL) {
        // This should not normally be needed. It prevents a stale RX task
        // from accessing a driver that is about to be deleted.
        ESP_LOGW(TAG, "Port A RX task did not stop in time; deleting it");
        vTaskDelete(s_rx_task_handle);
        s_rx_task_handle = NULL;
    }

    if (uart_is_driver_installed(PORTA_UART_NUM)) {
        uart_flush_input(PORTA_UART_NUM);
        esp_err_t err = uart_driver_delete(PORTA_UART_NUM);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "uart_driver_delete: %s", esp_err_to_name(err));
        }
    }

    s_shared_rx_ringbuf = NULL;
    s_ready = false;
    screen_log("[PortA] UART stopped.\r\n");
    ESP_LOGI(TAG, "Port A UART stopped");
}

void porta_uart_set_baud_rate(uint32_t baud)
{
    s_baud_rate = baud;
    if (!s_ready) return;

    esp_err_t err = uart_set_baudrate(PORTA_UART_NUM, baud);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_set_baudrate(%" PRIu32 "): %s",
                 baud, esp_err_to_name(err));
    }
}

uint32_t porta_uart_get_baud_rate(void)
{
    return s_baud_rate;
}

esp_err_t porta_uart_tx(const uint8_t *data, size_t len)
{
    if (!s_ready || data == NULL) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_OK;

    int written = uart_write_bytes(PORTA_UART_NUM, (const char *)data, len);
    if (written < 0 || (size_t)written != len) {
        ESP_LOGW(TAG, "UART TX wrote %d of %zu bytes", written, len);
        return ESP_FAIL;
    }

    return uart_wait_tx_done(PORTA_UART_NUM, pdMS_TO_TICKS(1000));
}

bool porta_uart_is_ready(void)
{
    return s_ready;
}
