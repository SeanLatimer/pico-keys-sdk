/*  NVS: non volatile storage in flash
 *
 * Copyright (c) 2018 Laczen
 * Copyright (c) 2026 Lingao Meng
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ported from Zephyr (subsys/kvss/nvs, commit ef2773dffed6ee0f2add16edc6ad6342c1901320);
 * see nvs.h for what changed. Data CRC-32 and the lookup cache are always on.
 */

#include <errno.h>
#include <string.h>

#include "nvs.h"

#define ADDR_SECT_MASK 0xFFFF0000
#define ADDR_SECT_SHIFT 16
#define ADDR_OFFS_MASK 0x0000FFFF

#define NVS_MAX_SECTOR_SIZE (64 * 1024)
#define NVS_BLOCK_SIZE 32
#define NVS_LOOKUP_CACHE_NO_ADDR 0xFFFFFFFF
#define NVS_DATA_CRC_SIZE 4

/* Batch markers (len 0, so garbage collection never copies them). */
#define NVS_ID_BATCH_BEGIN 0xFFFE
#define NVS_ID_BATCH_COMMIT 0xFFFD

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct nvs_block_move_ctx {
    uint8_t buffer[NVS_BLOCK_SIZE];
    size_t buffer_pos;
};

struct nvs_flash_buf {
    const uint8_t *ptr;
    size_t len;
};

struct nvs_flash_wrt_stream {
    struct nvs_flash_buf head;
    struct nvs_flash_buf data;
    struct nvs_flash_buf tail;
};

struct nvs_gc_write_entry {
    uint16_t id;
    const void *data;
    uint16_t len;
    bool is_written;
};

/* Allocation Table Entry */
struct nvs_ate {
    uint16_t id;
    uint16_t offset;
    uint16_t len;
    uint8_t part;
    uint8_t crc8;
} __attribute__((packed));

_Static_assert(sizeof(struct nvs_ate) == 8, "ATE must be 8 bytes");
_Static_assert(offsetof(struct nvs_ate, crc8) == sizeof(struct nvs_ate) - 1, "crc8 must be last");

/* CRC-8/CCITT (poly 0x07, no reflection), as Zephyr's crc8_ccitt. */
static uint8_t crc8_ccitt(uint8_t val, const void *buf, size_t cnt) {
    const uint8_t *p = buf;
    for (size_t i = 0; i < cnt; i++) {
        val ^= p[i];
        for (int b = 0; b < 8; b++) {
            val = (val & 0x80) ? (uint8_t)((val << 1) ^ 0x07) : (uint8_t)(val << 1);
        }
    }
    return val;
}

/* CRC-32/IEEE (reflected, poly 0xEDB88320), as Zephyr's crc32_ieee. */
static uint32_t crc32_table[256];
static bool crc32_table_ready;

static uint32_t crc32_ieee(const void *buf, size_t len) {
    if (!crc32_table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            }
            crc32_table[i] = c;
        }
        crc32_table_ready = true;
    }
    const uint8_t *p = buf;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc = crc32_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static int nvs_prev_ate(struct nvs_fs *fs, uint32_t *addr, struct nvs_ate *ate);
static int nvs_ate_valid(struct nvs_fs *fs, uint32_t entry_addr, const struct nvs_ate *entry);
static int nvs_gc(struct nvs_fs *fs, struct nvs_gc_write_entry *entry);

static inline size_t nvs_lookup_cache_pos(const struct nvs_fs *fs, uint16_t id) {
    uint16_t hash = id;
    /* 16-bit integer hash function found by https://github.com/skeeto/hash-prospector. */
    hash ^= hash >> 8;
    hash *= 0x88b5U;
    hash ^= hash >> 7;
    hash *= 0xdb2dU;
    hash ^= hash >> 9;
    return hash % fs->lookup_cache_size;
}

static int nvs_lookup_cache_rebuild(struct nvs_fs *fs) {
    int rc;
    uint32_t addr, ate_addr;
    uint32_t *cache_entry;
    struct nvs_ate ate;

    memset(fs->lookup_cache, 0xff, fs->lookup_cache_size * sizeof(fs->lookup_cache[0]));
    addr = fs->ate_wra;

    while (true) {
        ate_addr = addr;
        rc = nvs_prev_ate(fs, &addr, &ate);
        if (rc) {
            return rc;
        }
        cache_entry = &fs->lookup_cache[nvs_lookup_cache_pos(fs, ate.id)];
        if (ate.id != 0xFFFF && *cache_entry == NVS_LOOKUP_CACHE_NO_ADDR && nvs_ate_valid(fs, ate_addr, &ate)) {
            *cache_entry = ate_addr;
        }
        if (addr == fs->ate_wra) {
            break;
        }
    }
    return 0;
}

static void nvs_lookup_cache_invalidate(struct nvs_fs *fs, uint32_t sector) {
    for (size_t i = 0; i < fs->lookup_cache_size; i++) {
        if ((fs->lookup_cache[i] >> ADDR_SECT_SHIFT) == sector) {
            fs->lookup_cache[i] = NVS_LOOKUP_CACHE_NO_ADDR;
        }
    }
}

static inline size_t nvs_al_size(struct nvs_fs *fs, size_t len) {
    size_t write_block_size = fs->flash->write_block_size;
    if (write_block_size <= 1U) {
        return len;
    }
    return (len + (write_block_size - 1U)) & ~(write_block_size - 1U);
}

static inline uint32_t nvs_flash_offset(struct nvs_fs *fs, uint32_t addr) {
    return fs->sector_size * (addr >> ADDR_SECT_SHIFT) + (addr & ADDR_OFFS_MASK);
}

/* flash routines */

