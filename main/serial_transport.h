#pragma once
/*
 * serial_transport.h — Active serial-interface abstraction.
 *
 * This module selects exactly one terminal transport at a time:
 *   - USB Host CDC-ACM / VCP
 *   - M5Stack Tab5 Port A TTL UART (GPIO53 TX, GPIO54 RX; UART1)
 *   - M5Stack Tab5 M-Bus TTL UART (GPIO6 TX, GPIO7 RX; UART2)
 *
 * Both transports feed received bytes into the existing shared RX ring buffer
 * and expose the same TX/baud/status API to the terminal application.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

#include "settings.h"

/** Initialize transport-selection state. Call after usb_init(). */
void serial_transport_init(void);

/**
 * @brief Select and start an interface with the requested baud rate.
 *
 * If another interface is active, it is stopped before the requested
 * interface is started. USB selection starts the USB host lazily; Port A
 * selection configures UART1 for GPIO53 (TX) and GPIO54 (RX); M-Bus
 * selection configures UART2 for GPIO6 (TX) and GPIO7 (RX).
 */
esp_err_t serial_transport_select(serial_if_t iface, uint32_t baud);

/** Set the baud rate of the active transport. */
void serial_transport_set_baud_rate(uint32_t baud);

/** Return the currently configured baud rate. */
uint32_t serial_transport_get_baud_rate(void);

/** Send bytes through the active transport. */
esp_err_t serial_transport_tx(const uint8_t *data, size_t len);

/** Return the active serial-interface selection. */
serial_if_t serial_transport_get_interface(void);

/**
 * @brief Return true when the selected transport is usable.
 *
 * For USB this means a serial device is connected. For Port A and M-Bus it
 * means the selected UART driver is configured and ready; physical cable
 * presence is not detectable by a plain TTL UART.
 */
bool serial_transport_is_ready(void);

/** Human-readable interface label: "USB", "PortA", or "MBUS". */
const char *serial_transport_get_name(void);

/** Human-readable status label for the status bar. */
const char *serial_transport_get_status(void);

/**
 * @brief Wait for initial transport infrastructure readiness.
 *
 * USB waits until the host and CDC/VCP drivers are installed. Port A is ready
 * immediately after successful UART setup.
 */
bool serial_transport_wait_ready(uint32_t timeout_ms);

/**
 * @brief Send the xterm text-area size sequence for the active font geometry.
 *
 * Sends ESC[8;<rows>;<cols>t through the active transport. USB also sends
 * this when a device first connects; this API additionally updates Port A
 * peers and peers after a font-size change.
 */
void serial_transport_send_window_size(void);

/** Stop the currently active transport. */
void serial_transport_stop(void);
