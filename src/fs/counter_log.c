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

/* Append-only retry counters. Layout of each of the two sectors (one epoch
   each; the newer valid one is current):

     page 0:  magic(4) version(1) FF FF FF  sequence(4)  carried failures(16)
              crc32c of the 28 bytes before it (4); rest FF
     pages 1..: one byte per event, from the first byte of page 1

   An event byte only ever loses bits:
     FF  unused
     i7  attempt started on counter i (bit 3 cleared)
     i3  that attempt succeeded, or counter i was reset (bit 2 cleared too)
     00  sealed: a torn byte found at load, counted as one failure for every
         counter without a later success

   Failures of counter i = the epoch's carried value, plus one for every i7
   and every 00 after the last i3 of i; an i3 also drops the carried value. */

#include "picokeys.h"
#include "counter_log.h"

#ifdef PICOKEYS_COUNTER_LOG

#include <string.h>
#include "flash.h"
#include "crypto_utils.h"

#if defined(ENABLE_EMULATION)
#define CL_SECTOR 0x4000u   /* matches low_flash.c for the emulator */
#define CL_PAGE   0x400u
#else
#define CL_SECTOR 0x1000u
#define CL_PAGE   0x100u
#endif
#define CL_MAGIC   0x314C4350u   /* "PCL1" */
#define CL_VERSION 1u
#define CL_HEADER_LEN 32u

#ifdef COUNTER_LOG_TEST_EVENTS
#define CL_EVENTS COUNTER_LOG_TEST_EVENTS   /* emulator tests: force rollovers */
#else
#define CL_EVENTS (CL_SECTOR - CL_PAGE)
#endif

/* The file system's journal sector sits one sector below its lowest record,
   and its six redo sectors below that (TOTAL_FLASH_PAGES in low_flash.c). */
#define FS_SCRATCH_SECTORS 7u

extern uintptr_t last_base;

static bool cl_active = false;
static bool cl_loaded = false;
static uintptr_t cl_base = 0;         /* first of the two sectors */
static uint8_t cl_cur = 0;            /* current sector, 0 or 1 */
static uint32_t cl_seq = 0;
static uint32_t cl_next = 0;          /* next free event index */
static uint8_t cl_fail[COUNTER_LOG_IDS];
static int32_t cl_attempt[COUNTER_LOG_IDS];   /* event index of the open attempt */

static uintptr_t sector_addr(uint8_t s) {
    return cl_base + (uintptr_t)s * CL_SECTOR;
}

static const uint8_t *sector_ptr(uint8_t s) {
    return low_flash_raw_ptr(sector_addr(s));
}

static bool header_valid(uint8_t s, uint32_t *seq, uint8_t carry[COUNTER_LOG_IDS]) {
    const uint8_t *h = sector_ptr(s);
    uint32_t magic, sq, crc;
    memcpy(&magic, h, 4);
    memcpy(&sq, h + 8, 4);
    memcpy(&crc, h + 28, 4);
    if (magic != CL_MAGIC || h[4] != CL_VERSION) {
        return false;
    }
    if (crc32c(CONST_BYTE_ARRAY(h, 28)) != crc) {
        return false;
    }
    if (seq) {
        *seq = sq;
    }
    if (carry) {
        memcpy(carry, h + 12, COUNTER_LOG_IDS);
    }
    return true;
}

void counter_log_boot(void) {
    cl_base = flash_data_area_start();
    cl_active = false;
    cl_loaded = false;
    bool owned = header_valid(0, NULL, NULL) || header_valid(1, NULL, NULL);
    /* Without an epoch, the sectors are ours only if the file system has
       never reached them. Anything found there is then a torn epoch of our
       own, and counter_log_open() starts over. */
    uintptr_t lowest = (last_base & ~(uintptr_t)(CL_SECTOR - 1)) - FS_SCRATCH_SECTORS * CL_SECTOR;
    bool untouched = last_base >= cl_base + (FS_SCRATCH_SECTORS + 2) * CL_SECTOR && lowest >= cl_base + 2 * CL_SECTOR;
    if (owned || untouched) {
        cl_active = true;
        flash_reserve_bottom(2 * CL_SECTOR);
    }
}

bool counter_log_active(void) {
    return cl_active;
}

static int program_byte(uint8_t s, uint32_t index, uint8_t value) {
    static uint8_t page[CL_PAGE];
    uint32_t offset = CL_PAGE + index;
    memset(page, 0xFF, sizeof(page));
    page[offset % CL_PAGE] = value;
    return low_flash_raw(sector_addr(s) + (offset / CL_PAGE) * CL_PAGE, page);
}