static int nvs_flash_al_wrt_streams(struct nvs_fs *fs, uint32_t addr, const struct nvs_flash_wrt_stream *strm) {
    size_t wbs = fs->flash->write_block_size ? fs->flash->write_block_size : 1;
    uint8_t buf[NVS_BLOCK_SIZE];
    size_t stream_idx = 0U, full_bytes, buf_fill = 0U, copy;
    uint32_t offset;
    int rc;

    if ((strm->head.len + strm->data.len + strm->tail.len) == 0U) {
        return 0;
    }
    offset = nvs_flash_offset(fs, addr);

    struct nvs_flash_buf streams[] = { strm->head, strm->data, strm->tail };

    while (stream_idx < ARRAY_SIZE(streams)) {
        if (streams[stream_idx].len == 0U) {
            stream_idx++;
            continue;
        }
        if (buf_fill == 0) {
            full_bytes = streams[stream_idx].len & ~(wbs - 1);
            if (full_bytes > 0U) {
                rc = fs->flash->write(fs->flash->ctx, offset, streams[stream_idx].ptr, full_bytes);
                if (rc) {
                    return rc;
                }
                streams[stream_idx].ptr += full_bytes;
                streams[stream_idx].len -= full_bytes;
                offset += full_bytes;
                continue;
            }
        }
        copy = MIN(wbs - buf_fill, streams[stream_idx].len);
        if (copy > 0U) {
            memcpy(buf + buf_fill, streams[stream_idx].ptr, copy);
            streams[stream_idx].ptr += copy;
            streams[stream_idx].len -= copy;
            buf_fill += copy;
        }
        if (buf_fill == wbs) {
            rc = fs->flash->write(fs->flash->ctx, offset, buf, wbs);
            if (rc) {
                return rc;
            }
            offset += wbs;
            buf_fill = 0U;
        }
    }
    if (buf_fill > 0U) {
        memset(buf + buf_fill, fs->flash->erase_value, wbs - buf_fill);
        rc = fs->flash->write(fs->flash->ctx, offset, buf, wbs);
        if (rc) {
            return rc;
        }
    }
    return 0;
}

static int nvs_flash_al_wrt(struct nvs_fs *fs, uint32_t addr, const void *data, size_t len) {
    struct nvs_flash_wrt_stream strm = { .data = { .ptr = data, .len = len } };
    return nvs_flash_al_wrt_streams(fs, addr, &strm);
}

static int nvs_flash_rd(struct nvs_fs *fs, uint32_t addr, void *data, size_t len) {
    return fs->flash->read(fs->flash->ctx, nvs_flash_offset(fs, addr), data, len);
}

static int nvs_flash_ate_wrt(struct nvs_fs *fs, const struct nvs_ate *entry) {
    int rc = nvs_flash_al_wrt(fs, fs->ate_wra, entry, sizeof(struct nvs_ate));
    /* 0xFFFF is a special-purpose identifier. Exclude it from the cache */
    if (entry->id != 0xFFFF) {
        fs->lookup_cache[nvs_lookup_cache_pos(fs, entry->id)] = fs->ate_wra;
    }
    fs->ate_wra -= nvs_al_size(fs, sizeof(struct nvs_ate));
    return rc;
}

static int nvs_flash_data_al_wrt(struct nvs_fs *fs, struct nvs_flash_wrt_stream *strm, bool compute_crc) {
    uint32_t data_crc;
    int rc;

    /* Only add the CRC if required (ignore deletion requests, i.e. when len is 0) */
    if (compute_crc && (strm->data.len > 0)) {
        data_crc = crc32_ieee(strm->data.ptr, strm->data.len);
        strm->tail.ptr = (const uint8_t *)&data_crc;
        strm->tail.len = sizeof(data_crc);
    }
    rc = nvs_flash_al_wrt_streams(fs, fs->data_wra, strm);
    fs->data_wra += nvs_al_size(fs, strm->head.len + strm->data.len + strm->tail.len);
    return rc;
}

static int nvs_flash_data_wrt(struct nvs_fs *fs, const void *data, size_t len, bool compute_crc) {
    struct nvs_flash_wrt_stream strm = { .data = { .ptr = data, .len = len } };
    return nvs_flash_data_al_wrt(fs, &strm, compute_crc);
}

static int nvs_flash_ate_rd(struct nvs_fs *fs, uint32_t addr, struct nvs_ate *entry) {
    return nvs_flash_rd(fs, addr, entry, sizeof(struct nvs_ate));
}

/* returns 0 if equal, 1 if not equal, errcode if error */
static int nvs_flash_block_cmp(struct nvs_fs *fs, uint32_t addr, const void *data, size_t len) {
    const uint8_t *data8 = data;
    int rc;
    size_t bytes_to_cmp, block_size;
    uint8_t buf[NVS_BLOCK_SIZE];

    block_size = NVS_BLOCK_SIZE & ~(nvs_al_size(fs, 1) - 1U);
    while (len) {
        bytes_to_cmp = MIN(block_size, len);
        rc = nvs_flash_rd(fs, addr, buf, bytes_to_cmp);
        if (rc) {
            return rc;
        }
        if (memcmp(data8, buf, bytes_to_cmp)) {
            return 1;
        }
        len -= bytes_to_cmp;
        addr += bytes_to_cmp;
        data8 += bytes_to_cmp;
    }
    return 0;
}

/* returns 0 if all data in flash is equal to value, 1 if not equal, errcode if error */
static int nvs_flash_cmp_const(struct nvs_fs *fs, uint32_t addr, uint8_t value, size_t len) {
    int rc;
    size_t bytes_to_cmp, block_size;
    uint8_t buf[NVS_BLOCK_SIZE];

    block_size = NVS_BLOCK_SIZE & ~(nvs_al_size(fs, 1) - 1U);
    while (len) {
        bytes_to_cmp = MIN(block_size, len);
        rc = nvs_flash_rd(fs, addr, buf, bytes_to_cmp);
        if (rc) {
            return rc;
        }
        for (size_t i = 0; i < bytes_to_cmp; i++) {
            if (buf[i] != value) {
                return 1;
            }
        }
        len -= bytes_to_cmp;
        addr += bytes_to_cmp;
    }
    return 0;
}

/* GC-only: move data at addr to data_wra in write_block_size chunks; an unaligned
 * tail stays in ctx->buffer for the caller to flush. */
static int nvs_flash_block_move(struct nvs_fs *fs, uint32_t addr, struct nvs_block_move_ctx *ctx, struct nvs_ate *gc_ate) {
    size_t wbs = nvs_al_size(fs, 1);
    size_t bytes_to_copy, block_size, tail_len;
    size_t len = gc_ate->len;
    int rc;

    len += ctx->buffer_pos;
    tail_len = len & (wbs - 1U);
    block_size = NVS_BLOCK_SIZE & ~(wbs - 1U);
    len -= tail_len;

    gc_ate->offset = (uint16_t)((fs->data_wra + ctx->buffer_pos) & ADDR_OFFS_MASK);

    while (len) {
        bytes_to_copy = MIN(block_size, len) - ctx->buffer_pos;
        rc = nvs_flash_rd(fs, addr, ctx->buffer + ctx->buffer_pos, bytes_to_copy);
        if (rc) {
            return rc;
        }
        /* The CRC is part of the moved bytes; the data did not change. */
        rc = nvs_flash_data_wrt(fs, ctx->buffer, bytes_to_copy + ctx->buffer_pos, false);
        if (rc) {
            return rc;
        }
        len -= bytes_to_copy + ctx->buffer_pos;
        addr += bytes_to_copy;
        ctx->buffer_pos = 0U;
    }
    if (tail_len) {
        rc = nvs_flash_rd(fs, addr, ctx->buffer + ctx->buffer_pos, tail_len - ctx->buffer_pos);
        if (rc) {
            return rc;
        }
        ctx->buffer_pos = tail_len;
    }
    return 0;
}

