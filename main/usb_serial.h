#pragma once
/*
 * usb_serial.h — USB Host CDC-ACM / VCP serial communication.
 *
 * This module owns:
 *   - USB Host library task (usb_lib_task)
 *   - VCP/CDC-ACM connection task (vcp_task)
 *   - USB RX ring buffer and keyboard TX path
 *   - Baud rate state
 *   - screen_log() helper (thread-safe printf to VT100 parser)
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

// ==============================================================
// USB State Accessors (called from display and main)
// ==============================================================

/** Returns true if a USB serial device is currently connected. */
bool usb_is_connected(void);

/** Returns the currently configured baud rate. */
uint32_t usb_get_baud_rate(void);

/** Set the baud rate (also applies to connected VCP device if any). */
void usb_set_baud_rate(uint32_t baud);

// ==============================================================
// USB TX
// ==============================================================

/**
 * @brief Send bytes to the connected USB serial device.
 * @param data  Bytes to send.
 * @param len   Number of bytes.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t usb_tx(const uint8_t *data, size_t len);

// ==============================================================
// Screen Log Helper
// ==============================================================

/**
 * @brief Thread-safe printf-style log to the terminal screen.
 *
 * Can be called from any task. Messages are queued and processed
 * in the main loop via vt100_process_byte().
 */
void screen_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ==============================================================
// USB RX Ring Buffer (read by main loop)
// ==============================================================

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

/** Returns the USB RX ring buffer handle (read by main loop). */
RingbufHandle_t usb_get_rx_ringbuf(void);

// ==============================================================
// Screen Log Queue (read by main loop)
// ==============================================================

#include <freertos/queue.h>
#include "m5_tab5_keyboard.h"

#define SCREEN_LOG_MSG_LEN  128
typedef struct {
    char msg[SCREEN_LOG_MSG_LEN];
} screen_log_msg_t;

/** Returns the screen log queue handle (read by main loop). */
QueueHandle_t usb_get_screen_log_queue(void);

// ==============================================================
// Key Event Queue (written by keyboard callback, read by main loop)
// ==============================================================

typedef struct {
    uint8_t modifier;
    char    str[16];
} key_event_msg_t;

/** Returns the keyboard event queue handle. */
QueueHandle_t usb_get_key_queue(void);

// ==============================================================
// USB Task Entry Points
// ==============================================================

/**
 * @brief Initialize USB state (queues, semaphores, ring buffer).
 * Must be called before usb_start_vcp_task().
 */
void usb_init(void);

/**
 * @brief Start or re-enable the VCP task (which in turn starts usb_lib_task).
 *
 * This function is idempotent. The host infrastructure is created only once;
 * after a Port A → USB switch it only re-enables USB device enumeration.
 * Must be called after usb_init().
 */
esp_err_t usb_start_vcp_task(void);

/**
 * @brief Disable USB serial enumeration and close any active CDC/VCP device.
 *
 * The USB host infrastructure remains installed so a later USB selection can
 * resume without rebuilding the host stack.
 */
void usb_stop_vcp_task(void);

/**
 * @brief Wait until USB host and CDC/VCP drivers have been installed.
 *
 * This does not wait for a physical USB serial device to be attached.
 */
bool usb_wait_ready(uint32_t timeout_ms);

/**
 * @brief Keyboard event callback (pass to s_keyboard.enableStringMode).
 */
void keyboard_event_cb(m5_tab5_key_event_t event, void *arg);
