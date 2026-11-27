/*
 * Host tests for src/fs/nvs (the NVS port and its batches).
 *
 * The flash is simulated as NOR: a program can only clear bits and an erase sets
 * a whole store sector to FF. A power cut can be armed at any program or erase;
 * the interrupted operation is left torn (a random prefix programmed, the next
 * byte partly programmed, or an erase that only raised some bits), and the run
 * jumps back to the test, which mounts again and checks that the store holds
 * exactly the state from before or after the interrupted operation.
 */

#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs/nvs/nvs.h"

#define MAX_SECTOR 8192
#define MAX_SECTORS 8
#define IDS 12
#define MAX_VAL 900

static uint8_t flash[MAX_SECTOR * MAX_SECTORS];
static uint32_t sector_size, sector_count;

static uint64_t rng_state;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

/* power cut */
static jmp_buf cut_env;
static long events, cut_at = -1;

static void maybe_cut_write(uint32_t off, const uint8_t *data, size_t len) {
    if (++events != cut_at) {
        for (size_t i = 0; i < len; i++) {
            flash[off + i] &= data[i];
        }
        return;
    }
    size_t done = len ? rnd() % len : 0;
    for (size_t i = 0; i < done; i++) {
        flash[off + i] &= data[i];
    }
    if (done < len) {
        flash[off + done] &= (uint8_t)(data[done] | (uint8_t)rnd());
    }
    longjmp(cut_env, 1);
}

static int f_read(void *ctx, uint32_t off, void *data, size_t len) {
    (void)ctx;
    assert(off + len <= sector_size * sector_count);
    memcpy(data, flash + off, len);
    return 0;
}

static int f_write(void *ctx, uint32_t off, const void *data, size_t len) {
    (void)ctx;
    assert(off + len <= sector_size * sector_count);
    maybe_cut_write(off, data, len);
    return 0;
}

static int f_erase(void *ctx, uint32_t off, size_t len) {
    (void)ctx;
    assert(off % sector_size == 0 && len == sector_size);
    if (++events == cut_at) {
        for (size_t i = 0; i < len; i++) {
            if (rnd() & 1) {
                flash[off + i] = 0xFF;
            }
        }
        longjmp(cut_env, 1);
    }
    memset(flash + off, 0xFF, len);
    return 0;
}

static const struct nvs_flash sim = { f_read, f_write, f_erase, NULL, 0xFF, 1 };

static struct nvs_fs fs;
static uint32_t cache[64];
static uint16_t cache_slots = 64;

static void mount(void) {
    memset(&fs, 0, sizeof(fs));
    fs.lookup_cache = cache;
    fs.lookup_cache_size = cache_slots;
    fs.sector_size = sector_size;
    fs.sector_count = (uint16_t)sector_count;
    fs.flash = &sim;
    int rc = nvs_mount(&fs);
    if (rc) {
        fprintf(stderr, "mount failed: %d\n", rc);
    }
    assert(rc == 0);
}

/* model */
typedef struct {
    bool present[IDS];
    uint16_t len[IDS];
    uint8_t val[IDS][MAX_VAL];
} model_t;

static void model_check(const model_t *m, const char *what) {
    static uint8_t buf[MAX_VAL + 8];
    for (int id = 0; id < IDS; id++) {
        ssize_t r = nvs_read(&fs, (uint16_t)id, buf, sizeof(buf));
        if (!m->present[id]) {
            if (r != -ENOENT) {
                fprintf(stderr, "%s: id %d should be absent, read %zd\n", what, id, r);
                abort();
            }
            continue;
        }
        if (r != m->len[id] || memcmp(buf, m->val[id], m->len[id])) {
            fprintf(stderr, "%s: id %d wrong (read %zd, want %u)\n", what, id, r, m->len[id]);
            abort();
        }
    }
}

static bool model_equal(const model_t *m) {
    static uint8_t buf[MAX_VAL + 8];
    for (int id = 0; id < IDS; id++) {
        ssize_t r = nvs_read(&fs, (uint16_t)id, buf, sizeof(buf));
        if (!m->present[id]) {
            if (r != -ENOENT) {
                return false;
            }
        }
        else if (r != m->len[id] || memcmp(buf, m->val[id], m->len[id])) {
            return false;
        }
    }
    return true;
}

/* operations: generated from a seed so a run can be replayed exactly */
#define MAX_BATCH 5
typedef struct {
    int kind; /* 0 write, 1 delete, 2 batch */
    int n;
    uint16_t id[MAX_BATCH];
    uint16_t len[MAX_BATCH];
    uint8_t fill[MAX_BATCH];
} op_t;

#define OPS 120
static op_t ops[OPS];
static model_t states[OPS + 1];
static uint8_t valbuf[MAX_BATCH][MAX_VAL];

static void make_value(uint8_t *out, uint16_t len, uint8_t fill, uint16_t id) {
    for (uint16_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(fill + i * 7 + id);
    }
}