/* erase a sector and verify erase was OK. */
static int nvs_flash_erase_sector(struct nvs_fs *fs, uint32_t addr) {
    int rc;

    addr &= ADDR_SECT_MASK;
    nvs_lookup_cache_invalidate(fs, addr >> ADDR_SECT_SHIFT);
    rc = fs->flash->erase(fs->flash->ctx, nvs_flash_offset(fs, addr), fs->sector_size);
    if (rc) {
        return rc;
    }
    if (nvs_flash_cmp_const(fs, addr, fs->flash->erase_value, fs->sector_size)) {
        rc = -ENXIO;
    }
    return rc;
}

static inline uint16_t nvs_data_len_with_crc(size_t len) {
    return (uint16_t)(len ? len + NVS_DATA_CRC_SIZE : 0U);
}

static void nvs_ate_crc8_update(struct nvs_ate *entry) {
    entry->crc8 = crc8_ccitt(0xff, entry, offsetof(struct nvs_ate, crc8));
}

static int nvs_ate_crc8_check(const struct nvs_ate *entry) {
    return crc8_ccitt(0xff, entry, offsetof(struct nvs_ate, crc8)) == entry->crc8 ? 0 : 1;
}

static int nvs_ate_cmp_const(const struct nvs_ate *entry, uint8_t value) {
    const uint8_t *data8 = (const uint8_t *)entry;
    for (size_t i = 0; i < sizeof(struct nvs_ate); i++) {
        if (data8[i] != value) {
            return 1;
        }
    }
    return 0;
}

/* return 1 if crc8, offset and length are valid, 0 otherwise */
static int nvs_ate_valid(struct nvs_fs *fs, uint32_t entry_addr, const struct nvs_ate *entry) {
    (void)fs;
    uint32_t position = (uint32_t)entry->offset + entry->len;
    if (nvs_ate_crc8_check(entry) || (position > (entry_addr & ADDR_OFFS_MASK))) {
        return 0;
    }
    return 1;
}

static bool nvs_ate_is_marker(struct nvs_fs *fs, uint32_t addr, const struct nvs_ate *ate, uint16_t id) {
    return ate->id == id && ate->len == 0U && nvs_ate_valid(fs, addr, ate);
}

/* a valid sector close ate: valid, len 0, id 0xFFFF, offset on the ATE grid */
static int nvs_close_ate_valid(struct nvs_fs *fs, const struct nvs_ate *entry) {
    size_t ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

    if ((!nvs_ate_valid(fs, (uint32_t)(fs->sector_size - ate_size), entry)) || (entry->len != 0U) || (entry->id != 0xFFFF)) {
        return 0;
    }
    if ((fs->sector_size - entry->offset) % ate_size) {
        return 0;
    }
    return 1;
}

static int nvs_flash_wrt_entry(struct nvs_fs *fs, uint16_t id, const void *data, size_t len) {
    int rc;
    struct nvs_ate entry;

    entry.id = id;
    entry.offset = (uint16_t)(fs->data_wra & ADDR_OFFS_MASK);
    entry.len = nvs_data_len_with_crc(len);
    entry.part = 0xff;

    rc = nvs_flash_data_wrt(fs, data, len, true);
    if (rc) {
        return rc;
    }
    nvs_ate_crc8_update(&entry);
    return nvs_flash_ate_wrt(fs, &entry);
}

/* If the closing ate is invalid, find the last valid ate by going through all
 * of them. addr points to the faulty closing ate and is updated to the last
 * valid ate; left untouched if none is found.
 *
 * Batches: a batch whose begin marker has no commit marker after it is not part
 * of the sector, so the result is the last valid ate before that begin marker. */
static int nvs_recover_last_ate(struct nvs_fs *fs, uint32_t *addr) {
    uint32_t data_end_addr, ate_end_addr, before_batch = 0;
    struct nvs_ate end_ate;
    size_t ate_size;
    bool in_batch = false, before_batch_set = false;
    uint32_t start = *addr;
    int rc;

    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

    *addr -= ate_size;
    ate_end_addr = *addr;
    data_end_addr = *addr & ADDR_SECT_MASK;
    uint32_t last_valid = *addr;
    bool last_valid_set = false;
    while ((ate_end_addr >= data_end_addr) && (ate_end_addr & ADDR_OFFS_MASK) != 0U) {
        rc = nvs_flash_ate_rd(fs, ate_end_addr, &end_ate);
        if (rc) {
            return rc;
        }
        if (nvs_ate_valid(fs, ate_end_addr, &end_ate)) {
            if (!in_batch && nvs_ate_is_marker(fs, ate_end_addr, &end_ate, NVS_ID_BATCH_BEGIN)) {
                in_batch = true;
                before_batch = last_valid;
                before_batch_set = last_valid_set;
            }
            else if (in_batch && nvs_ate_is_marker(fs, ate_end_addr, &end_ate, NVS_ID_BATCH_COMMIT)) {
                in_batch = false;
            }
            data_end_addr &= ADDR_SECT_MASK;
            data_end_addr += end_ate.offset + end_ate.len;
            *addr = ate_end_addr;
            last_valid = ate_end_addr;
            last_valid_set = true;
        }
        ate_end_addr -= ate_size;
    }
    if (in_batch) {
        *addr = before_batch_set ? before_batch : start - ate_size;
    }
    return 0;
}

/* walk the allocation entry list from newest to oldest: read the ate at addr,
 * move addr to the previous ate */
