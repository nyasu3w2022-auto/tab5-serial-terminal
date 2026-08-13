#pragma once
/*
 * porta_uart.h — M5Stack Tab5 Port A TTL UART transport.
 *
 * Port A (HY2.0-4P) mapping:
 *   GPIO53: Tab5 TX (yellow wire)
 *   GPIO54: Tab5 RX (white wire)
 *
 * Received bytes are forwarded into the terminal's existing shared RX ring
 * buffer, so USB and UART use identical VT100 parsing and rendering paths.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>
#include <freertos/ringbuf.h>

/** Configure UART1 on Port A and start its RX forwarding task. */
esp_err_t porta_uart_start(uint32_t baud, RingbufHandle_t shared_rx_ringbuf);

/** Stop the RX task and release the UART driver. */
void porta_uart_stop(void);

/** Change the Port A UART baud rate. */
void porta_uart_set_baud_rate(uint32_t baud);

/** Return the currently configured Port A UART baud rate. */
uint32_t porta_uart_get_baud_rate(void);

/** Send bytes through the Port A UART. */
esp_err_t porta_uart_tx(const uint8_t *data, size_t len);

/** Return true when UART1 is installed and the RX task is running. */
bool porta_uart_is_ready(void);
