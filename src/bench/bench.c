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

#ifdef PICOKEYS_BENCHMARK

#include "picokeys.h"
#include <string.h>
#include <malloc.h>
#include "bench.h"

#if defined(PICO_PLATFORM)
#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "pico/binary_info.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#if PICO_RP2350 && !defined(__riscv)
#include "hardware/structs/m33.h"
#define BENCH_HAVE_CYCCNT 1
#endif
bi_decl(bi_program_feature("BENCHMARK BUILD - not for production"))
#elif defined(ESP_PLATFORM)
#include "compat/esp_compat.h"
#include "esp_timer.h"
#else
#include <time.h>
#include "compat/queue.h"
#endif

#define BENCH_MAX_PROBES 64
#define BENCH_WIRE_VERSION 1
#define BENCH_RECORD_SIZE 33
#define BENCH_STACK_PAINT 0xA5A5A5A5u

typedef struct {
    uint8_t id;
    uint32_t param;
    uint32_t count;
    uint64_t total_us;
    uint32_t min_us;
    uint32_t max_us;
    uint64_t total_cycles;  /* 0 when cycles are not available */
} bench_probe_t;

static bench_probe_t probes[BENCH_MAX_PROBES];
static size_t probe_count = 0;
static uint32_t probe_dropped = 0;
static mutex_t bench_mutex;
static bool bench_ready = false;

static uint64_t now_us(void) {
#if defined(PICO_PLATFORM)
    return time_us_64();
#elif defined(ESP_PLATFORM)
    return (uint64_t)esp_timer_get_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

#ifdef BENCH_HAVE_CYCCNT
/* Each core has its own DWT, so enable it on whichever core asks. */
static inline uint32_t now_cycles(void) {
    if (!(m33_hw->dwt_ctrl & M33_DWT_CTRL_CYCCNTENA_BITS)) {
        m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
        m33_hw->dwt_cyccnt = 0;
        m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
    }
    return m33_hw->dwt_cyccnt;
}
#else
static inline uint32_t now_cycles(void) {
    return 0;
}
#endif

#if defined(PICO_PLATFORM)
/* Linker symbols, declared as word arrays so no pointer casts are needed. */
extern uint32_t __StackBottom[], __StackTop[];
extern char __end__, __HeapLimit;

/* Fill the unused part of the core0 stack so the high-water mark can be
 * read later. Stops well short of the live frames. */
static void paint_core0_stack(void) {
    void *frame = __builtin_frame_address(0);
    uintptr_t limit = (uintptr_t)frame - 512;
    volatile uint32_t *p = __StackBottom;
    uint32_t save = save_and_disable_interrupts();
    while ((uintptr_t)p < limit) {
        *p++ = BENCH_STACK_PAINT;
    }
    restore_interrupts(save);
}

static size_t core0_stack_used(size_t *total) {
    const volatile uint32_t *p = __StackBottom;
    const uint32_t *top = __StackTop;
    *total = (size_t)((uintptr_t)top - (uintptr_t)p);
    while (p < top && *p == BENCH_STACK_PAINT) {
        p++;
    }
    return (size_t)((uintptr_t)top - (uintptr_t)p);
}
#endif

void bench_init(void) {
    if (bench_ready) {
        return;
    }
    mutex_init(&bench_mutex);
#if defined(PICO_PLATFORM)
    paint_core0_stack();
#endif
    bench_ready = true;
}

bench_t bench_begin(void) {
    bench_t t;
    t.cycles = now_cycles();
    t.us = now_us();
    return t;
}

void bench_end(bench_id_t id, uint32_t param, bench_t start) {
    uint32_t c_end = now_cycles();
    uint64_t us = now_us() - start.us;
    uint32_t cycles = c_end - start.cycles;
#ifdef BENCH_HAVE_CYCCNT
    /* CYCCNT is 32 bits: it wraps after 2^32 cycles (about 28 s at 150 MHz).
     * Report 0 cycles rather than a wrong number. */
    if ((uint64_t)us * (clock_get_hz(clk_sys) / 1000000u) >= 0xF0000000ull) {
        cycles = 0;
    }
#endif
    if (!bench_ready) {
        return;
    }
    uint32_t us32 = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;

    mutex_enter_blocking(&bench_mutex);
    bench_probe_t *p = NULL;
    for (size_t i = 0; i < probe_count; i++) {
        if (probes[i].id == (uint8_t)id && probes[i].param == param) {
            p = &probes[i];
            break;
        }
    }
    if (!p && probe_count < BENCH_MAX_PROBES) {
        p = &probes[probe_count++];
        memset(p, 0, sizeof(*p));
        p->id = (uint8_t)id;
        p->param = param;
        p->min_us = UINT32_MAX;
    }
    if (p) {
        p->count++;
        p->total_us += us;
        p->total_cycles += cycles;
        if (us32 < p->min_us) {
            p->min_us = us32;
        }
        if (us32 > p->max_us) {
            p->max_us = us32;
        }
    }
    else {
        probe_dropped++;
    }
    mutex_exit(&bench_mutex);
}

void bench_reset(void) {
    if (!bench_ready) {
        return;
    }
    mutex_enter_blocking(&bench_mutex);
    probe_count = 0;
    probe_dropped = 0;
    mutex_exit(&bench_mutex);
}

uint32_t bench_pow2_bucket(size_t n) {
    uint32_t b = 1;
    while (b < n && b < 0x80000000u) {
        b <<= 1;
    }
    return b;
}

static uint8_t *put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
    return p + 4;
}