static void gen_ops(uint64_t seed, uint16_t max_len) {
    rng_state = seed;
    for (int i = 0; i < OPS; i++) {
        op_t *o = &ops[i];
        memset(o, 0, sizeof(*o));
        uint32_t k = rnd() % 10;
        o->kind = k < 5 ? 0 : (k < 6 ? 1 : 2);
        o->n = o->kind == 2 ? 2 + (int)(rnd() % (MAX_BATCH - 1)) : 1;
        for (int j = 0; j < o->n; j++) {
            uint16_t id;
            bool dup;
            do {
                id = (uint16_t)(rnd() % IDS);
                dup = false;
                for (int q = 0; q < j; q++) {
                    dup |= o->id[q] == id;
                }
            } while (dup);
            o->id[j] = id;
            bool del = o->kind == 1 || (o->kind == 2 && rnd() % 6 == 0);
            uint16_t cap = o->kind == 2 ? (uint16_t)(max_len / 2) : max_len;
            o->len[j] = del ? 0 : (uint16_t)(1 + rnd() % cap);
            /* sometimes rewrite the same value: the store must skip it */
            o->fill[j] = (uint8_t)(rnd() % 4);
        }
    }
    memset(&states[0], 0, sizeof(states[0]));
    for (int i = 0; i < OPS; i++) {
        states[i + 1] = states[i];
        for (int j = 0; j < ops[i].n; j++) {
            uint16_t id = ops[i].id[j];
            states[i + 1].present[id] = ops[i].len[j] > 0;
            states[i + 1].len[id] = ops[i].len[j];
            make_value(states[i + 1].val[id], ops[i].len[j], ops[i].fill[j], id);
        }
    }
}

static int run_op(const op_t *o) {
    struct nvs_batch_entry e[MAX_BATCH];
    for (int j = 0; j < o->n; j++) {
        make_value(valbuf[j], o->len[j], o->fill[j], o->id[j]);
        e[j].id = o->id[j];
        e[j].data = o->len[j] ? valbuf[j] : NULL;
        e[j].len = o->len[j];
    }
    if (o->kind == 2) {
        return nvs_batch_write(&fs, e, (size_t)o->n);
    }
    ssize_t r = nvs_write(&fs, e[0].id, e[0].data, e[0].len);
    return r < 0 ? (int)r : 0;
}

static volatile int current_op;

static long run_all(void) {
    for (current_op = 0; current_op < OPS; current_op++) {
        int rc = run_op(&ops[current_op]);
        if (rc) {
            fprintf(stderr, "op %d failed: %d\n", current_op, rc);
            abort();
        }
    }
    return events;
}

static void fresh_flash(void) {
    memset(flash, 0xFF, sizeof(flash));
}

/* every program and erase of a run, cut in turn; then a second cut while
 * recovering, for a sample of them */
static void power_cut_sweep(uint64_t seed, uint32_t ssize, uint32_t scount, uint16_t max_len, int stride) {
    sector_size = ssize;
    sector_count = scount;
    gen_ops(seed, max_len);

    fresh_flash();
    events = 0;
    cut_at = -1;
    mount();
    long total = run_all();
    model_check(&states[OPS], "uncut run");
    mount();
    model_check(&states[OPS], "uncut run after remount");

    long cuts = 0, double_cuts = 0;
    for (long k = 1; k <= total; k += stride) {
        fresh_flash();
        events = 0;
        cut_at = -1;
        mount();
        long base = events;
        cut_at = base + k;
        rng_state = seed ^ (uint64_t)k * 0x9E3779B97F4A7C15ull;
        int op = -1;
        if (setjmp(cut_env) == 0) {
            run_all();
            continue; /* events differ from the uncut run once values repeat */
        }
        op = current_op;
        cuts++;

        /* sometimes cut again inside the recovery */
        if (k % 7 == 0) {
            long before = events;
            cut_at = before + 1 + (long)(rnd() % 40);
            if (setjmp(cut_env) == 0) {
                mount();
            }
            else {
                double_cuts++;
            }
        }
        cut_at = -1;
        mount();
        bool pre = model_equal(&states[op]), post = model_equal(&states[op + 1]);
        if (!pre && !post) {
            fprintf(stderr, "seed %llu sector %u cut %ld (op %d kind %d): neither before nor after\n",
                    (unsigned long long)seed, ssize, k, op, ops[op].kind);
            model_check(&states[op], "before");
        }
        /* the store keeps working: finish the script from the state it is in */
        model_t m = post ? states[op + 1] : states[op];
        for (int i = op + 1; i < OPS; i++) {
            assert(run_op(&ops[i]) == 0);
            for (int j = 0; j < ops[i].n; j++) {
                uint16_t id = ops[i].id[j];
                m.present[id] = ops[i].len[j] > 0;
                m.len[id] = ops[i].len[j];
                make_value(m.val[id], ops[i].len[j], ops[i].fill[j], id);
            }
        }
        model_check(&m, "after recovery and the rest of the script");
        mount();
        model_check(&m, "after a further remount");
    }
    printf("sector %5u x %u, values up to %3u: %ld events, %ld cuts, %ld during recovery: ok\n",
           ssize, scount, max_len, total, cuts, double_cuts);
}

