/*
 * mbus_uart.cpp — M5Stack Tab5 M-Bus TTL UART transport.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mbus_uart.h"
#include "usb_serial.h"  // screen_log()

#include <inttypes.h>
#include <esp_log.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "mbus_uart";

// M5Stack Tab5 M-Bus: pin 16 = GPIO6 (PC_TX), pin 15 = GPIO7 (PC_RX).
// UART_NUM_2 is separate from Port A's UART_NUM_1.
static constexpr uart_port_t MBUS_UART_NUM    = UART_NUM_2;
static constexpr int         MBUS_UART_TX_PIN = 6;
static constexpr int         MBUS_UART_RX_PIN = 7;
static constexpr int         MBUS_UART_RX_BUF_SIZE = 4096;
static constexpr int         MBUS_UART_TX_BUF_SIZE = 2048;
static constexpr int         MBUS_RX_CHUNK_SIZE    = 512;

static volatile bool   s_running = false;
static bool            s_ready = false;
static uint32_t        s_baud_rate = 115200;
static TaskHandle_t    s_rx_task_handle = NULL;
static RingbufHandle_t s_shared_rx_ringbuf = NULL;

static void mbus_uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[MBUS_RX_CHUNK_SIZE];

    ESP_LOGI(TAG, "M-Bus RX task started");
    while (s_running) {
        // The bounded timeout makes shutdown deterministic without deleting a
        // task that is blocked indefinitely in the UART driver.
        int read_len = uart_read_bytes(MBUS_UART_NUM, buf, sizeof(buf),
                                       pdMS_TO_TICKS(100));
        if (read_len <= 0 || !s_running) continue;
        if (s_shared_rx_ringbuf == NULL) continue;

        BaseType_t sent = xRingbufferSend(s_shared_rx_ringbuf, buf,
                                          (size_t)read_len, 0);
        if (sent != pdTRUE) {
            ESP_LOGW(TAG, "Shared RX ring buffer full, dropped %d M-Bus bytes", read_len);
        }
    }

    s_rx_task_handle = NULL;
    ESP_LOGI(TAG, "M-Bus RX task stopped");
    vTaskDelete(NULL);
}

esp_err_t mbus_uart_start(uint32_t baud, RingbufHandle_t shared_rx_ringbuf)
{
    if (shared_rx_ringbuf == NULL) {
        ESP_LOGE(TAG, "Shared RX ring buffer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ready) {
        mbus_uart_set_baud_rate(baud);
        return ESP_OK;
    }

    uart_config_t cfg = {};
    cfg.baud_rate = (int)baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.rx_flow_ctrl_thresh = 0;

    // Follow the ESP-IDF driver initialization sequence: install, configure,
    // then route the UART signals to the selected GPIO pins.
    esp_err_t err = uart_driver_install(MBUS_UART_NUM,
                                        MBUS_UART_RX_BUF_SIZE, MBUS_UART_TX_BUF_SIZE,
                                        0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(MBUS_UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(MBUS_UART_NUM);
        return err;
    }

    err = uart_set_pin(MBUS_UART_NUM,
                       MBUS_UART_TX_PIN, MBUS_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(MBUS_UART_NUM);
        return err;
    }

    s_shared_rx_ringbuf = shared_rx_ringbuf;
    s_baud_rate = baud;
    s_running = true;

    BaseType_t task_ok = xTaskCreate(mbus_uart_rx_task, "mbus_uart_rx", 4096,
                                     NULL, 18, &s_rx_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create M-Bus RX task");
        s_running = false;
        s_shared_rx_ringbuf = NULL;
        uart_driver_delete(MBUS_UART_NUM);
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    screen_log("[MBUS] UART ready: TX=GPIO6 RX=GPIO7 Baud:%" PRIu32 "\r\n", baud);
    ESP_LOGI(TAG, "M-Bus UART ready: TX=GPIO6 RX=GPIO7 baud=%" PRIu32, baud);
    return ESP_OK;
}

void mbus_uart_stop(void)
{
    if (!s_ready && !s_running) return;

    s_running = false;
    // uart_read_bytes() uses a 100ms timeout. Wait for orderly task exit.
    for (int i = 0; i < 15 && s_rx_task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_rx_task_handle != NULL) {
        ESP_LOGW(TAG, "M-Bus RX task did not stop in time; deleting it");
        vTaskDelete(s_rx_task_handle);
        s_rx_task_handle = NULL;
    }

    if (uart_is_driver_installed(MBUS_UART_NUM)) {
        uart_flush_input(MBUS_UART_NUM);
        esp_err_t err = uart_driver_delete(MBUS_UART_NUM);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "uart_driver_delete: %s", esp_err_to_name(err));
        }
    }

    s_shared_rx_ringbuf = NULL;
    s_ready = false;
    screen_log("[MBUS] UART stopped.\r\n");
    ESP_LOGI(TAG, "M-Bus UART stopped");
}

void mbus_uart_set_baud_rate(uint32_t baud)
{
    s_baud_rate = baud;
    if (!s_ready) return;

    esp_err_t err = uart_set_baudrate(MBUS_UART_NUM, baud);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_set_baudrate(%" PRIu32 "): %s",
                 baud, esp_err_to_name(err));
    }
}

uint32_t mbus_uart_get_baud_rate(void)
{
    return s_baud_rate;
}

esp_err_t mbus_uart_tx(const uint8_t *data, size_t len)
{
    if (!s_ready || data == NULL) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_OK;

    int written = uart_write_bytes(MBUS_UART_NUM, (const char *)data, len);
    if (written < 0 || (size_t)written != len) {
        ESP_LOGW(TAG, "UART TX wrote %d of %zu bytes", written, len);
        return ESP_FAIL;
    }

    return uart_wait_tx_done(MBUS_UART_NUM, pdMS_TO_TICKS(1000));
}

bool mbus_uart_is_ready(void)
{
    return s_ready;
}
