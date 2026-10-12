/*
 * GPT partition write guard filter.
 *
 * Copyright (c) 2026 QEMU contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "block/qdict.h"
#include "qemu/cutils.h"
#include "qemu/crc32.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"

#define GPT_SECTOR_SIZE_MIN 512
#define GPT_SECTOR_SIZE_MAX 4096
#define GPT_HEADER_MIN_SIZE 92
#define GPT_MAX_ENTRIES 4096
#define GPT_MAX_ENTRY_SIZE 4096

typedef struct PartitionRange {
    uint64_t start;
    uint64_t end;
} PartitionRange;

typedef struct BDRVPartitionWriteGuardState {
    PartitionRange *ranges;
    size_t range_count;
    uint32_t sector_size;
} BDRVPartitionWriteGuardState;

static uint32_t pwg_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffff;

    while (len--) {
        crc = crc32_table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffff;
}

static bool pwg_get_partition_number(const QObject *value, int64_t *number)
{
    if (qobject_type(value) == QTYPE_QNUM) {
        return qnum_get_try_int(qobject_to(QNum, value), number);
    }

    if (qobject_type(value) == QTYPE_QSTRING) {
        const char *str = qstring_get_str(qobject_to(QString, value));
        const char *end;

        return !qemu_strtoi64(str, &end, 0, number) && !*end;
    }

    return false;
}

static int pwg_read_gpt(BlockDriverState *bs, QDict *partitions,
                        Error **errp)
{
    uint8_t header[GPT_SECTOR_SIZE_MAX];
    uint8_t *table = NULL;
    int64_t disk_bytes;
    uint32_t header_size, entry_count, entry_size, table_crc;
    uint64_t table_lba, table_bytes, table_offset, table_end_lba;
    uint64_t disk_sectors, current_lba, backup_lba;
    uint64_t first_usable_lba, last_usable_lba;
    uint32_t header_crc;
    uint64_t selected[256];
    size_t selected_count = 0;
    const QDictEntry *entry;
    BDRVPartitionWriteGuardState *s = bs->opaque;
    uint32_t sector_size = s->sector_size;
    int ret = -EINVAL;

    if (!qdict_size(partitions) ||
        qdict_size(partitions) > G_N_ELEMENTS(selected)) {
        error_setg(errp, "partitions must contain 1 to 256 GPT partition"
                   " numbers");
        return -EINVAL;
    }
    for (entry = qdict_first(partitions); entry;
         entry = qdict_next(partitions, entry)) {
        int64_t value;

        if (!pwg_get_partition_number(qdict_entry_value(entry), &value) ||
            value < 1 || value > GPT_MAX_ENTRIES) {
            error_setg(errp, "GPT partition numbers must be between 1 and %u",
                       GPT_MAX_ENTRIES);
            return -EINVAL;
        }
        for (size_t j = 0; j < selected_count; j++) {
            if (selected[j] == (uint64_t)value) {
                error_setg(errp, "Duplicate GPT partition number %" PRId64,
                           value);
                return -EINVAL;
            }
        }
        selected[selected_count++] = value;
    }

    disk_bytes = bdrv_getlength(bs->file->bs);
    if (disk_bytes < (int64_t)sector_size * 2 || disk_bytes % sector_size) {
        error_setg(errp, "Underlying disk has an unsupported size");
        return -EINVAL;
    }
    disk_sectors = disk_bytes / sector_size;
    ret = bdrv_pread(bs->file, sector_size, sector_size, header, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Cannot read the primary GPT header");
        return ret;
    }
    if (memcmp(header, "EFI PART", 8)) {
        error_setg(errp, "Underlying disk does not contain a primary GPT"
                   " header");
        return -EINVAL;
    }

    header_size = ldl_le_p(header + 12);
    header_crc = ldl_le_p(header + 16);
    if (header_size < GPT_HEADER_MIN_SIZE || header_size > sector_size) {
        error_setg(errp, "Invalid GPT header size %u", header_size);
        return -EINVAL;
    }
    stl_le_p(header + 16, 0);
    if (pwg_crc32(header, header_size) != header_crc) {
        error_setg(errp, "GPT header checksum is invalid");
        return -EINVAL;
    }

    table_lba = ldq_le_p(header + 72);
    entry_count = ldl_le_p(header + 80);
    entry_size = ldl_le_p(header + 84);
    table_crc = ldl_le_p(header + 88);
    current_lba = ldq_le_p(header + 24);
    backup_lba = ldq_le_p(header + 32);
    first_usable_lba = ldq_le_p(header + 40);
    last_usable_lba = ldq_le_p(header + 48);
    if (!entry_count || entry_count > GPT_MAX_ENTRIES ||
        entry_size < 128 || entry_size > GPT_MAX_ENTRY_SIZE ||
        (entry_size & 7) ||
        umul64_overflow(entry_count, entry_size, &table_bytes) ||
        umul64_overflow(table_lba, sector_size, &table_offset) ||
        table_offset > disk_bytes || table_bytes > disk_bytes - table_offset ||
        uadd64_overflow(table_lba,
                        (table_bytes + sector_size - 1) / sector_size,
                        &table_end_lba) ||
        current_lba != 1 || backup_lba != disk_sectors - 1 ||
        first_usable_lba < 2 || first_usable_lba > last_usable_lba ||
        last_usable_lba >= backup_lba || table_lba < 2 ||
        table_end_lba > first_usable_lba) {
        error_setg(errp, "Invalid or out-of-bounds GPT partition table");
        return -EINVAL;
    }
    table = g_malloc(table_bytes);
    ret = bdrv_pread(bs->file, table_offset, table_bytes, table, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Cannot read the GPT partition table");
        goto out;
    }
    if (pwg_crc32(table, table_bytes) != table_crc) {
        error_setg(errp, "GPT partition table checksum is invalid");
        ret = -EINVAL;
        goto out;
    }

    s->ranges = g_new0(PartitionRange, selected_count);
    s->range_count = selected_count;
    for (size_t i = 0; i < selected_count; i++) {
        uint64_t index = selected[i] - 1;
        const uint8_t *part;
        uint64_t first, last, start, end;

        if (index >= entry_count) {
            error_setg(errp, "GPT partition number %" PRIu64 " is absent",
                       selected[i]);
            ret = -EINVAL;
            goto out;
        }
        part = table + index * entry_size;
        if (!memcmp(part, (uint8_t[16]) { 0 }, 16)) {
            error_setg(errp, "GPT partition number %" PRIu64 " is unused",
                       selected[i]);
            ret = -EINVAL;
            goto out;
        }
        first = ldq_le_p(part + 32);
        last = ldq_le_p(part + 40);
        if (first > last || first < first_usable_lba ||
            last > last_usable_lba ||
            umul64_overflow(first, sector_size, &start) ||
            uadd64_overflow(last, 1, &end) ||
            umul64_overflow(end, sector_size, &end)) {
            error_setg(errp, "GPT partition number %" PRIu64
                       " has invalid bounds",
                       selected[i]);
            ret = -EINVAL;
            goto out;
        }
        s->ranges[i] = (PartitionRange) { .start = start, .end = end };
    }
    ret = 0;
out:
    g_free(table);
    return ret;
}

static int partition_write_guard_open(BlockDriverState *bs, QDict *options,
                                      int flags, Error **errp)
{
    BDRVPartitionWriteGuardState *s = bs->opaque;
    QDict *partitions = NULL;
    int ret;

    qdict_extract_subqdict(options, &partitions, "partitions.");
    if (!partitions || !qdict_size(partitions)) {
        error_setg(errp, "Missing GPT partition allow-list");
        qobject_unref(partitions);
        return -EINVAL;
    }
    ret = bdrv_open_file_child(NULL, options, "file", bs, errp);
    if (ret < 0) {
        qobject_unref(partitions);
        return ret;
    }

    GRAPH_RDLOCK_GUARD_MAINLOOP();
    s->sector_size = MAX(bs->file->bs->bl.request_alignment,
                         GPT_SECTOR_SIZE_MIN);
    if (s->sector_size > GPT_SECTOR_SIZE_MAX ||
        !is_power_of_2(s->sector_size)) {
        error_setg(errp, "Unsupported underlying logical block size %u",
                   s->sector_size);
        qobject_unref(partitions);
        return -EINVAL;
    }
    bs->supported_write_flags = BDRV_REQ_WRITE_UNCHANGED |
        (BDRV_REQ_FUA & bs->file->bs->supported_write_flags);
    bs->supported_zero_flags = BDRV_REQ_WRITE_UNCHANGED |
        ((BDRV_REQ_FUA | BDRV_REQ_MAY_UNMAP | BDRV_REQ_NO_FALLBACK) &
         bs->file->bs->supported_zero_flags);
    ret = pwg_read_gpt(bs, partitions, errp);
    qobject_unref(partitions);
    return ret;
}

static void partition_write_guard_close(BlockDriverState *bs)
{
    BDRVPartitionWriteGuardState *s = bs->opaque;
    g_clear_pointer(&s->ranges, g_free);
    s->range_count = 0;
}

static bool partition_write_guard_allows(BlockDriverState *bs,
                                         int64_t offset, int64_t bytes)
{
    BDRVPartitionWriteGuardState *s = bs->opaque;
    uint64_t end;

    if (offset < 0 || bytes <= 0 || uadd64_overflow((uint64_t)offset,
                                                    (uint64_t)bytes, &end)) {
        return false;
    }
    for (size_t i = 0; i < s->range_count; i++) {
        if ((uint64_t)offset >= s->ranges[i].start && end <= s->ranges[i].end) {
            return true;
        }
    }
    return false;
}

static int coroutine_fn GRAPH_RDLOCK
partition_write_guard_co_preadv(BlockDriverState *bs, int64_t offset,
                                int64_t bytes, QEMUIOVector *qiov,
                                size_t qiov_offset, BdrvRequestFlags flags)
{
    return bdrv_co_preadv_part(bs->file, offset, bytes, qiov, qiov_offset,
                               flags);
}

static int coroutine_fn GRAPH_RDLOCK
partition_write_guard_co_pwritev(BlockDriverState *bs, int64_t offset,
                                 int64_t bytes, QEMUIOVector *qiov,
                                 size_t qiov_offset, BdrvRequestFlags flags)
{
    if (!partition_write_guard_allows(bs, offset, bytes)) {
        return -EACCES;
    }
    return bdrv_co_pwritev_part(bs->file, offset, bytes, qiov, qiov_offset,
                                flags);
}

static int coroutine_fn GRAPH_RDLOCK
partition_write_guard_co_pwrite_zeroes(BlockDriverState *bs, int64_t offset,
                                       int64_t bytes, BdrvRequestFlags flags)
{
    if (!partition_write_guard_allows(bs, offset, bytes)) {
        return -EACCES;
    }
    return bdrv_co_pwrite_zeroes(bs->file, offset, bytes, flags);
}

static int coroutine_fn GRAPH_RDLOCK
partition_write_guard_co_pdiscard(BlockDriverState *bs, int64_t offset,
                                  int64_t bytes)
{
    if (!partition_write_guard_allows(bs, offset, bytes)) {
        return -EACCES;
    }
    return bdrv_co_pdiscard(bs->file, offset, bytes);
}

static int64_t coroutine_fn GRAPH_RDLOCK
partition_write_guard_co_getlength(BlockDriverState *bs)
{
    return bdrv_co_getlength(bs->file->bs);
}

static int coroutine_fn GRAPH_RDLOCK
partition_write_guard_co_flush(BlockDriverState *bs)
{
    return bdrv_co_flush(bs->file->bs);
}

static BlockDriver bdrv_partition_write_guard = {
    .format_name = "partition-write-guard",
    .instance_size = sizeof(BDRVPartitionWriteGuardState),
    .bdrv_open = partition_write_guard_open,
    .bdrv_close = partition_write_guard_close,
    .bdrv_child_perm = bdrv_default_perms,
    .bdrv_co_getlength = partition_write_guard_co_getlength,
    .bdrv_co_flush = partition_write_guard_co_flush,
    .bdrv_co_preadv_part = partition_write_guard_co_preadv,
    .bdrv_co_pwritev_part = partition_write_guard_co_pwritev,
    .bdrv_co_pwrite_zeroes = partition_write_guard_co_pwrite_zeroes,
    .bdrv_co_pdiscard = partition_write_guard_co_pdiscard,
    .is_filter = true,
};

static void bdrv_partition_write_guard_init(void)
{
    bdrv_register(&bdrv_partition_write_guard);
}

block_init(bdrv_partition_write_guard_init);