static int nvs_prev_ate(struct nvs_fs *fs, uint32_t *addr, struct nvs_ate *ate) {
    int rc;
    struct nvs_ate close_ate;
    size_t ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

    rc = nvs_flash_ate_rd(fs, *addr, ate);
    if (rc) {
        return rc;
    }
    *addr += ate_size;
    if (((*addr) & ADDR_OFFS_MASK) != (fs->sector_size - ate_size)) {
        return 0;
    }

    /* last ate in sector, do jump to previous sector */
    if (((*addr) >> ADDR_SECT_SHIFT) == 0U) {
        *addr += ((uint32_t)(fs->sector_count - 1) << ADDR_SECT_SHIFT);
    }
    else {
        *addr -= (1 << ADDR_SECT_SHIFT);
    }

    rc = nvs_flash_ate_rd(fs, *addr, &close_ate);
    if (rc) {
        return rc;
    }
    /* at the end of filesystem */
    if (!nvs_ate_cmp_const(&close_ate, fs->flash->erase_value)) {
        *addr = fs->ate_wra;
        return 0;
    }
    if (nvs_close_ate_valid(fs, &close_ate)) {
        (*addr) &= ADDR_SECT_MASK;
        (*addr) += close_ate.offset;
        return 0;
    }
    return nvs_recover_last_ate(fs, addr);
}

static void nvs_sector_advance(struct nvs_fs *fs, uint32_t *addr) {
    *addr += (1 << ADDR_SECT_SHIFT);
    if ((*addr >> ADDR_SECT_SHIFT) == fs->sector_count) {
        *addr -= ((uint32_t)fs->sector_count << ADDR_SECT_SHIFT);
    }
}

/* Close the current sector with a close ate pointing at last_ate (the newest ate
 * that belongs to the sector), and move to the next sector. Upstream ignored the
 * result of the close ate write (zephyr#122391). */
static int nvs_sector_close_at(struct nvs_fs *fs, uint32_t last_ate) {
    struct nvs_ate close_ate;
    size_t ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
    int rc;

    close_ate.id = 0xFFFF;
    close_ate.len = 0U;
    close_ate.offset = (uint16_t)(last_ate & ADDR_OFFS_MASK);
    close_ate.part = 0xff;

    fs->ate_wra &= ADDR_SECT_MASK;
    fs->ate_wra += (fs->sector_size - ate_size);

    nvs_ate_crc8_update(&close_ate);
    rc = nvs_flash_ate_wrt(fs, &close_ate);

    nvs_sector_advance(fs, &fs->ate_wra);
    fs->data_wra = fs->ate_wra & ADDR_SECT_MASK;
    return rc;
}

static int nvs_sector_close(struct nvs_fs *fs) {
    return nvs_sector_close_at(fs, fs->ate_wra + nvs_al_size(fs, sizeof(struct nvs_ate)));
}

static int nvs_add_marker_ate(struct nvs_fs *fs, uint16_t id) {
    struct nvs_ate ate;

    ate.id = id;
    ate.len = 0U;
    ate.part = 0xff;
    ate.offset = (uint16_t)(fs->data_wra & ADDR_OFFS_MASK);
    nvs_ate_crc8_update(&ate);
    return nvs_flash_ate_wrt(fs, &ate);
}

static int nvs_add_gc_done_ate(struct nvs_fs *fs) {
    return nvs_add_marker_ate(fs, 0xFFFF);
}

static int nvs_gc_flush_and_try_write(struct nvs_fs *fs, struct nvs_block_move_ctx *bm_ctx, struct nvs_gc_write_entry *entry) {
    struct nvs_flash_wrt_stream strm = { .head = { .ptr = bm_ctx->buffer, .len = bm_ctx->buffer_pos } };
    size_t required_space = 0U;
    struct nvs_ate wrt_ate;
    size_t ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
    int rc;

    if (entry) {
        required_space = ate_size + nvs_al_size(fs, bm_ctx->buffer_pos + nvs_data_len_with_crc(entry->len));
    }
    if (!entry || (fs->ate_wra < (fs->data_wra + required_space))) {
        if (bm_ctx->buffer_pos > 0U) {
            rc = nvs_flash_data_al_wrt(fs, &strm, false);
            if (rc) {
                return rc;
            }
        }
        return 0;
    }

    strm.data.ptr = entry->data;
    strm.data.len = entry->len;
    wrt_ate.offset = (uint16_t)((fs->data_wra + bm_ctx->buffer_pos) & ADDR_OFFS_MASK);

    rc = nvs_flash_data_al_wrt(fs, &strm, true);
    if (rc) {
        return rc;
    }
    wrt_ate.id = entry->id;
    wrt_ate.len = nvs_data_len_with_crc(entry->len);
    wrt_ate.part = 0xff;
    nvs_ate_crc8_update(&wrt_ate);
    rc = nvs_flash_ate_wrt(fs, &wrt_ate);
    if (rc) {
        return rc;
    }
    entry->is_written = true;
    return 0;
}

/* garbage collection: ate_wra has moved to the new sector; the data to collect is
 * in the sector after it. */
