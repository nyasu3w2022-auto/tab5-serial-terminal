/*
 * serial_transport.cpp — Active USB / Port A UART transport selection.
 *
 * SPDX-License-Identifier: MIT
 */

#include "serial_transport.h"
#include "usb_serial.h"
#include "porta_uart.h"
#include "terminal.h"

#include <stdio.h>
#include <inttypes.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "serial_transport";

static serial_if_t s_active_if = SERIAL_IF_USB;
static bool        s_started   = false;

void serial_transport_init(void)
{
    s_active_if = SERIAL_IF_USB;
    s_started = false;
}

static bool valid_interface(serial_if_t iface)
{
    return iface == SERIAL_IF_USB || iface == SERIAL_IF_PORTA;
}

esp_err_t serial_transport_select(serial_if_t iface, uint32_t baud)
{
    if (!valid_interface(iface)) {
        ESP_LOGW(TAG, "Invalid serial interface %d; using USB", (int)iface);
        iface = SERIAL_IF_USB;
    }

    // Selecting the same interface only changes its baud rate.
    if (s_started && s_active_if == iface) {
        serial_transport_set_baud_rate(baud);
        return ESP_OK;
    }

    if (s_started) {
        serial_transport_stop();
    }

    esp_err_t err = ESP_OK;
    if (iface == SERIAL_IF_PORTA) {
        err = porta_uart_start(baud, usb_get_rx_ringbuf());
    } else {
        usb_set_baud_rate(baud);
        err = usb_start_vcp_task();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start %s: %s",
                 iface == SERIAL_IF_PORTA ? "PortA UART" : "USB",
                 esp_err_to_name(err));
        return err;
    }

    s_active_if = iface;
    s_started = true;
    ESP_LOGI(TAG, "Active serial interface: %s, baud=%" PRIu32,
             serial_transport_get_name(), baud);
    return ESP_OK;
}

void serial_transport_stop(void)
{
    if (!s_started) return;

    if (s_active_if == SERIAL_IF_PORTA) {
        porta_uart_stop();
    } else {
        // usb_start_vcp_task() owns the host task for the process lifetime.
        // Disabling only closes/pauses the USB serial connection loop.
        usb_stop_vcp_task();
    }
    s_started = false;
}

void serial_transport_set_baud_rate(uint32_t baud)
{
    if (s_active_if == SERIAL_IF_PORTA) {
        porta_uart_set_baud_rate(baud);
    } else {
        usb_set_baud_rate(baud);
    }
}

uint32_t serial_transport_get_baud_rate(void)
{
    return s_active_if == SERIAL_IF_PORTA
           ? porta_uart_get_baud_rate()
           : usb_get_baud_rate();
}

esp_err_t serial_transport_tx(const uint8_t *data, size_t len)
{
    if (s_active_if == SERIAL_IF_PORTA) {
        return porta_uart_tx(data, len);
    }
    return usb_tx(data, len);
}

serial_if_t serial_transport_get_interface(void)
{
    return s_active_if;
}

bool serial_transport_is_ready(void)
{
    return s_active_if == SERIAL_IF_PORTA
           ? porta_uart_is_ready()
           : usb_is_connected();
}

const char *serial_transport_get_name(void)
{
    return s_active_if == SERIAL_IF_PORTA ? "PortA" : "USB";
}

const char *serial_transport_get_status(void)
{
    if (s_active_if == SERIAL_IF_PORTA) {
        return porta_uart_is_ready() ? "Ready" : "Error";
    }
    return usb_is_connected() ? "Connected" : "Waiting...";
}

bool serial_transport_wait_ready(uint32_t timeout_ms)
{
    if (s_active_if == SERIAL_IF_PORTA) {
        return porta_uart_is_ready();
    }
    return usb_wait_ready(timeout_ms);
}

void serial_transport_send_window_size(void)
{
    char winsz_seq[32];
    int len = snprintf(winsz_seq, sizeof(winsz_seq),
                       "\033[8;%d;%dt", TERM_ROWS, TERM_COLS);
    if (len <= 0 || (size_t)len >= sizeof(winsz_seq)) return;

    esp_err_t err = serial_transport_tx((const uint8_t *)winsz_seq, (size_t)len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Window-size notification not sent on %s: %s",
                 serial_transport_get_name(), esp_err_to_name(err));
    }
}