static uint8_t *put64(uint8_t *p, uint64_t v) {
    p = put32(p, (uint32_t)(v >> 32));
    return put32(p, (uint32_t)v);
}

/* info: version(1) platform(1) has_cycles(1) reserved(1) sys_hz(4)
 *       probe_count(1) dropped(4) records_per_page(1) */
#define BENCH_RECORDS_PER_PAGE 6

size_t bench_write_info(uint8_t *out, size_t cap) {
    if (cap < 14) {
        return 0;
    }
    uint8_t *p = out;
    *p++ = BENCH_WIRE_VERSION;
#if defined(ENABLE_EMULATION)
    *p++ = BENCH_PLATFORM_EMULATOR;
#else
    *p++ = BENCH_PLATFORM_DEVICE;
#endif
#ifdef BENCH_HAVE_CYCCNT
    *p++ = 1;
#else
    *p++ = 0;
#endif
    *p++ = 0;
#if defined(PICO_PLATFORM)
    p = put32(p, clock_get_hz(clk_sys));
#else
    p = put32(p, 0);
#endif
    mutex_enter_blocking(&bench_mutex);
    *p++ = (uint8_t)probe_count;
    p = put32(p, probe_dropped);
    mutex_exit(&bench_mutex);
    *p++ = BENCH_RECORDS_PER_PAGE;
    return (size_t)(p - out);
}

/* record: id(1) param(4) count(4) total_us(8) min_us(4) max_us(4) total_cycles(8) */
size_t bench_write_probes(uint8_t *out, size_t cap, uint8_t page) {
    uint8_t *p = out;
    mutex_enter_blocking(&bench_mutex);
    size_t first = (size_t)page * BENCH_RECORDS_PER_PAGE;
    for (size_t i = first; i < probe_count && i < first + BENCH_RECORDS_PER_PAGE; i++) {
        if ((size_t)(p - out) + BENCH_RECORD_SIZE > cap) {
            break;
        }
        const bench_probe_t *r = &probes[i];
        *p++ = r->id;
        p = put32(p, r->param);
        p = put32(p, r->count);
        p = put64(p, r->total_us);
        p = put32(p, r->min_us);
        p = put32(p, r->max_us);
        p = put64(p, r->total_cycles);
    }
    mutex_exit(&bench_mutex);
    return (size_t)(p - out);
}

/* memory: heap_total(4) heap_arena(4) heap_in_use(4)
 *         core0_stack_total(4) core0_stack_used(4)
 *         core1_stack_total(4) core1_stack_used(4)
 * Stack fields are 0 where they cannot be measured (emulator, ESP32). */
size_t bench_write_memory(uint8_t *out, size_t cap) {
    if (cap < 28) {
        return 0;
    }
    uint32_t heap_total = 0, arena = 0, in_use = 0;
    size_t s0_total = 0, s0_used = 0, s1_total = 0, s1_used = 0;
#if defined(PICO_PLATFORM)
    struct mallinfo mi = mallinfo();
    heap_total = (uint32_t)((uintptr_t)&__HeapLimit - (uintptr_t)&__end__);
    arena = (uint32_t)mi.arena;     /* sbrk high-water: newlib does not give memory back */
    in_use = (uint32_t)mi.uordblks;
    s0_used = core0_stack_used(&s0_total);
    s1_used = usb_card_stack_used(&s1_total);
#elif !defined(ESP_PLATFORM) && defined(__GLIBC__)
    struct mallinfo2 mi = mallinfo2();
    arena = (uint32_t)mi.arena;
    in_use = (uint32_t)mi.uordblks;
#endif
    uint8_t *p = out;
    p = put32(p, heap_total);
    p = put32(p, arena);
    p = put32(p, in_use);
    p = put32(p, (uint32_t)s0_total);
    p = put32(p, (uint32_t)s0_used);
    p = put32(p, (uint32_t)s1_total);
    p = put32(p, (uint32_t)s1_used);
    return (size_t)(p - out);
}

#endif
