/*
 * This file is part of the Pico Keys SDK distribution (https://github.com/polhenarejos/pico-keys-sdk).
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

/*
 * Benchmark probes. Compiled only with -DPICOKEYS_BENCHMARK=ON, which is
 * refused for signed (secure boot) images. A probe records how long a code
 * region took and nothing else: no data, keys or lengths of secrets.
 *
 *     bench_t t = bench_begin();
 *     ... work ...
 *     bench_end(BENCH_RSA_SIGN, key_bits, t);
 *
 * Without PICOKEYS_BENCHMARK the calls compile to nothing.
 */

#ifndef _PICOKEYS_BENCH_H_
#define _PICOKEYS_BENCH_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Probe ids. The number is part of the wire format read by tools/bench.py;
 * append new ids, do not renumber. */
typedef enum {
    BENCH_NONE = 0,
    BENCH_RSA_KEYGEN = 1,       /* param: key bits */
    BENCH_RSA_UNWRAP = 2,       /* key file decrypt (MKEK/DKEK) */
    BENCH_RSA_RECONSTRUCT = 3,  /* P,Q -> full private key (import, complete, check) */
    BENCH_RSA_SIGN = 4,
    BENCH_RSA_DECRYPT = 5,
    BENCH_RSA_VERIFY = 6,
    BENCH_EC_KEYGEN = 7,        /* param: curve bits */
    BENCH_EC_UNWRAP = 8,
    BENCH_ECDSA_SIGN = 9,
    BENCH_ECDSA_VERIFY = 10,
    BENCH_ECDH = 11,
    BENCH_AES_CBC_ENC = 12,     /* param: key bits; 1024-byte buffer */
    BENCH_AES_CBC_DEC = 13,
    BENCH_AES_GCM_ENC = 14,
    BENCH_SHA256 = 15,          /* param: input bytes */
    BENCH_SHA512 = 16,
    BENCH_RNG_FILL = 17,        /* param: output bytes, rounded up to a power of two */
    BENCH_RNG_HW64 = 18,        /* one get_rand_64() call (device only) */
    BENCH_FLASH_ERASE = 19,     /* one sector */
    BENCH_FLASH_PROGRAM = 20,   /* param: bytes */
    BENCH_FLASH_ERASE_PROGRAM = 21,
    BENCH_APDU = 22,            /* param: INS */
    BENCH_ID_COUNT
} bench_id_t;

typedef struct {
    uint64_t us;
    uint32_t cycles;
} bench_t;

#define BENCH_PLATFORM_DEVICE   0
#define BENCH_PLATFORM_EMULATOR 1

#ifdef PICOKEYS_BENCHMARK

void bench_init(void);
bench_t bench_begin(void);
void bench_end(bench_id_t id, uint32_t param, bench_t start);
void bench_reset(void);

/* Serialised table for the benchmark APDU. Returns bytes written. */
size_t bench_write_info(uint8_t *out, size_t cap);
size_t bench_write_probes(uint8_t *out, size_t cap, uint8_t page);
size_t bench_write_memory(uint8_t *out, size_t cap);

uint32_t bench_pow2_bucket(size_t n);

/* usb.c: bytes of the core1 stack used since core1 was last launched. */
size_t usb_card_stack_used(size_t *total);

#else

static inline void bench_init(void) {}
static inline bench_t bench_begin(void) { bench_t t = { 0, 0 }; return t; }
static inline void bench_end(bench_id_t id, uint32_t param, bench_t start) { (void)id; (void)param; (void)start; }
static inline void bench_reset(void) {}
static inline uint32_t bench_pow2_bucket(size_t n) { (void)n; return 0; }

#endif

#endif
