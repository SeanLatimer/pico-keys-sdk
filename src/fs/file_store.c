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
 * Files kept in NVS (nvs/nvs.c) instead of being rewritten in place.
 *
 * Layout of the data area, bottom up: the counter log's two sectors
 * (counter_log.c), the data store, the persistent store (files marked
 * FILE_PERSISTENT, which INITIALIZE keeps), then whatever the old in-place file
 * system left, which is copied in once and erased.
 *
 * A file of up to INLINE_MAX bytes is one value: a length (2 bytes, or FFFF and
 * 4 bytes) and the payload, as the old format stored it, so file_t and its
 * readers are unchanged: file->data points at the length, in flash once
 * committed. A larger file is a head (FFFF, the length, the chunk count and the
 * chunk IDs) plus CHUNK-byte chunks under IDs FILE_STORE_CHUNK_FIRST..LAST.
 *
 * A command's writes are kept in RAM (the arena) until flash_commit(), which
 * writes them as one batch: all of them survive a power cut, or none do.
 * Chunks are written at once under fresh IDs that nothing refers to yet; the
 * head that refers to them, and the deletion of the chunks they replace, are
 * part of the batch.
 */

#include "picokeys.h"
#include "file.h"
#include "flash.h"
#include "counter_log.h"
#include "nvs/nvs.h"
#include <errno.h>
#include <stdio.h>

#ifndef PICOKEYS_LEGACY_FS

#ifdef ENABLE_EMULATION
#define FS_ERASE 0x4000u
#define FS_PAGE  0x400u
#else
#define FS_ERASE 0x1000u
#define FS_PAGE  0x100u
#endif

#define STORE_SECTOR      0x4000u
#ifndef FILE_STORE_DATA_SECTORS
#define FILE_STORE_DATA_SECTORS 64u      /* 1 MB */
#endif
#define STORE_PERM_SECTORS 2u
#define STORE_MAX_VALUE   NVS_MAX_VALUE(STORE_SECTOR)
#define STORE_FORMAT_ID   FILE_STORE_RESERVED_FID   /* in the persistent store */
static const uint8_t store_format[4] = { 'P', 'K', 'S', '1' };

/* Two chunks, each with its 4-byte CRC and 8-byte entry, fit a store sector
   beside the 3 entries every sector keeps (close, gc done, one for a delete).
   An 8 KB chunk would leave half of every sector unused. */
#define CHUNK        8160u
#define INLINE_MAX   CHUNK
_Static_assert(2u * (CHUNK + 4u + 8u) + 3u * 8u <= STORE_SECTOR, "two chunks per store sector");
#define CHUNK_IDS    (FILE_STORE_CHUNK_LAST - FILE_STORE_CHUNK_FIRST + 1u)
#define HEAD_FIXED   8u     /* FFFF, length, chunk count */

#define ARENA_SIZE  (2u * INLINE_MAX + 1024u)
#define MAX_PENDING 96u

_Static_assert(INLINE_MAX + FLASH_FILE_EXTENDED_LENGTH_SIZE <= STORE_MAX_VALUE, "inline files must fit a value");
_Static_assert(CHUNK <= STORE_MAX_VALUE, "chunks must fit a value");

extern uintptr_t end_flash, last_base;
extern uintptr_t end_data_pool, start_data_pool, end_rom_pool, start_rom_pool;
extern uint32_t num_files;

typedef struct {
    struct nvs_fs fs;
    struct nvs_flash flash;
    uintptr_t base;
    uint16_t sectors;
} store_t;

static store_t data_store, perm_store;
/* NVS lookup caches: the data store holds hundreds of files, the persistent
   store a handful */
static uint32_t data_cache[512], perm_cache[32];
static bool stores_ready = false;
static uintptr_t stores_end;

typedef struct {
    uint16_t id;
    bool perm;
    bool del;
    const uint8_t *buf;
    uint32_t len;
} pending_t;

static uint8_t arena[ARENA_SIZE];
static size_t arena_used;
static pending_t pending[MAX_PENDING];
static size_t n_pending;

static uint8_t chunk_used[CHUNK_IDS / 8];

static uint8_t *big_copy = NULL;    /* file_get_data() of a chunked file */
static uint16_t big_copy_fid = 0;

/* flash access for the stores, through the counter log's raw path: on RP2350,
   core 0 runs every program and erase while core 1 waits */

static int store_read(void *ctx, uint32_t off, void *data, size_t len) {
    store_t *s = ctx;
    memcpy(data, low_flash_raw_ptr(s->base + off), len);
    return 0;
}

static int store_program(void *ctx, uint32_t off, const void *data, size_t len) {
    static uint8_t page[FS_PAGE];
    store_t *s = ctx;
    const uint8_t *d = data;
    uintptr_t addr = s->base + off;
    while (len > 0) {
        uintptr_t page_addr = addr & ~(uintptr_t)(FS_PAGE - 1);
        size_t in_page = addr - page_addr;
        size_t n = MIN(len, FS_PAGE - in_page);
        memset(page, 0xFF, sizeof(page));
        memcpy(page + in_page, d, n);
        if (low_flash_raw(page_addr, page) != PICOKEYS_OK) {
            return -EIO;
        }
        addr += n;
        d += n;
        len -= n;
    }
    return 0;
}

/* Set by every store erase: garbage collection moved values, so the index's
   pointers into flash must be looked up again (index_repoint). */