static int nvs_gc(struct nvs_fs *fs, struct nvs_gc_write_entry *entry) {
    int rc;
    struct nvs_ate close_ate, gc_ate, wlk_ate;
    uint32_t sec_addr, gc_addr, gc_prev_addr, wlk_addr, wlk_prev_addr, data_addr, stop_addr;
    struct nvs_block_move_ctx ctx = { .buffer_pos = 0U };
    size_t ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

    sec_addr = (fs->ate_wra & ADDR_SECT_MASK);
    nvs_sector_advance(fs, &sec_addr);
    gc_addr = sec_addr + fs->sector_size - ate_size;

    rc = nvs_flash_ate_rd(fs, gc_addr, &close_ate);
    if (rc < 0) {
        return rc;
    }
    /* if the sector is not closed don't do gc */
    if (!nvs_ate_cmp_const(&close_ate, fs->flash->erase_value)) {
        goto gc_done;
    }

    stop_addr = gc_addr - ate_size;

    if (nvs_close_ate_valid(fs, &close_ate)) {
        gc_addr &= ADDR_SECT_MASK;
        gc_addr += close_ate.offset;
    }
    else {
        rc = nvs_recover_last_ate(fs, &gc_addr);
        if (rc) {
            return rc;
        }
    }

    do {
        gc_prev_addr = gc_addr;
        rc = nvs_prev_ate(fs, &gc_addr, &gc_ate);
        if (rc) {
            return rc;
        }
        if (!nvs_ate_valid(fs, gc_prev_addr, &gc_ate)) {
            continue;
        }

        wlk_addr = fs->lookup_cache[nvs_lookup_cache_pos(fs, gc_ate.id)];
        if (wlk_addr == NVS_LOOKUP_CACHE_NO_ADDR) {
            wlk_addr = fs->ate_wra;
        }
        do {
            wlk_prev_addr = wlk_addr;
            rc = nvs_prev_ate(fs, &wlk_addr, &wlk_ate);
            if (rc) {
                return rc;
            }
            /* only consider valid ates with the same id */
            if ((wlk_ate.id == gc_ate.id) && (nvs_ate_valid(fs, wlk_prev_addr, &wlk_ate))) {
                break;
            }
        } while (wlk_addr != fs->ate_wra);

        /* the newest entry for this id: move it unless it is a delete (len 0) */
        if ((wlk_prev_addr == gc_prev_addr) && gc_ate.len) {
            if (entry && (entry->id == gc_ate.id) && (nvs_data_len_with_crc(entry->len) <= gc_ate.len)) {
                /* will be written in the final flush */
                continue;
            }
            data_addr = (gc_prev_addr & ADDR_SECT_MASK) + gc_ate.offset;
            rc = nvs_flash_block_move(fs, data_addr, &ctx, &gc_ate);
            if (rc) {
                return rc;
            }
            nvs_ate_crc8_update(&gc_ate);
            rc = nvs_flash_ate_wrt(fs, &gc_ate);
            if (rc) {
                return rc;
            }
        }
    } while (gc_prev_addr != stop_addr);

    rc = nvs_gc_flush_and_try_write(fs, &ctx, entry);
    if (rc) {
        return rc;
    }

gc_done:
    /* mark gc done, if there is room */
    if (fs->ate_wra >= (fs->data_wra + ate_size)) {
        rc = nvs_add_gc_done_ate(fs);
        if (rc) {
            return rc;
        }
    }
    return nvs_flash_erase_sector(fs, sec_addr);
}

/* A batch begun in the open sector but not committed (power lost while it was
 * written): close the sector just before its begin marker, so neither lookups nor
 * garbage collection ever see its entries, and continue in the next sector. */
static int nvs_batch_abort(struct nvs_fs *fs, uint32_t begin_addr) {
    size_t ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
    int rc = nvs_sector_close_at(fs, begin_addr + ate_size);
    if (rc) {
        return rc;
    }
    /* the cache still points at the aborted entries, and gc relies on it to find
     * the newest copy of each id */
    rc = nvs_lookup_cache_rebuild(fs);
    if (rc) {
        return rc;
    }
    rc = nvs_gc(fs, NULL);
    if (rc) {
        return rc;
    }
    return nvs_lookup_cache_rebuild(fs);
}

static int nvs_batch_recover(struct nvs_fs *fs) {
    size_t ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
    uint32_t sector = fs->ate_wra & ADDR_SECT_MASK;
    uint32_t end = sector + fs->sector_size - 2 * ate_size;
    struct nvs_ate ate;
    int rc;

    for (uint32_t addr = fs->ate_wra + ate_size; addr <= end; addr += ate_size) {
        rc = nvs_flash_ate_rd(fs, addr, &ate);
        if (rc) {
            return rc;
        }
        if (nvs_ate_is_marker(fs, addr, &ate, NVS_ID_BATCH_COMMIT)) {
            return 0;
        }
        if (nvs_ate_is_marker(fs, addr, &ate, NVS_ID_BATCH_BEGIN)) {
            return nvs_batch_abort(fs, addr);
        }
    }
    return 0;
}

static int nvs_startup(struct nvs_fs *fs) {
    int rc;
    struct nvs_ate last_ate;
    size_t ate_size, empty_len;
    uint32_t addr = 0U;
    uint16_t i, closed_sectors = 0;
    uint8_t erase_value = fs->flash->erase_value;

    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
    /* find an open sector following a closed sector: this is where NVS writes */
    for (i = 0; i < fs->sector_count; i++) {
        addr = ((uint32_t)i << ADDR_SECT_SHIFT) + (uint16_t)(fs->sector_size - ate_size);
        rc = nvs_flash_cmp_const(fs, addr, erase_value, sizeof(struct nvs_ate));
        if (rc) {
            /* closed sector */
            closed_sectors++;
            nvs_sector_advance(fs, &addr);
            rc = nvs_flash_cmp_const(fs, addr, erase_value, sizeof(struct nvs_ate));
            if (!rc) {
                /* open sector */
                break;
            }
        }
    }
    /* all sectors are closed: not an nvs fs, or irreparably corrupted */
    if (closed_sectors == fs->sector_count) {
        return -EDEADLK;
    }

    if (i == fs->sector_count) {
        /* none of the sectors were closed: start in the first sector, unless there
         * are only two sectors and the last one has ates */
        rc = nvs_flash_cmp_const(fs, addr - ate_size, erase_value, sizeof(struct nvs_ate));
        if (!rc) {
            nvs_sector_advance(fs, &addr);
        }
    }

    /* addr is the closing ate of the most recent sector: find its last valid ate */
    rc = nvs_recover_last_ate(fs, &addr);
    if (rc) {
        goto end;
    }

    /* find the first erased ate, updating data_wra on the way */
    fs->ate_wra = addr;
    fs->data_wra = addr & ADDR_SECT_MASK;

    while (fs->ate_wra >= fs->data_wra) {
        rc = nvs_flash_ate_rd(fs, fs->ate_wra, &last_ate);
        if (rc) {
            goto end;
        }
        if (!nvs_ate_cmp_const(&last_ate, erase_value)) {
            break;
        }
        if (nvs_ate_valid(fs, fs->ate_wra, &last_ate)) {
            fs->data_wra = addr & ADDR_SECT_MASK;
            fs->data_wra += nvs_al_size(fs, last_ate.offset + last_ate.len);
            /* the ate on the last position of a sector is reserved for a delete */
            if (fs->ate_wra == fs->data_wra && last_ate.len) {
                rc = -ESPIPE;
                goto end;
            }
        }
        fs->ate_wra -= ate_size;
    }

    /* if the sector after the write sector is not empty, gc was interrupted:
     * erase it if gc had finished, else restart gc */
    addr = fs->ate_wra & ADDR_SECT_MASK;
    nvs_sector_advance(fs, &addr);
    rc = nvs_flash_cmp_const(fs, addr, erase_value, fs->sector_size);
    if (rc < 0) {
        goto end;
    }
    if (rc) {
        bool gc_done_marker = false;
        struct nvs_ate gc_done_ate;

        addr = fs->ate_wra + ate_size;
        while ((addr & ADDR_OFFS_MASK) < (fs->sector_size - ate_size)) {
            rc = nvs_flash_ate_rd(fs, addr, &gc_done_ate);
            if (rc) {
                goto end;
            }
            if (nvs_ate_valid(fs, addr, &gc_done_ate) && (gc_done_ate.id == 0xffff) && (gc_done_ate.len == 0U)) {
                gc_done_marker = true;
                break;
            }
            addr += ate_size;
        }
        if (gc_done_marker) {
            addr = fs->ate_wra & ADDR_SECT_MASK;
            nvs_sector_advance(fs, &addr);
            rc = nvs_flash_erase_sector(fs, addr);
            goto end;
        }
        rc = nvs_flash_erase_sector(fs, fs->ate_wra);
        if (rc) {
            goto end;
        }
        fs->ate_wra &= ADDR_SECT_MASK;
        fs->ate_wra += (fs->sector_size - 2 * ate_size);
        fs->data_wra = (fs->ate_wra & ADDR_SECT_MASK);
        /* gc uses the lookup cache, which is not built yet: point every slot at the
         * end of the fs; it is rebuilt afterwards */
        for (i = 0; i < fs->lookup_cache_size; i++) {
            fs->lookup_cache[i] = fs->ate_wra;
        }
        rc = nvs_gc(fs, NULL);
        goto end;
    }

    /* possible data write after the last ate write: update data_wra */
    while (fs->ate_wra > fs->data_wra) {
        empty_len = fs->ate_wra - fs->data_wra;
        rc = nvs_flash_cmp_const(fs, fs->data_wra, erase_value, empty_len);
        if (rc < 0) {
            goto end;
        }
        if (!rc) {
            break;
        }
        fs->data_wra += nvs_al_size(fs, 1);
        /* no erased space left: the next write closes the sector */
        rc = 0;
    }

    /* ate_wra at the first ate position with data written: the sector holds no
     * valid data, erase it */
    if (((fs->ate_wra + 2 * ate_size) == fs->sector_size) && (fs->data_wra != (fs->ate_wra & ADDR_SECT_MASK))) {
        rc = nvs_flash_erase_sector(fs, fs->ate_wra);
        if (rc) {
            goto end;
        }
        fs->data_wra = fs->ate_wra & ADDR_SECT_MASK;
    }

end:
    if (!rc) {
        rc = nvs_lookup_cache_rebuild(fs);
    }
    if (!rc) {
        rc = nvs_batch_recover(fs);
    }
    /* empty sector: add a gc done ate so gc always has room */
    if ((!rc) && ((fs->ate_wra & ADDR_OFFS_MASK) == (fs->sector_size - 2 * ate_size))) {
        rc = nvs_add_gc_done_ate(fs);
    }
    return rc;
}

