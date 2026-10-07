#pragma once
/*
 * bell_notification.h — Deferred audible terminal Bell notification.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>

#include "m5_tab5_component.h"

/**
 * @brief Start the one-slot Bell audio worker.
 *
 * This does not initialise I2S, ES8388, or the speaker amplifier.  Those
 * resources are opened in the worker only after the first audible Bell so an
 * otherwise silent terminal retains the board's normal low-power state.
 */
bool bell_notification_init(m5::tab5::m5tab5_component &board);

/**
 * @brief Request one audible Bell without blocking the caller.
 *
 * The worker queue holds one pending request.  Additional BELL bytes while a
 * beep is being played are intentionally coalesced rather than queued
 * indefinitely.
 */
void bell_notification_request_sound(void);