static void test_api(void) {
    sector_size = 4096;
    sector_count = 4;
    fresh_flash();
    cut_at = -1;
    mount();

    uint8_t a[16] = { 1, 2, 3 }, b[16] = { 4, 5, 6 }, out[64];
    struct nvs_batch_entry dup[2] = { { 1, a, 3 }, { 1, b, 3 } };
    assert(nvs_batch_write(&fs, dup, 2) == -EINVAL);
    assert(nvs_write(&fs, NVS_ID_RESERVED, a, 3) == -EINVAL);
    struct nvs_batch_entry reserved[2] = { { 1, a, 3 }, { 0xFFFE, b, 3 } };
    assert(nvs_batch_write(&fs, reserved, 2) == -EINVAL);

    /* a batch larger than a sector is refused before anything is written */
    static uint8_t big[3000];
    struct nvs_batch_entry huge[2] = { { 2, big, sizeof(big) }, { 3, big, sizeof(big) } };
    long before = events;
    assert(nvs_batch_write(&fs, huge, 2) == -ENOSPC);
    assert(events == before);

    /* an unchanged batch writes nothing */
    struct nvs_batch_entry ok[2] = { { 4, a, 3 }, { 5, b, 3 } };
    assert(nvs_batch_write(&fs, ok, 2) == 0);
    before = events;
    assert(nvs_batch_write(&fs, ok, 2) == 0);
    assert(events == before);
    assert(nvs_read(&fs, 4, out, sizeof(out)) == 3 && !memcmp(out, a, 3));

    /* locate gives a contiguous copy of the value */
    uint32_t off;
    size_t len;
    assert(nvs_locate(&fs, 5, &off, &len) == 0 && len == 3 && !memcmp(flash + off, b, 3));
    assert(nvs_locate(&fs, 9, &off, &len) == -ENOENT);

    /* peek reads without a mount, and finds nothing in blank or random flash */
    assert(nvs_peek(&sim, sector_size, (uint16_t)sector_count, 4, out, sizeof(out)) == 3 && !memcmp(out, a, 3));
    assert(nvs_peek(&sim, sector_size, (uint16_t)sector_count, 9, out, sizeof(out)) == -ENOENT);
    fresh_flash();
    assert(nvs_peek(&sim, sector_size, (uint16_t)sector_count, 4, out, sizeof(out)) == -ENOENT);
    rng_state = 99;
    for (size_t i = 0; i < sector_size * sector_count; i++) {
        flash[i] = (uint8_t)rnd();
    }
    assert(nvs_peek(&sim, sector_size, (uint16_t)sector_count, 4, out, sizeof(out)) == -ENOENT);
    printf("api checks: ok\n");
}

static void apply(model_t *m, const op_t *o) {
    for (int j = 0; j < o->n; j++) {
        uint16_t id = o->id[j];
        m->present[id] = o->len[j] > 0;
        m->len[id] = o->len[j];
        make_value(m->val[id], o->len[j], o->fill[j], o->id[j]);
    }
}

/* A store that has never wrapped (values from sector 0 to the write sector,
 * blank after it) mounts with more sectors and keeps working, as the file
 * store relies on when it grows into the space an earlier layout used. */
static void test_grow(void) {
    sector_size = 4096;
    sector_count = 5;
    gen_ops(7, 300);
    fresh_flash();
    cut_at = -1;
    mount();
    model_t m;
    memset(&m, 0, sizeof(m));
    int i = 0;
    for (; i < OPS && (fs.ate_wra >> 16) < 2; i++) {
        assert(run_op(&ops[i]) == 0);
        apply(&m, &ops[i]);
    }
    uint32_t k = fs.ate_wra >> 16;
    assert(k == 2 && k + 3 <= sector_count);
    for (size_t b = (k + 1) * sector_size; b < MAX_SECTORS * sector_size; b++) {
        assert(flash[b] == 0xFF);
    }
    sector_count = MAX_SECTORS;
    mount();
    model_check(&m, "after growing");
    /* the rest of the script, then all of it twice more, so gc wraps the grown store */
    for (int round = 0; round < 3; round++) {
        for (; i < OPS; i++) {
            assert(run_op(&ops[i]) == 0);
            apply(&m, &ops[i]);
        }
        model_check(&m, "grown store");
        mount();
        model_check(&m, "grown store after remount");
        i = 0;
    }
    printf("growing a store from 5 to %u sectors: ok\n", MAX_SECTORS);
}

int main(void) {
    test_api();
    test_grow();
    power_cut_sweep(1, 4096, 3, 300, 1);
    power_cut_sweep(2, 4096, 4, 600, 1);
    power_cut_sweep(3, 8192, 3, 900, 1);
    power_cut_sweep(4, 4096, 2, 200, 1);
    /* one cache slot: every lookup collides and walks the log */
    cache_slots = 1;
    power_cut_sweep(5, 4096, 3, 300, 1);
    return 0;
}