static int write_epoch(uint8_t s, uint32_t seq, const uint8_t carry[COUNTER_LOG_IDS]) {
    static uint8_t page[CL_PAGE];
    int r = low_flash_raw(sector_addr(s), NULL);
    if (r != PICOKEYS_OK) {
        return r;
    }
    memset(page, 0xFF, sizeof(page));
    uint32_t magic = CL_MAGIC;
    memcpy(page, &magic, 4);
    page[4] = CL_VERSION;
    memcpy(page + 8, &seq, 4);
    memcpy(page + 12, carry, COUNTER_LOG_IDS);
    uint32_t crc = crc32c(CONST_BYTE_ARRAY(page, 28));
    memcpy(page + 28, &crc, 4);
    return low_flash_raw(sector_addr(s), page);
}

static bool event_valid(uint8_t b) {
    uint8_t low = b & 0x0F;
    return low == 0x7 || low == 0x3;
}

/* Replays the current epoch into cl_fail, sealing torn bytes. */
static int replay(const uint8_t carry[COUNTER_LOG_IDS]) {
    memcpy(cl_fail, carry, COUNTER_LOG_IDS);
    for (int i = 0; i < COUNTER_LOG_IDS; i++) {
        cl_attempt[i] = -1;
    }
    const uint8_t *events = sector_ptr(cl_cur) + CL_PAGE;
    uint32_t last = 0;
    bool any = false;
    for (uint32_t i = 0; i < CL_EVENTS; i++) {
        if (events[i] != 0xFF) {
            last = i;
            any = true;
        }
    }
    cl_next = any ? last + 1 : 0;
    for (uint32_t i = 0; i < cl_next; i++) {
        uint8_t b = events[i];
        if (b == 0xFF) {
            continue;   /* a torn write that reads blank: nothing to count */
        }
        if (b != 0x00 && !event_valid(b)) {
            int r = program_byte(cl_cur, i, 0x00);
            if (r != PICOKEYS_OK) {
                return r;
            }
            b = 0x00;
        }
        if (b == 0x00) {
            for (int c = 0; c < COUNTER_LOG_IDS; c++) {
                if (cl_fail[c] < 0xFF) {
                    cl_fail[c]++;
                }
            }
            continue;
        }
        uint8_t id = b >> 4;
        if ((b & 0x0F) == 0x7) {
            if (cl_fail[id] < 0xFF) {
                cl_fail[id]++;
            }
            cl_attempt[id] = (int32_t)i;
        }
        else {
            cl_fail[id] = 0;
            cl_attempt[id] = -1;
        }
    }
    /* An attempt left open by a power cut stays a failure: it can't be
       completed later. */
    for (int i = 0; i < COUNTER_LOG_IDS; i++) {
        cl_attempt[i] = -1;
    }
    return PICOKEYS_OK;
}

int counter_log_open(const uint8_t carry[COUNTER_LOG_IDS]) {
    if (!cl_active) {
        return PICOKEYS_ERR_FILE_NOT_FOUND;
    }
    if (cl_loaded) {
        return PICOKEYS_OK;
    }
    uint32_t seq0 = 0, seq1 = 0;
    uint8_t carry0[COUNTER_LOG_IDS], carry1[COUNTER_LOG_IDS];
    bool v0 = header_valid(0, &seq0, carry0), v1 = header_valid(1, &seq1, carry1);
    int r = PICOKEYS_OK;
    if (v0 && v1) {
        /* A rollover stopped between writing the new epoch and erasing the
           old one: keep the newer. */
        cl_cur = seq1 > seq0 ? 1 : 0;
        cl_seq = cl_cur ? seq1 : seq0;
        r = low_flash_raw(sector_addr(cl_cur ^ 1), NULL);
    }
    else if (v0 || v1) {
        cl_cur = v1 ? 1 : 0;
        cl_seq = v1 ? seq1 : seq0;
    }
    else {
        /* First use on this device: migrate. */
        cl_cur = 0;
        cl_seq = 1;
        r = write_epoch(0, cl_seq, carry);
        if (r == PICOKEYS_OK) {
            r = low_flash_raw(sector_addr(1), NULL);
        }
        if (r == PICOKEYS_OK) {
            memcpy(carry0, carry, COUNTER_LOG_IDS);
        }
    }
    if (r != PICOKEYS_OK) {
        return r;
    }
    r = replay(cl_cur ? carry1 : carry0);
    if (r == PICOKEYS_OK) {
        cl_loaded = true;
    }
    return r;
}

