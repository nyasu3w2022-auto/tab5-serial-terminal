#pragma once
/*
 * mbus_uart.h — M5Stack Tab5 M-Bus TTL UART transport.
 *
 * M-Bus mapping:
 *   MBUS pin 16 / GPIO6: Tab5 TX (PC_TX)
 *   MBUS pin 15 / GPIO7: Tab5 RX (PC_RX)
 *
 * UART_NUM_2 is used deliberately so this transport can coexist in the
 * hardware with Port A, which uses UART_NUM_1. The application selects one
 * terminal transport at a time, but the distinct UART number avoids pin and
 * driver ownership ambiguity.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>
#include <freertos/ringbuf.h>

/** Configure UART2 on the M-Bus G6/G7 pair and start its RX task. */
esp_err_t mbus_uart_start(uint32_t baud, RingbufHandle_t shared_rx_ringbuf);

/** Stop the M-Bus RX task and release the UART2 driver. */
void mbus_uart_stop(void);

/** Change the M-Bus UART baud rate. */
void mbus_uart_set_baud_rate(uint32_t baud);

/** Return the currently configured M-Bus UART baud rate. */
uint32_t mbus_uart_get_baud_rate(void);

/** Send bytes through the M-Bus UART. */
esp_err_t mbus_uart_tx(const uint8_t *data, size_t len);

/** Return true when UART2 is installed and its RX task is running. */
bool mbus_uart_is_ready(void);