static bool store_moved = false;

static int store_erase(void *ctx, uint32_t off, size_t len) {
    store_t *s = ctx;
    store_moved = true;
    for (size_t o = 0; o < len; o += FS_ERASE) {
        if (low_flash_raw(s->base + off + o, NULL) != PICOKEYS_OK) {
            return -EIO;
        }
    }
    return 0;
}

static bool region_blank(uintptr_t addr, size_t len) {
    const uint8_t *p = low_flash_raw_ptr(addr);
    for (size_t i = 0; i < len; i++) {
        if (p[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

/* Erases the store's sectors that hold anything; most are blank. */
static int store_wipe(store_t *s) {
    for (uint32_t o = 0; o < (uint32_t)s->sectors * STORE_SECTOR; o += FS_ERASE) {
        if (!region_blank(s->base + o, FS_ERASE) && low_flash_raw(s->base + o, NULL) != PICOKEYS_OK) {
            return PICOKEYS_ERR_MEMORY_FATAL;
        }
    }
    return PICOKEYS_OK;
}

static int store_mount(store_t *s, uintptr_t base, uint16_t sectors) {
    memset(s, 0, sizeof(*s));
    s->fs.lookup_cache = s == &perm_store ? perm_cache : data_cache;
    s->fs.lookup_cache_size = s == &perm_store ? (uint16_t)(sizeof(perm_cache) / sizeof(perm_cache[0])) : (uint16_t)(sizeof(data_cache) / sizeof(data_cache[0]));
    s->base = base;
    s->sectors = sectors;
    s->flash.read = store_read;
    s->flash.write = store_program;
    s->flash.erase = store_erase;
    s->flash.ctx = s;
    s->flash.erase_value = 0xFF;
    s->flash.write_block_size = 1;
    s->fs.sector_size = STORE_SECTOR;
    s->fs.sector_count = sectors;
    s->fs.flash = &s->flash;
    return nvs_mount(&s->fs);
}

static store_t *store_for(const file_t *file) {
    return (file_get_type(file) & FILE_PERSISTENT) == FILE_PERSISTENT ? &perm_store : &data_store;
}

static const uint8_t *store_ptr(store_t *s, uint16_t id, size_t *len) {
    uint32_t off;
    size_t l;
    if (nvs_locate(&s->fs, id, &off, &l) != 0) {
        return NULL;
    }
    if (len) {
        *len = l;
    }
    return low_flash_raw_ptr(s->base + off);
}

static bool in_arena(const uint8_t *p);

static void repoint(file_t *file) {
    if (!file->data || in_arena(file->data) || (file_get_type(file) & FILE_DATA_FUNC) != 0) {
        return;
    }
    file->data = (uint8_t *)store_ptr(store_for(file), file->fid, NULL);
    if (!file->data) {
        printf("ERROR: file %x lost its value\n", file->fid);
    }
}

static bool repoint_cb(file_t *file, void *ctx) {
    (void)ctx;
    repoint(file);
    return true;
}

/* After garbage collection: every committed file's value may have moved. Files
   keep their file_t, so pointers to those stay valid; only data changes. */
static void index_repoint(void) {
    if (!store_moved) {
        return;
    }
    store_moved = false;
    for (file_entry_t *f = file_entries; f != file_last; f++) {
        if ((f->type & FILE_DATA_FLASH) == FILE_DATA_FLASH) {
            repoint(&f->file);
        }
    }
#ifndef ENABLE_EMULATION
    repoint(ef_phy);
#endif
    file_for_each_dynamic(repoint_cb, NULL);
}

/* value headers */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t value_size(const uint8_t *v) {
    uint16_t l = rd16(v);
    return l == FLASH_FILE_EXTENDED_LENGTH ? rd32(v + 2) : l;
}

static bool chunked(uint32_t size) {
    return size > INLINE_MAX;
}

static uint16_t head_count(const uint8_t *head) {
    return rd16(head + 6);
}

static uint16_t head_chunk(const uint8_t *head, uint32_t i) {
    return rd16(head + HEAD_FIXED + 2 * i);
}

static uint32_t chunks_for(uint32_t size) {
    return (size + CHUNK - 1) / CHUNK;
}

/* chunk IDs */

static bool chunk_is_used(uint16_t id) {
    uint32_t i = id - FILE_STORE_CHUNK_FIRST;
    return (chunk_used[i / 8] >> (i % 8)) & 1;
}

static void chunk_mark(uint16_t id, bool used) {
    uint32_t i = id - FILE_STORE_CHUNK_FIRST;
    if (used) {
        chunk_used[i / 8] |= (uint8_t)(1u << (i % 8));
    }
    else {
        chunk_used[i / 8] &= (uint8_t)~(1u << (i % 8));
    }
}

static bool is_chunk_id(uint16_t id) {
    return id >= FILE_STORE_CHUNK_FIRST && id <= FILE_STORE_CHUNK_LAST;
}

static int chunk_alloc(uint16_t *id) {
    for (uint16_t c = FILE_STORE_CHUNK_FIRST; c <= FILE_STORE_CHUNK_LAST; c++) {
        if (!chunk_is_used(c)) {
            chunk_mark(c, true);
            *id = c;
            return PICOKEYS_OK;
        }
    }
    return PICOKEYS_ERR_NO_MEMORY;
}

/* index */

static uint8_t chunk_seen[CHUNK_IDS / 8];

static int index_add(void *ctx, uint16_t id, uint32_t off, size_t len) {
    store_t *s = ctx;
    if (is_chunk_id(id)) {
        uint32_t i = id - FILE_STORE_CHUNK_FIRST;
        chunk_seen[i / 8] |= (uint8_t)(1u << (i % 8));
        return 0;
    }
    if (id >= FILE_STORE_RESERVED_FID || len < 2) {
        return 0;
    }
    file_t *file = file_search_by_fid(id, NULL, SPECIFY_EF);
    if (!file) {
        file = file_new(id);
    }
    if (!file) {
        printf("ERROR: no room in the file index for %x\n", id);
        return 0;
    }
    if ((file_get_type(file) & FILE_DATA_FUNC) != 0) {
        /* Generated files keep their handler in data; never replace it with flash bytes. */
        return 0;
    }
    const uint8_t *v = low_flash_raw_ptr(s->base + off);
    uint32_t size = value_size(v);
    if (chunked(size)) {
        if (len < HEAD_FIXED || len < HEAD_FIXED + 2u * head_count(v) || head_count(v) != chunks_for(size)) {
            printf("ERROR: damaged head for file %x\n", id);
            return 0;
        }
        for (uint32_t i = 0; i < head_count(v); i++) {
            if (is_chunk_id(head_chunk(v, i))) {
                chunk_mark(head_chunk(v, i), true);
            }
        }
    }
    file->data = (uint8_t *)v;
    num_files++;
    return 0;
}

static void index_rebuild(void) {
    file_index_reset();
    num_files = 0;
    memset(chunk_used, 0, sizeof(chunk_used));
    memset(chunk_seen, 0, sizeof(chunk_seen));
    if (big_copy) {
        free(big_copy);
        big_copy = NULL;
    }
    if (!stores_ready) {
        return;
    }
    /* the arena is empty whenever the index is rebuilt: borrow it */
    uint8_t *work = n_pending == 0 ? arena : malloc(NVS_FOREACH_WORK);
    if (!work) {
        printf("ERROR: no memory to list the store\n");
        return;
    }
    nvs_foreach(&perm_store.fs, index_add, &perm_store, work);
    nvs_foreach(&data_store.fs, index_add, &data_store, work);
    if (work != arena) {
        free(work);
    }
    /* chunks no head refers to: written before a power cut, never committed */
    for (uint32_t i = 0; i < CHUNK_IDS; i++) {
        bool seen = (chunk_seen[i / 8] >> (i % 8)) & 1;
        if (seen && !chunk_is_used((uint16_t)(FILE_STORE_CHUNK_FIRST + i))) {
            nvs_delete(&data_store.fs, (uint16_t)(FILE_STORE_CHUNK_FIRST + i));
            nvs_delete(&perm_store.fs, (uint16_t)(FILE_STORE_CHUNK_FIRST + i));
        }
    }
    index_repoint();
}

/* pending writes */

static pending_t *pending_get(uint16_t id) {
    for (size_t i = 0; i < n_pending; i++) {
        if (pending[i].id == id) {
            return &pending[i];
        }
    }
    return NULL;
}

static int store_batch(store_t *s, const struct nvs_batch_entry *e, size_t n) {
    if (n == 0) {
        return 0;
    }
    int r = nvs_batch_write(&s->fs, e, n);
    if (r == -ENOSPC) {
        /* Larger than a store sector: written one at a time, heads before the
           deletion of the chunks they replaced, so a cut keeps a whole file. */
        printf("WARNING: %u entries committed one at a time\n", (unsigned)n);
        r = 0;
        for (size_t i = 0; i < n && r == 0; i++) {
            ssize_t w = nvs_write(&s->fs, e[i].id, e[i].data, e[i].len);
            r = w < 0 ? (int)w : 0;
        }
    }
    return r;
}

/* The pending entries of one store, writes first, then deletes. */
static size_t batch_of(bool perm, struct nvs_batch_entry *e) {
    size_t n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < n_pending; i++) {
            if (pending[i].perm != perm || pending[i].del != (pass == 1)) {
                continue;
            }
            e[n].id = pending[i].id;
            e[n].data = pending[i].del ? NULL : pending[i].buf;
            e[n].len = pending[i].del ? 0 : pending[i].len;
            n++;
        }
    }
    return n;
}

static int store_flush(void) {
    static struct nvs_batch_entry e[MAX_PENDING];

    if (n_pending == 0) {
        return PICOKEYS_OK;
    }
    int r = store_batch(&data_store, e, batch_of(false, e));
    if (r == 0) {
        r = store_batch(&perm_store, e, batch_of(true, e));
    }
    if (r == 0) {
        index_repoint();
        for (size_t i = 0; i < n_pending; i++) {
            if (is_chunk_id(pending[i].id)) {
                if (pending[i].del) {
                    chunk_mark(pending[i].id, false);
                }
                continue;
            }
            file_t *file = file_search(pending[i].id);
            if (!file || pending[i].del || (file_get_type(file) & FILE_DATA_FUNC) != 0) {
                continue;
            }
            file->data = (uint8_t *)store_ptr(pending[i].perm ? &perm_store : &data_store, pending[i].id, NULL);
            if (!file->data) {
                printf("ERROR: file %x committed but not found in the store\n", pending[i].id);
            }
        }
    }
    n_pending = 0;
    arena_used = 0;
    if (r != 0) {
        printf("ERROR: file store commit failed (%d)\n", r);
        /* RAM must not claim what flash does not hold. */
        index_rebuild();
        return PICOKEYS_ERR_MEMORY_FATAL;
    }
    return PICOKEYS_OK;
}

/* Makes room for `entries` more pending entries and `bytes` of arena, so the
   parts of one change are always committed together. A command that writes
   more than that is committed in parts, as the old file system did. */
static int pending_room(size_t entries, size_t bytes) {
    if (n_pending + entries > MAX_PENDING || bytes > ARENA_SIZE - arena_used) {
        if (store_flush() != PICOKEYS_OK) {
            return PICOKEYS_ERR_MEMORY_FATAL;
        }
        if (entries > MAX_PENDING || bytes > ARENA_SIZE) {
            return PICOKEYS_ERR_NO_MEMORY;
        }
    }
    return PICOKEYS_OK;
}

static uint32_t chunk_count_of(const uint8_t *v) {
    return v && chunked(value_size(v)) ? head_count(v) : 0;
}

static int pending_add(uint16_t id, bool perm, const uint8_t *buf, uint32_t len, bool del) {
    pending_t *p = pending_get(id);
    if (!p) {
        if (n_pending == MAX_PENDING) {
            return PICOKEYS_ERR_NO_MEMORY;
        }
        p = &pending[n_pending++];
        p->id = id;
    }
    p->perm = perm;
    p->del = del;
    p->buf = buf;
    p->len = len;
    return PICOKEYS_OK;
}

static bool in_arena(const uint8_t *p) {
    return p >= arena && p < arena + ARENA_SIZE;
}

static uint8_t *arena_alloc(size_t len) {
    size_t rounded = (len + 3u) & ~(size_t)3u;
    if (rounded > ARENA_SIZE - arena_used) {
        return NULL;
    }
    uint8_t *p = arena + arena_used;
    arena_used += rounded;
    return p;
}

static void big_copy_drop(uint16_t fid) {
    if (big_copy && big_copy_fid == fid) {
        free(big_copy);
        big_copy = NULL;
    }
}

/* Queues the deletion of every chunk of a chunked value. */
static int drop_chunks(const uint8_t *v, bool perm) {
    if (!v || !chunked(value_size(v))) {
        return PICOKEYS_OK;
    }
    for (uint32_t i = 0; i < head_count(v); i++) {
        int r = pending_add(head_chunk(v, i), perm, NULL, 0, true);
        if (r != PICOKEYS_OK) {
            return r;
        }
    }
    return PICOKEYS_OK;
}

static int write_inline(file_t *file, const_byte_array_t data, uint32_t offset, bool partial, uint32_t new_size) {
    store_t *s = store_for(file);
    bool perm = s == &perm_store;
    size_t header = new_size >= FLASH_FILE_EXTENDED_LENGTH ? FLASH_FILE_EXTENDED_LENGTH_SIZE : FLASH_FILE_LEGACY_LENGTH_SIZE;
    size_t total = header + new_size;

    int r = pending_room(1 + (partial ? 0 : chunk_count_of(file->data)), total);
    if (r != PICOKEYS_OK) {
        return r;
    }
    uint8_t *buf = arena_alloc(total);
    if (!buf) {
        return PICOKEYS_ERR_NO_MEMORY;
    }
    if (header == FLASH_FILE_EXTENDED_LENGTH_SIZE) {
        wr16(buf, FLASH_FILE_EXTENDED_LENGTH);
        wr32(buf + 2, new_size);
    }
    else {
        wr16(buf, (uint16_t)new_size);
    }
    uint32_t old_size = file_get_size(file);
    if (partial && old_size > 0 && file_read_at(file, 0, BYTE_ARRAY(buf + header, old_size)) != PICOKEYS_OK) {
        return PICOKEYS_ERR_MEMORY_FATAL;
    }
    if (data.len > 0) {
        memcpy(buf + header + offset, data.data, data.len);
    }
    /* a file that shrinks below INLINE_MAX leaves its chunks */
    if (!partial && drop_chunks(file->data, perm) != PICOKEYS_OK) {
        return PICOKEYS_ERR_MEMORY_FATAL;
    }
    r = pending_add(file->fid, perm, buf, (uint32_t)total, false);
    if (r != PICOKEYS_OK) {
        return r;
    }
    if (!file->data) {
        num_files++;
    }
    file->data = buf;
    return PICOKEYS_OK;
}

static int write_chunked(file_t *file, const_byte_array_t data, uint32_t offset, bool partial, uint32_t new_size) {
    store_t *s = store_for(file);
    bool perm = s == &perm_store;
    uint32_t old_size = file_get_size(file);
    const uint8_t *old = file->data;
    bool old_chunked = old && chunked(old_size);
    uint32_t n_old = old_chunked ? head_count(old) : 0;
    uint32_t n_new = chunks_for(new_size);
    size_t head_len = HEAD_FIXED + 2u * n_new;
    uint32_t end = offset + (uint32_t)data.len;

    if (n_new > CHUNK_IDS || head_len > STORE_MAX_VALUE) {
        return PICOKEYS_ERR_NO_MEMORY;
    }
    /* the new head and one pending delete per old chunk */
    int rr = pending_room(n_old + 1, head_len);
    if (rr != PICOKEYS_OK) {
        return rr;
    }
    old = file->data;
    uint8_t *head = arena_alloc(head_len);
    uint8_t *chunk_buf = malloc(CHUNK);
    if (!head || !chunk_buf) {
        free(chunk_buf);
        return PICOKEYS_ERR_NO_MEMORY;
    }
    wr16(head, FLASH_FILE_EXTENDED_LENGTH);
    wr32(head + 2, new_size);
    wr16(head + 6, (uint16_t)n_new);
    int r = PICOKEYS_OK;

    for (uint32_t i = 0; i < n_new && r == PICOKEYS_OK; i++) {
        uint32_t c_start = i * CHUNK;
        uint32_t c_len = MIN(CHUNK, new_size - c_start);
        bool touched = !partial || !old_chunked || i >= n_old || (end > c_start && offset < c_start + c_len) ||
                       (c_start + c_len > old_size);
        if (!touched) {
            wr16(head + HEAD_FIXED + 2 * i, head_chunk(old, i));
            continue;
        }
        /* the chunk's new bytes: what the file had there, then the write */
        if (partial) {
            uint32_t keep = old_size > c_start ? MIN(c_len, old_size - c_start) : 0;
            if (keep && file_read_at(file, c_start, BYTE_ARRAY(chunk_buf, keep)) != PICOKEYS_OK) {
                r = PICOKEYS_ERR_MEMORY_FATAL;
                break;
            }
        }
        uint32_t w_from = MAX(offset, c_start), w_to = MIN(end, c_start + c_len);
        if (w_to > w_from) {
            memcpy(chunk_buf + (w_from - c_start), data.data + (w_from - offset), w_to - w_from);
        }
        uint16_t id = 0;
        if (chunk_alloc(&id) != PICOKEYS_OK) {
            printf("ERROR: no chunk IDs left\n");
            r = PICOKEYS_ERR_NO_MEMORY;
            break;
        }
        ssize_t w = nvs_write(&s->fs, id, chunk_buf, c_len);
        index_repoint();
        old = file->data;
        if (w < 0) {
            printf("ERROR: writing part %u of file %x failed (%d)\n", (unsigned)i, file->fid, (int)w);
            chunk_mark(id, false);
            r = PICOKEYS_ERR_MEMORY_FATAL;
            break;
        }
        wr16(head + HEAD_FIXED + 2 * i, id);
    }
    free(chunk_buf);
    if (r != PICOKEYS_OK) {
        /* nothing queued yet: the old file stays whole, and the chunks
           written so far are unreferenced (deleted at the next boot) */
        return r;
    }
    /* every old chunk the new head no longer uses */
    for (uint32_t i = 0; i < n_old; i++) {
        if (i >= n_new || head_chunk(head, i) != head_chunk(old, i)) {
            pending_add(head_chunk(old, i), perm, NULL, 0, true);
        }
    }
    r = pending_add(file->fid, perm, head, (uint32_t)head_len, false);
    if (r != PICOKEYS_OK) {
        return r;
    }
    if (!file->data) {
        num_files++;
    }
    file->data = head;
    return PICOKEYS_OK;
}

static int store_write(file_t *file, const_byte_array_t data, uint32_t offset, bool partial) {
    if (!file || (!data.data && data.len > 0) || data.len > UINT32_MAX) {
        return PICOKEYS_ERR_NULL_PARAM;
    }
    if (!stores_ready) {
        return PICOKEYS_ERR_MEMORY_FATAL;
    }
    uint32_t len = (uint32_t)data.len;
    uint32_t old_size = file_get_size(file);
    if (offset > UINT32_MAX - len || (partial && offset > old_size)) {
        return PICOKEYS_ERR_NO_MEMORY;
    }
    uint32_t new_size = partial ? MAX(old_size, offset + len) : len;
    big_copy_drop(file->fid);
    if (chunked(new_size)) {
        return write_chunked(file, data, partial ? offset : 0, partial, new_size);
    }
    return write_inline(file, data, partial ? offset : 0, partial, new_size);
}

int flash_write_data_to_file(file_t *file, const_byte_array_t data) {
    return store_write(file, data, 0, false);
}

int flash_write_data_to_file_offset(file_t *file, const_byte_array_t data, uint32_t offset) {
    return store_write(file, data, offset, true);
}

int flash_clear_file(file_t *file) {
    if (file == NULL || file->data == NULL) {
        return PICOKEYS_OK;
    }
    if (!stores_ready) {
        return PICOKEYS_ERR_MEMORY_FATAL;
    }
    bool perm = store_for(file) == &perm_store;
    big_copy_drop(file->fid);
    int r = pending_room(1 + chunk_count_of(file->data), 0);
    if (r != PICOKEYS_OK || (r = drop_chunks(file->data, perm)) != PICOKEYS_OK) {
        return r;
    }
    r = pending_add(file->fid, perm, NULL, 0, true);
    if (r != PICOKEYS_OK) {
        return r;
    }
    file->data = NULL;
    if (num_files > 0) {
        num_files--;
    }
    return PICOKEYS_OK;
}

void flash_commit(void) {
    store_flush();
}

bool flash_commit_sync(uint32_t timeout_ms) {
    (void)timeout_ms;
    return store_flush() == PICOKEYS_OK;
}

/* reading */

uint8_t *file_read(const uint8_t *addr) {
    return (uint8_t *)addr;
}

uint16_t file_read_uint16(const uint8_t *addr) {
    return rd16(addr);
}

uint32_t file_read_uint32(const uint8_t *addr) {
    return rd32(addr);
}

uint32_t file_get_size(const file_t *tf) {
    if (!tf || !tf->data) {
        return 0;
    }
    return value_size(tf->data);
}

int file_read_at(const file_t *tf, uint32_t offset, byte_array_t data) {
    if (!tf || !tf->data || (!data.data && data.len > 0)) {
        return PICOKEYS_ERR_NULL_PARAM;
    }
    uint32_t size = file_get_size(tf);
    if (offset > size || data.len > size - offset) {
        return PICOKEYS_ERR_NULL_PARAM;
    }
    if (data.len == 0) {
        return PICOKEYS_OK;
    }
    if (!chunked(size)) {
        size_t header = rd16(tf->data) == FLASH_FILE_EXTENDED_LENGTH ? FLASH_FILE_EXTENDED_LENGTH_SIZE : FLASH_FILE_LEGACY_LENGTH_SIZE;
        memcpy(data.data, tf->data + header + offset, data.len);
        return PICOKEYS_OK;
    }
    store_t *s = store_for(tf);
    uint8_t *out = data.data;
    size_t left = data.len;
    while (left > 0) {
        uint32_t i = offset / CHUNK, within = offset % CHUNK;
        size_t clen;
        const uint8_t *c = store_ptr(s, head_chunk(tf->data, i), &clen);
        if (!c || within >= clen) {
            return PICOKEYS_ERR_MEMORY_FATAL;
        }
        size_t n = MIN(left, clen - within);
        memcpy(out, c + within, n);
        out += n;
        offset += (uint32_t)n;
        left -= n;
    }
    return PICOKEYS_OK;
}

uint8_t *file_get_data(const file_t *tf) {
    if (!tf || !tf->data) {
        return NULL;
    }
    uint32_t size = value_size(tf->data);
    if (!chunked(size)) {
        return tf->data + (rd16(tf->data) == FLASH_FILE_EXTENDED_LENGTH ? FLASH_FILE_EXTENDED_LENGTH_SIZE : FLASH_FILE_LEGACY_LENGTH_SIZE);
    }
    /* A chunked file is not contiguous: hand out a RAM copy, valid until the
       next call for another large file or a write to this one. READ BINARY
       uses file_read_at() and never gets here. */
    if (big_copy && big_copy_fid == tf->fid) {
        return big_copy;
    }
    free(big_copy);
    big_copy = malloc(size);
    if (!big_copy) {
        return NULL;
    }
    big_copy_fid = tf->fid;
    if (file_read_at(tf, 0, BYTE_ARRAY(big_copy, size)) != PICOKEYS_OK) {
        free(big_copy);
        big_copy = NULL;
    }
    return big_copy;
}

uint8_t file_read_uint8_offset(const file_t *ef, const uint16_t offset) {
    uint8_t value = 0;
    file_read_at(ef, offset, BYTE_ARRAY(&value, sizeof(value)));
    return value;
}

uint8_t file_read_uint8(const file_t *ef) {
    return file_read_uint8_offset(ef, 0);
}

/* writing a whole file straight to a store, for migration */
static int store_put_direct(store_t *s, uint16_t fid, uint32_t size, int (*reader)(void *ctx, uint32_t off, uint8_t *out, uint32_t len), void *ctx) {
    if (!chunked(size)) {
        size_t header = size >= FLASH_FILE_EXTENDED_LENGTH ? FLASH_FILE_EXTENDED_LENGTH_SIZE : FLASH_FILE_LEGACY_LENGTH_SIZE;
        uint8_t *buf = malloc(header + size);
        if (!buf) {
            return PICOKEYS_ERR_NO_MEMORY;
        }
        wr16(buf, header == FLASH_FILE_EXTENDED_LENGTH_SIZE ? FLASH_FILE_EXTENDED_LENGTH : (uint16_t)size);
        if (header == FLASH_FILE_EXTENDED_LENGTH_SIZE) {
            wr32(buf + 2, size);
        }
        int r = reader(ctx, 0, buf + header, size);
        if (r == PICOKEYS_OK && nvs_write(&s->fs, fid, buf, header + size) < 0) {
            r = PICOKEYS_ERR_MEMORY_FATAL;
        }
        free(buf);
        return r;
    }
    uint32_t n = chunks_for(size);
    size_t head_len = HEAD_FIXED + 2u * n;
    uint8_t *head = malloc(head_len);
    if (!head) {
        return PICOKEYS_ERR_NO_MEMORY;
    }
    wr16(head, FLASH_FILE_EXTENDED_LENGTH);
    wr32(head + 2, size);
    wr16(head + 6, (uint16_t)n);
    uint8_t *chunk_buf = malloc(CHUNK);
    int r = chunk_buf ? PICOKEYS_OK : PICOKEYS_ERR_NO_MEMORY;
    for (uint32_t i = 0; i < n && r == PICOKEYS_OK; i++) {
        uint32_t c_len = MIN(CHUNK, size - i * CHUNK);
        uint16_t id = 0;
        r = chunk_alloc(&id);
        if (r == PICOKEYS_OK) {
            r = reader(ctx, i * CHUNK, chunk_buf, c_len);
        }
        if (r == PICOKEYS_OK && nvs_write(&s->fs, id, chunk_buf, c_len) < 0) {
            r = PICOKEYS_ERR_MEMORY_FATAL;
        }
        wr16(head + HEAD_FIXED + 2 * i, id);
    }
    free(chunk_buf);
    if (r == PICOKEYS_OK && nvs_write(&s->fs, fid, head, head_len) < 0) {
        r = PICOKEYS_ERR_MEMORY_FATAL;
    }
    free(head);
    return r;
}

/* the old file system, read once to migrate it */

typedef int (*legacy_cb)(uint16_t fid, uintptr_t payload, uint32_t size, bool persistent);

/* Walks the old record chain, as the old scan did, checking every link. */
static int legacy_walk(bool persistent, legacy_cb cb, bool *corrupted) {
    uintptr_t endp = persistent ? end_rom_pool : end_data_pool, startp = persistent ? start_rom_pool : start_data_pool;
    if (!persistent) {
        last_base = endp;
    }
    for (uintptr_t base = flash_read_uintptr(endp); base != 0; base = flash_read_uintptr(base)) {
        size_t minimum_header = 2 * sizeof(uintptr_t) + 2 * sizeof(uint16_t);
        if (base < startp || base > endp || endp - base < minimum_header) {
            *corrupted = true;
            return 0;
        }
        uintptr_t next_addr = flash_read_uintptr(base);
        uintptr_t prev_addr = flash_read_uintptr(base + sizeof(uintptr_t));
        uint16_t fid = flash_read_uint16(base + 2 * sizeof(uintptr_t));
        uintptr_t length_addr = base + 2 * sizeof(uintptr_t) + sizeof(uint16_t);
        uint16_t stored_length = flash_read_uint16(length_addr);
        uint32_t length = stored_length == FLASH_FILE_EXTENDED_LENGTH ? flash_read_uint32(length_addr + sizeof(uint16_t)) : stored_length;
        size_t length_size = stored_length == FLASH_FILE_EXTENDED_LENGTH ? FLASH_FILE_EXTENDED_LENGTH_SIZE : FLASH_FILE_LEGACY_LENGTH_SIZE;
        size_t record_header = 2 * sizeof(uintptr_t) + sizeof(uint16_t) + length_size;
        if (fid == 0x0000 || fid == 0xFFFF || record_header > endp - base || length > endp - base - record_header ||
            (next_addr != 0 && (next_addr < startp || next_addr >= base)) || (prev_addr != endp && (prev_addr <= base || prev_addr > endp))) {
            *corrupted = true;
            return 0;
        }
        if (!persistent && base < last_base) {
            last_base = base;
        }
        if (cb) {
            int r = cb(fid, length_addr + length_size, length, persistent);
            if (r) {
                return r;
            }
        }
        if (next_addr == 0) {
            break;
        }
    }
    return 0;
}

static int legacy_reader(void *ctx, uint32_t off, uint8_t *out, uint32_t len) {
    if (len == 0) {
        return PICOKEYS_OK;
    }
    return flash_read_block(*(uintptr_t *)ctx + off, BYTE_ARRAY(out, len));
}

static int legacy_copy(uint16_t fid, uintptr_t payload, uint32_t size, bool persistent) {
    if (size == 0 || FILE_STORE_FID_RESERVED(fid)) {
        return 0;
    }
    return store_put_direct(persistent ? &perm_store : &data_store, fid, size, legacy_reader, &payload);
}

static bool legacy_present(void) {
    uint32_t r1 = (uint32_t)flash_read_uintptr(end_rom_pool);
    uint32_t r2 = (uint32_t)flash_read_uintptr(end_rom_pool + sizeof(uintptr_t));
    return !((r1 == 0xffffffff || r1 == 0xefefefef) && (r2 == 0xffffffff || r2 == 0xefefefef));
}

/* Copies every old file into the stores. Resumable: the stores are emptied
   first, and the format marker, written last, says the copy is complete. */
static int legacy_migrate_open(void);

static int legacy_migrate(void) {
    /* the old reader and its journal replay go through the old cache */
    if (low_flash_cache_open() != PICOKEYS_OK) {
        return PICOKEYS_ERR_NO_MEMORY;
    }
    int r = legacy_migrate_open();
    low_flash_cache_close();
    return r;
}

static int legacy_migrate_open(void) {
    bool corrupted = false;
    legacy_walk(false, NULL, &corrupted);
    /* finish a commit the old firmware was in the middle of */
    if (low_flash_recover_journal(corrupted) == PICOKEYS_OK) {
        printf("INFO: flash redo restored\n");
    }
    corrupted = false;
    if (store_wipe(&data_store) != PICOKEYS_OK || store_wipe(&perm_store) != PICOKEYS_OK ||
        store_mount(&data_store, data_store.base, data_store.sectors) != 0 ||
        store_mount(&perm_store, perm_store.base, perm_store.sectors) != 0) {
        return PICOKEYS_ERR_MEMORY_FATAL;
    }
    memset(chunk_used, 0, sizeof(chunk_used));
    int r = legacy_walk(true, legacy_copy, &corrupted);
    if (r == 0) {
        r = legacy_walk(false, legacy_copy, &corrupted);
    }
    if (r != 0) {
        return r;
    }
    if (corrupted) {
        printf("WARNING: old file system damaged; copied what could be read\n");
    }
    printf("INFO: files moved to the new store\n");
    return PICOKEYS_OK;
}

/* Everything above the stores belonged to the old file system. */
static void legacy_erase(void) {
    for (uintptr_t a = stores_end; a + FS_ERASE <= end_flash; a += FS_ERASE) {
        if (!region_blank(a, FS_ERASE)) {
            low_flash_raw(a, NULL);
        }
    }
}

static int stores_open(void) {
    uintptr_t base = (flash_data_area_start() + 2 * FS_ERASE + FS_ERASE - 1) & ~(uintptr_t)(FS_ERASE - 1);
    uint32_t avail = end_flash > base ? (uint32_t)(end_flash - base) : 0;
    if (avail / 2 / STORE_SECTOR < STORE_PERM_SECTORS + 4) {
        printf("ERROR: data area too small for the file store\n");
        return PICOKEYS_ERR_MEMORY_FATAL;
    }
    /* at most half the area, so an old file system above can be copied in */
    uint32_t data_sectors = MIN(FILE_STORE_DATA_SECTORS, avail / 2 / STORE_SECTOR - STORE_PERM_SECTORS);
    uintptr_t perm_base = base + data_sectors * STORE_SECTOR;
    stores_end = perm_base + STORE_PERM_SECTORS * STORE_SECTOR;

    int rd = store_mount(&data_store, base, (uint16_t)data_sectors);
    int rp = store_mount(&perm_store, perm_base, STORE_PERM_SECTORS);
    uint8_t marker[sizeof(store_format)];
    bool formatted = rp == 0 && nvs_read(&perm_store.fs, STORE_FORMAT_ID, marker, sizeof(marker)) == (ssize_t)sizeof(marker) &&
                     memcmp(marker, store_format, sizeof(marker)) == 0;
    if (formatted) {
        if (rd != 0) {
            printf("ERROR: file store does not mount (%d)\n", rd);
            return PICOKEYS_ERR_MEMORY_FATAL;
        }
    }
    else {
        /* Not formatted: a new device, or one with the old file system. What
           the store regions hold is at most an unfinished earlier copy, unless
           old files reach down into them: then touch nothing. */
        bool legacy = legacy_present();
        if (legacy) {
            bool corrupted = false;
            legacy_walk(false, NULL, &corrupted);
            /* below the lowest record: the old journal sector and six redo sectors */
            uintptr_t lowest = (last_base & ~(uintptr_t)(FS_ERASE - 1)) - 7 * FS_ERASE;
            if (lowest < stores_end) {
                printf("ERROR: old files reach into the store region; not moving them\n");
                return PICOKEYS_ERR_MEMORY_FATAL;
            }
        }
        if (rd != 0 || rp != 0) {
            if (store_wipe(&data_store) != PICOKEYS_OK || store_wipe(&perm_store) != PICOKEYS_OK ||
                store_mount(&data_store, base, (uint16_t)data_sectors) != 0 || store_mount(&perm_store, perm_base, STORE_PERM_SECTORS) != 0) {
                return PICOKEYS_ERR_MEMORY_FATAL;
            }
        }
        if (legacy) {
            int r = legacy_migrate();
            if (r != PICOKEYS_OK) {
                printf("ERROR: moving files to the new store failed (%d); old files left as they were\n", r);
                return r;
            }
        }
        if (nvs_write(&perm_store.fs, STORE_FORMAT_ID, store_format, sizeof(store_format)) < 0) {
            return PICOKEYS_ERR_MEMORY_FATAL;
        }
    }
    /* after the marker: drop the old copies (also finishes an interrupted erase) */
    legacy_erase();
    stores_ready = true;
    return PICOKEYS_OK;
}

void file_initialize_flash(bool hard) {
    if (hard && stores_ready) {
        size_t keep = 0;
        for (size_t i = 0; i < n_pending; i++) {
            if (pending[i].perm) {
                pending[keep++] = pending[i];
            }
        }
        n_pending = keep;
        if (store_wipe(&data_store) != PICOKEYS_OK || store_mount(&data_store, data_store.base, data_store.sectors) != 0) {
            printf("ERROR: file store wipe failed\n");
            stores_ready = false;
        }
    }
    file_index_reset();
    num_files = 0;
}

void file_scan_flash(void) {
    if (!stores_ready) {
        if (stores_open() != PICOKEYS_OK) {
            printf("ERROR: file store unavailable\n");
        }
    }
    else {
        store_flush();
    }
    index_rebuild();
    /* the store never reaches the counter log's sectors */
    last_base = end_flash;
    counter_log_boot();
}

static bool add_size(file_t *file, void *ctx) {
    *(uint32_t *)ctx += file_get_size(file);
    return true;
}

uint32_t flash_used_space(void) {
    uint32_t used = 0;
    file_for_each_dynamic(add_size, &used);
    return used;
}

uint32_t flash_total_space(void) {
    return stores_ready ? (uint32_t)(data_store.sectors - 1) * (STORE_SECTOR - 16) : 0;
}

uint32_t flash_free_space(void) {
    uint32_t total = flash_total_space(), used = flash_used_space();
    return used < total ? total - used : 0;
}

uint32_t flash_num_files(void) {
    return num_files;
}

#endif