int nvs_clear(struct nvs_fs *fs) {
    int rc;

    if (!fs->ready) {
        return -EACCES;
    }
    for (uint16_t i = 0; i < fs->sector_count; i++) {
        rc = nvs_flash_erase_sector(fs, (uint32_t)i << ADDR_SECT_SHIFT);
        if (rc) {
            return rc;
        }
    }
    /* nvs needs to be mounted again after clearing */
    fs->ready = false;
    return 0;
}

int nvs_mount(struct nvs_fs *fs) {
    int rc;
    size_t wbs;

    fs->ready = false;
    if (!fs->flash || !fs->flash->read || !fs->flash->write || !fs->flash->erase || !fs->lookup_cache || fs->lookup_cache_size == 0) {
        return -EINVAL;
    }
    wbs = fs->flash->write_block_size ? fs->flash->write_block_size : 1;
    if (wbs > NVS_BLOCK_SIZE || (wbs & (wbs - 1))) {
        return -EINVAL;
    }
    if (!fs->sector_size || fs->sector_size > NVS_MAX_SECTOR_SIZE) {
        return -EINVAL;
    }
    if (fs->sector_count < 2) {
        return -EINVAL;
    }
    rc = nvs_startup(fs);
    if (rc) {
        return rc;
    }
    fs->ready = true;
    return 0;
}

/* Find the newest valid ate for id. Returns 1 and its address if found, 0 if not. */
static int nvs_find(struct nvs_fs *fs, uint16_t id, uint32_t *ate_addr, struct nvs_ate *ate) {
    uint32_t wlk_addr = fs->lookup_cache[nvs_lookup_cache_pos(fs, id)], rd_addr;
    int rc;

    if (wlk_addr == NVS_LOOKUP_CACHE_NO_ADDR) {
        return 0;
    }
    while (1) {
        rd_addr = wlk_addr;
        rc = nvs_prev_ate(fs, &wlk_addr, ate);
        if (rc) {
            return rc;
        }
        if ((ate->id == id) && nvs_ate_valid(fs, rd_addr, ate)) {
            *ate_addr = rd_addr;
            return 1;
        }
        if (wlk_addr == fs->ate_wra) {
            return 0;
        }
    }
}

/* 1 if writing (id, data, len) would not change what is stored, 0 if it would,
 * negative on error. */
static int nvs_unchanged(struct nvs_fs *fs, uint16_t id, const void *data, size_t len) {
    struct nvs_ate ate;
    uint32_t ate_addr = 0;
    int rc = nvs_find(fs, id, &ate_addr, &ate);

    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        /* delete of a non-existing entry */
        return len == 0;
    }
    if (len == 0) {
        return ate.len == 0U;
    }
    if (len + NVS_DATA_CRC_SIZE != ate.len) {
        return 0;
    }
    rc = nvs_flash_block_cmp(fs, (ate_addr & ADDR_SECT_MASK) + ate.offset, data, len);
    if (rc < 0) {
        return rc;
    }
    return rc == 0;
}

