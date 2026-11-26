/*
 * This file is part of the Pico Keys SDK distribution (https://github.com/polhenarejos/pico-keys-sdk).
 * Copyright (c) 2022 Pol Henarejos.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef _COUNTER_LOG_H_
#define _COUNTER_LOG_H_

#include <stdbool.h>
#include <stdint.h>

/* Retry counters kept in an append-only log in the first two sectors of the
   data area, so an attempt is recorded in flash before the PIN is compared
   and no sector is erased per login. Design: pico-hsm doc/review/pin-counter.md.
   Compiled for RP2040, RP2350 and the emulator; elsewhere counter_log_active()
   is false and callers keep their old counters. */

#define COUNTER_LOG_IDS 16

#if defined(PICO_PLATFORM) || defined(ENABLE_EMULATION)
#define PICOKEYS_COUNTER_LOG 1
#endif

/* At boot, after the file system scan: decides whether the log owns the
   first two sectors and, if so, keeps the file system out of them. No
   writes. */
extern void counter_log_boot(void);

/* True when the log owns its sectors on this device. */
extern bool counter_log_active(void);

/* Loads the log, sealing torn bytes. On a device without an epoch yet,
   creates one whose failure counts are `carry` (migration from the old
   counters). Returns PICOKEYS_OK, or an error when inactive or on a flash
   failure. Safe to call repeatedly. */
extern int counter_log_open(const uint8_t carry[COUNTER_LOG_IDS]);

/* Failures since the last success or reset of counter `id`. */
extern int counter_log_failures(uint8_t id);

/* Records an attempt on `id` and returns once it is in flash. Until
   counter_log_succeed(), the attempt counts as a failure, including across
   a power cut. */
extern int counter_log_attempt(uint8_t id);

/* Marks the last attempt on `id` as successful: failures back to 0. */
extern int counter_log_succeed(uint8_t id);

/* Sets failures of `id` to 0 without an attempt (RESET RETRY COUNTER). */
extern int counter_log_reset(uint8_t id);

/* Starts a new epoch with these failure counts (INITIALIZE uses zeros). */
extern int counter_log_new_epoch(const uint8_t carry[COUNTER_LOG_IDS]);

#endif