int counter_log_new_epoch(const uint8_t carry[COUNTER_LOG_IDS]) {
    if (!cl_active) {
        return PICOKEYS_ERR_FILE_NOT_FOUND;
    }
    if (!cl_loaded) {
        int r = counter_log_open(carry);
        if (r != PICOKEYS_OK) {
            return r;
        }
    }
    uint8_t other = cl_cur ^ 1;
    int r = write_epoch(other, cl_seq + 1, carry);
    if (r != PICOKEYS_OK) {
        return r;
    }
    /* The new epoch is complete before the old one goes. */
    r = low_flash_raw(sector_addr(cl_cur), NULL);
    if (r != PICOKEYS_OK) {
        return r;
    }
    cl_cur = other;
    cl_seq++;
    cl_next = 0;
    memcpy(cl_fail, carry, COUNTER_LOG_IDS);
    for (int i = 0; i < COUNTER_LOG_IDS; i++) {
        cl_attempt[i] = -1;
    }
    return PICOKEYS_OK;
}

static int ensure_room(void) {
    if (cl_next < CL_EVENTS) {
        return PICOKEYS_OK;
    }
    uint8_t carry[COUNTER_LOG_IDS];
    memcpy(carry, cl_fail, COUNTER_LOG_IDS);
    return counter_log_new_epoch(carry);
}

int counter_log_failures(uint8_t id) {
    if (!cl_loaded || id >= COUNTER_LOG_IDS) {
        return PICOKEYS_ERR_FILE_NOT_FOUND;
    }
    return cl_fail[id];
}

int counter_log_attempt(uint8_t id) {
    if (!cl_loaded || id >= COUNTER_LOG_IDS) {
        return PICOKEYS_ERR_FILE_NOT_FOUND;
    }
    int r = ensure_room();
    if (r != PICOKEYS_OK) {
        return r;
    }
    r = program_byte(cl_cur, cl_next, (uint8_t)((id << 4) | 0x7));
    if (r != PICOKEYS_OK) {
        return r;
    }
    cl_attempt[id] = (int32_t)cl_next;
    cl_next++;
    if (cl_fail[id] < 0xFF) {
        cl_fail[id]++;
    }
    return PICOKEYS_OK;
}

int counter_log_succeed(uint8_t id) {
    if (!cl_loaded || id >= COUNTER_LOG_IDS || cl_attempt[id] < 0) {
        return PICOKEYS_ERR_FILE_NOT_FOUND;
    }
    int r = program_byte(cl_cur, (uint32_t)cl_attempt[id], (uint8_t)((id << 4) | 0x3));
    if (r != PICOKEYS_OK) {
        return r;
    }
    cl_attempt[id] = -1;
    cl_fail[id] = 0;
    return PICOKEYS_OK;
}

int counter_log_reset(uint8_t id) {
    if (!cl_loaded || id >= COUNTER_LOG_IDS) {
        return PICOKEYS_ERR_FILE_NOT_FOUND;
    }
    int r = ensure_room();
    if (r != PICOKEYS_OK) {
        return r;
    }
    r = program_byte(cl_cur, cl_next, (uint8_t)((id << 4) | 0x3));
    if (r != PICOKEYS_OK) {
        return r;
    }
    cl_next++;
    cl_attempt[id] = -1;
    cl_fail[id] = 0;
    return PICOKEYS_OK;
}

#else

void counter_log_boot(void) {
}

bool counter_log_active(void) {
    return false;
}

int counter_log_open(const uint8_t carry[COUNTER_LOG_IDS]) {
    (void)carry;
    return PICOKEYS_ERR_FILE_NOT_FOUND;
}

int counter_log_failures(uint8_t id) {
    (void)id;
    return PICOKEYS_ERR_FILE_NOT_FOUND;
}

int counter_log_attempt(uint8_t id) {
    (void)id;
    return PICOKEYS_ERR_FILE_NOT_FOUND;
}

int counter_log_succeed(uint8_t id) {
    (void)id;
    return PICOKEYS_ERR_FILE_NOT_FOUND;
}

int counter_log_reset(uint8_t id) {
    (void)id;
    return PICOKEYS_ERR_FILE_NOT_FOUND;
}

int counter_log_new_epoch(const uint8_t carry[COUNTER_LOG_IDS]) {
    (void)carry;
    return PICOKEYS_ERR_FILE_NOT_FOUND;
}

#endif