static ssize_t nvs_write_internal(struct nvs_fs *fs, uint16_t id, const void *data, size_t len) {
    int rc, gc_count;
    struct nvs_gc_write_entry wrt_entry;
    size_t ate_size, data_size;
    uint16_t required_space = 0U; /* no space, appropriate for delete ate */

    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
    data_size = nvs_al_size(fs, nvs_data_len_with_crc(len));

    /* largest value: sector less 4 ates (data, sector close, gc done, and one
     * always kept for a delete) and the data CRC */
    if ((data_size > (fs->sector_size - 4 * ate_size)) || ((len > 0) && (data == NULL))) {
        return -EINVAL;
    }
    rc = nvs_unchanged(fs, id, data, len);
    if (rc) {
        return rc < 0 ? rc : 0;
    }
    if (data_size) {
        /* leave space for a delete ate */
        required_space = (uint16_t)(data_size + ate_size);
    }

    gc_count = 0;
    while (1) {
        if (gc_count == fs->sector_count) {
            /* gc'ed all sectors, no extra space will be created */
            return -ENOSPC;
        }
        /* never write an ate at the start of a sector */
        if (fs->ate_wra >= (fs->data_wra + required_space) && (fs->ate_wra & ADDR_OFFS_MASK) != 0) {
            rc = nvs_flash_wrt_entry(fs, id, data, len);
            if (rc) {
                return rc;
            }
            break;
        }
        rc = nvs_sector_close(fs);
        if (rc) {
            return rc;
        }
        if (gc_count == 0) {
            wrt_entry.id = id;
            wrt_entry.data = data;
            wrt_entry.len = (uint16_t)len;
            wrt_entry.is_written = false;
        }
        rc = nvs_gc(fs, &wrt_entry);
        if (rc) {
            return rc;
        }
        gc_count++;
        if (wrt_entry.is_written) {
            break;
        }
    }
    return (ssize_t)len;
}

ssize_t nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len) {
    if (!fs->ready) {
        return -EACCES;
    }
    if (id >= NVS_ID_RESERVED) {
        return -EINVAL;
    }
    return nvs_write_internal(fs, id, data, len);
}

int nvs_delete(struct nvs_fs *fs, uint16_t id) {
    ssize_t rc = nvs_write(fs, id, NULL, 0);
    return rc < 0 ? (int)rc : 0;
}

int nvs_batch_write(struct nvs_fs *fs, const struct nvs_batch_entry *entries, size_t count) {
    size_t ate_size, need, changed = 0;
    const struct nvs_batch_entry *only = NULL;
    uint32_t begin_addr;
    int rc, gc_count;

    if (!fs->ready) {
        return -EACCES;
    }
    if (count && !entries) {
        return -EINVAL;
    }
    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

    /* begin marker, commit marker, and the ate always kept for a delete */
    need = 3 * ate_size;
    for (size_t i = 0; i < count; i++) {
        const struct nvs_batch_entry *e = &entries[i];
        if (e->id >= NVS_ID_RESERVED || (e->len && !e->data) || e->len > 0xFFFF - NVS_DATA_CRC_SIZE) {
            return -EINVAL;
        }
        for (size_t j = 0; j < i; j++) {
            if (entries[j].id == e->id) {
                return -EINVAL;
            }
        }
        rc = nvs_unchanged(fs, e->id, e->data, e->len);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            changed++;
            only = e;
            need += ate_size + nvs_al_size(fs, nvs_data_len_with_crc(e->len));
        }
    }
    if (changed == 0) {
        return 0;
    }
    if (changed == 1) {
        /* a single write is already all-or-nothing */
        ssize_t r = nvs_write_internal(fs, only->id, only->data, only->len);
        return r < 0 ? (int)r : 0;
    }
    /* a fresh sector also holds the sector close and gc done ates */
    if (need > fs->sector_size - 2 * ate_size) {
        return -ENOSPC;
    }

    /* make room in the current sector first, so gc never runs inside the batch */
    gc_count = 0;
    while (!(fs->ate_wra >= fs->data_wra + need && (fs->ate_wra & ADDR_OFFS_MASK) != 0)) {
        if (gc_count == fs->sector_count) {
            return -ENOSPC;
        }
        rc = nvs_sector_close(fs);
        if (rc) {
            return rc;
        }
        rc = nvs_gc(fs, NULL);
        if (rc) {
            return rc;
        }
        gc_count++;
    }

    begin_addr = fs->ate_wra;
    rc = nvs_add_marker_ate(fs, NVS_ID_BATCH_BEGIN);
    for (size_t i = 0; rc == 0 && i < count; i++) {
        const struct nvs_batch_entry *e = &entries[i];
        int u = nvs_unchanged(fs, e->id, e->data, e->len);
        if (u < 0) {
            rc = u;
        }
        else if (u == 0) {
            rc = nvs_flash_wrt_entry(fs, e->id, e->data, e->len);
        }
    }
    if (rc == 0) {
        rc = nvs_add_marker_ate(fs, NVS_ID_BATCH_COMMIT);
    }
    if (rc) {
        /* a write failed part way: drop the batch as a power loss would */
        int ra = nvs_batch_abort(fs, begin_addr);
        if (ra) {
            fs->ready = false;
        }
        return rc;
    }
    return 0;
}

ssize_t nvs_read_hist(struct nvs_fs *fs, uint16_t id, void *data, size_t len, uint16_t cnt) {
    int rc;
    uint32_t wlk_addr, rd_addr;
    uint16_t cnt_his;
    struct nvs_ate wlk_ate;
    size_t ate_size;
    uint32_t read_data_crc, computed_data_crc;

    if (!fs->ready) {
        return -EACCES;
    }
    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
    if (len > (fs->sector_size - 2 * ate_size)) {
        return -EINVAL;
    }
    cnt_his = 0U;

    wlk_addr = fs->lookup_cache[nvs_lookup_cache_pos(fs, id)];
    if (wlk_addr == NVS_LOOKUP_CACHE_NO_ADDR) {
        return -ENOENT;
    }
    rd_addr = wlk_addr;

    while (cnt_his <= cnt) {
        rd_addr = wlk_addr;
        rc = nvs_prev_ate(fs, &wlk_addr, &wlk_ate);
        if (rc) {
            return rc;
        }
        if ((wlk_ate.id == id) && (nvs_ate_valid(fs, rd_addr, &wlk_ate))) {
            cnt_his++;
        }
        if (wlk_addr == fs->ate_wra) {
            break;
        }
    }
    if (((wlk_addr == fs->ate_wra) && (wlk_ate.id != id)) || (wlk_ate.len == 0U) || (cnt_his < cnt)) {
        return -ENOENT;
    }
    if (wlk_ate.len < NVS_DATA_CRC_SIZE) {
        return -ENOENT;
    }
    rd_addr &= ADDR_SECT_MASK;
    rd_addr += wlk_ate.offset;
    rc = nvs_flash_rd(fs, rd_addr, data, MIN(len, (size_t)wlk_ate.len - NVS_DATA_CRC_SIZE));
    if (rc) {
        return rc;
    }
    /* check the data CRC if the whole value was read */
    if (len >= (size_t)(wlk_ate.len - NVS_DATA_CRC_SIZE)) {
        rd_addr += wlk_ate.len - NVS_DATA_CRC_SIZE;
        rc = nvs_flash_rd(fs, rd_addr, &read_data_crc, sizeof(read_data_crc));
        if (rc) {
            return rc;
        }
        computed_data_crc = crc32_ieee(data, wlk_ate.len - NVS_DATA_CRC_SIZE);
        if (read_data_crc != computed_data_crc) {
            return -EIO;
        }
    }
    return wlk_ate.len - NVS_DATA_CRC_SIZE;
}

