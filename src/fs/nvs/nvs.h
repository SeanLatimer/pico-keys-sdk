/*  NVS: non volatile storage in flash
 *
 * Copyright (c) 2018 Laczen
 * Copyright (c) 2026 Lingao Meng
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ported from Zephyr (subsys/kvss/nvs, commit ef2773dffed6ee0f2add16edc6ad6342c1901320)
 * for pico-keys-sdk. Changed from upstream: no Zephyr dependencies (flash access
 * through struct nvs_flash), batches written all-or-nothing (nvs_batch_write), the
 * sector close error is no longer ignored, and IDs 0xFFFD-0xFFFF are reserved.
 * The licence text is in LICENSE-APACHE-2.0 next to this file.
 */

#ifndef NVS_H
#define NVS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef NVS_LOOKUP_CACHE_SIZE
#define NVS_LOOKUP_CACHE_SIZE 512
#endif

/* IDs at and above this are used by the store itself. */
#define NVS_ID_RESERVED 0xFFFD

/* Flash access. Offsets are relative to the start of the store. write() may only
 * clear bits (NOR program), at any offset and length; erase() sets a whole store
 * sector to erase_value. Each returns 0 or a negative error. */
struct nvs_flash {
    int (*read)(void *ctx, uint32_t offset, void *data, size_t len);
    int (*write)(void *ctx, uint32_t offset, const void *data, size_t len);
    int (*erase)(void *ctx, uint32_t offset, size_t len);
    void *ctx;
    uint8_t erase_value;
    uint8_t write_block_size;  /* 1 for byte-programmable NOR */
};

struct nvs_fs {
    uint32_t ate_wra;          /* next ATE: high 16 bits sector, low 16 bits offset */
    uint32_t data_wra;         /* next data byte, same encoding */
    uint32_t sector_size;      /* multiple of the flash erase size, at most 64 KB */
    uint16_t sector_count;     /* at least 2 */
    bool ready;
    const struct nvs_flash *flash;
    uint32_t lookup_cache[NVS_LOOKUP_CACHE_SIZE];
};

/* One write of a batch. len 0 deletes the ID. */
struct nvs_batch_entry {
    uint16_t id;
    const void *data;
    size_t len;
};

int nvs_mount(struct nvs_fs *fs);
int nvs_clear(struct nvs_fs *fs);
ssize_t nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len);
int nvs_delete(struct nvs_fs *fs, uint16_t id);
ssize_t nvs_read(struct nvs_fs *fs, uint16_t id, void *data, size_t len);
ssize_t nvs_read_hist(struct nvs_fs *fs, uint16_t id, void *data, size_t len, uint16_t cnt);
ssize_t nvs_calc_free_space(struct nvs_fs *fs);
size_t nvs_sector_max_data_size(struct nvs_fs *fs);
int nvs_sector_use_next(struct nvs_fs *fs);

/* Writes every entry, or none of them if power is lost or a write fails part way.
 * Entries equal to what is stored are skipped. The whole batch must fit in one
 * store sector; returns -ENOSPC before writing anything if it can't. */
int nvs_batch_write(struct nvs_fs *fs, const struct nvs_batch_entry *entries, size_t count);

/* Byte offset (from the start of the store) of the stored value of id, and its length.
 * Values are contiguous; the offset is valid until the next write. -ENOENT if absent. */
int nvs_locate(struct nvs_fs *fs, uint16_t id, uint32_t *offset, size_t *len);

#endif