ssize_t nvs_read(struct nvs_fs *fs, uint16_t id, void *data, size_t len) {
    return nvs_read_hist(fs, id, data, len, 0);
}

int nvs_locate(struct nvs_fs *fs, uint16_t id, uint32_t *offset, size_t *len) {
    struct nvs_ate ate;
    uint32_t ate_addr = 0;
    int rc;

    if (!fs->ready) {
        return -EACCES;
    }
    rc = nvs_find(fs, id, &ate_addr, &ate);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0 || ate.len < NVS_DATA_CRC_SIZE) {
        return -ENOENT;
    }
    *offset = nvs_flash_offset(fs, (ate_addr & ADDR_SECT_MASK) + ate.offset);
    *len = ate.len - NVS_DATA_CRC_SIZE;
    return 0;
}

int nvs_foreach(struct nvs_fs *fs, nvs_foreach_cb cb, void *ctx, uint8_t *seen) {
    uint32_t addr, ate_addr;
    struct nvs_ate ate;
    int rc;

    if (!fs->ready) {
        return -EACCES;
    }
    /* one bit per id: the walk goes newest first, so the first sighting of an id
     * is its current value, or its delete */
    memset(seen, 0, NVS_FOREACH_WORK);
    addr = fs->ate_wra;
    while (true) {
        ate_addr = addr;
        rc = nvs_prev_ate(fs, &addr, &ate);
        if (rc) {
            return rc;
        }
        if (ate.id < NVS_ID_RESERVED && nvs_ate_valid(fs, ate_addr, &ate) && !(seen[ate.id >> 3] & (1u << (ate.id & 7)))) {
            seen[ate.id >> 3] |= (uint8_t)(1u << (ate.id & 7));
            if (ate.len >= NVS_DATA_CRC_SIZE) {
                rc = cb(ctx, ate.id, nvs_flash_offset(fs, (ate_addr & ADDR_SECT_MASK) + ate.offset), ate.len - NVS_DATA_CRC_SIZE);
                if (rc) {
                    return rc;
                }
            }
        }
        if (addr == fs->ate_wra) {
            break;
        }
    }
    return 0;
}

ssize_t nvs_calc_free_space(struct nvs_fs *fs) {
    uint32_t step_prev_addr, step_addr, wlk_addr;
    struct nvs_ate step_ate, wlk_ate;
    size_t ate_size, free_space;
    int rc;

    if (!fs->ready) {
        return -EACCES;
    }
    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
    /* each sector keeps a close ate and a delete ate; one sector is kept for gc */
    free_space = (fs->sector_count - 1) * (fs->sector_size - (2 * ate_size));
    step_addr = fs->ate_wra;

    while (1) {
        step_prev_addr = step_addr;
        rc = nvs_prev_ate(fs, &step_addr, &step_ate);
        if (rc) {
            return rc;
        }
        wlk_addr = fs->ate_wra;
        while (1) {
            rc = nvs_prev_ate(fs, &wlk_addr, &wlk_ate);
            if (rc) {
                return rc;
            }
            if ((wlk_ate.id == step_ate.id) || (wlk_addr == fs->ate_wra)) {
                break;
            }
        }
        if (nvs_ate_valid(fs, step_prev_addr, &step_ate)) {
            if (step_ate.len == 0) {
                if (step_ate.id >= NVS_ID_RESERVED) {
                    free_space -= ate_size;
                }
            }
            else if (wlk_addr == step_addr) {
                free_space -= nvs_al_size(fs, step_ate.len);
                free_space -= ate_size;
            }
        }
        if (step_addr == fs->ate_wra) {
            break;
        }
    }
    return free_space;
}

size_t nvs_sector_max_data_size(struct nvs_fs *fs) {
    if (!fs->ready) {
        return 0;
    }
    size_t ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
    return fs->ate_wra - fs->data_wra - ate_size - NVS_DATA_CRC_SIZE;
}

int nvs_sector_use_next(struct nvs_fs *fs) {
    int ret;

    if (!fs->ready) {
        return -EACCES;
    }
    ret = nvs_sector_close(fs);
    if (ret != 0) {
        return ret;
    }
    return nvs_gc(fs, NULL);
}

uint32_t nvs_crc32(const void *buf, size_t len) {
    return crc32_ieee(buf, len);
}

ssize_t nvs_peek(const struct nvs_flash *flash, uint32_t sector_size, uint16_t sector_count, uint16_t id, void *data, size_t len) {
    struct nvs_ate ate;
    const size_t ate_size = sizeof(struct nvs_ate);
    uint32_t crc;

    if (!flash || !flash->read || flash->write_block_size > 1 || sector_size > NVS_MAX_SECTOR_SIZE || sector_size < 4 * ate_size) {
        return -EINVAL;
    }
    for (uint32_t s = 0; s < sector_count; s++) {
        uint32_t base = s * sector_size;
        /* the close ate is at the top; entries go down from below it */
        for (uint32_t a = sector_size - 2 * ate_size; ; a -= ate_size) {
            if (flash->read(flash->ctx, base + a, &ate, sizeof(ate))) {
                return -EIO;
            }
            if (!nvs_ate_cmp_const(&ate, flash->erase_value)) {
                break;
            }
            if (!nvs_ate_crc8_check(&ate) && ate.id == id && (uint32_t)ate.offset + ate.len <= a &&
                ate.len >= NVS_DATA_CRC_SIZE && (size_t)(ate.len - NVS_DATA_CRC_SIZE) <= len) {
                size_t n = ate.len - NVS_DATA_CRC_SIZE;
                if (flash->read(flash->ctx, base + ate.offset, data, n) || flash->read(flash->ctx, base + ate.offset + n, &crc, sizeof(crc))) {
                    return -EIO;
                }
                if (crc == crc32_ieee(data, n)) {
                    return (ssize_t)n;
                }
            }
            if (a < ate_size) {
                break;
            }
        }
    }
    return -ENOENT;
}
