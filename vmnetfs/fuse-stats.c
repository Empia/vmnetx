/*
 * vmnetfs - virtual machine network execution virtual filesystem
 *
 * Copyright (C) 2006-2012 Carnegie Mellon University
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.  A copy of the GNU General Public License
 * should have been distributed along with this program in the file
 * COPYING.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include "vmnetfs-private.h"

static char *format_u64(uint64_t val)
{
    return g_strdup_printf("%"PRIu64"\n", val);
}

static int u64_stat_open(void *dentry_ctx, struct vmnetfs_fuse_fh *fh)
{
    struct vmnetfs_stat *stat = dentry_ctx;

    if (_vmnetfs_stat_is_closed(stat)) {
        return -EACCES;
    }
    fh->data = stat;
    fh->buf = format_u64(_vmnetfs_u64_stat_get(stat, &fh->change_cookie));
    fh->length = strlen(fh->buf);
    return 0;
}

static int u32_fixed_open(void *dentry_ctx, struct vmnetfs_fuse_fh *fh)
{
    uint32_t *val = dentry_ctx;

    fh->buf = format_u64(*val);
    fh->length = strlen(fh->buf);
    return 0;
}

static int chunks_open(void *dentry_ctx, struct vmnetfs_fuse_fh *fh)
{
    struct vmnetfs_image *img = dentry_ctx;
    uint64_t len;

    if (_vmnetfs_io_image_is_closed(img)) {
        return -EACCES;
    }
    fh->data = img;
    len = (_vmnetfs_io_get_image_size(img, &fh->change_cookie) +
            img->chunk_size - 1) / img->chunk_size;
    fh->buf = format_u64(len);
    fh->length = strlen(fh->buf);
    return 0;
}

static int stat_poll(struct vmnetfs_fuse_fh *fh, struct fuse_pollhandle *ph,
        bool *readable)
{
    struct vmnetfs_stat *stat = fh->data;

    g_assert(stat != NULL);
    *readable = _vmnetfs_stat_add_poll_handle(stat, ph, fh->change_cookie);
    return 0;
}

static int image_size_poll(struct vmnetfs_fuse_fh *fh,
        struct fuse_pollhandle *ph, bool *readable)
{
    struct vmnetfs_image *img = fh->data;

    g_assert(img != NULL);
    *readable = _vmnetfs_io_image_size_add_poll_handle(img, ph,
            fh->change_cookie);
    return 0;
}

static const struct vmnetfs_fuse_ops u64_stat_ops = {
    .getattr = _vmnetfs_fuse_readonly_pseudo_file_getattr,
    .open = u64_stat_open,
    .read = _vmnetfs_fuse_buffered_file_read,
    .poll = stat_poll,
    .release = _vmnetfs_fuse_buffered_file_release,
};

static const struct vmnetfs_fuse_ops u32_fixed_ops = {
    .getattr = _vmnetfs_fuse_readonly_pseudo_file_getattr,
    .open = u32_fixed_open,
    .read = _vmnetfs_fuse_buffered_file_read,
    .release = _vmnetfs_fuse_buffered_file_release,
};

static const struct vmnetfs_fuse_ops chunks_ops = {
    .getattr = _vmnetfs_fuse_readonly_pseudo_file_getattr,
    .open = chunks_open,
    .read = _vmnetfs_fuse_buffered_file_read,
    .poll = image_size_poll,
    .release = _vmnetfs_fuse_buffered_file_release,
};

void _vmnetfs_fuse_stats_populate(struct vmnetfs_fuse_dentry *dir,
        struct vmnetfs_image *img)
{
    struct vmnetfs_fuse_dentry *stats;

    stats = _vmnetfs_fuse_add_dir(dir, "stats");

#define add_stat(n) _vmnetfs_fuse_add_file(stats, #n, &u64_stat_ops, img->n)
    add_stat(bytes_read);
    add_stat(bytes_written);
    add_stat(chunk_fetch_skips);
    add_stat(chunk_fetches);
    add_stat(chunk_dirties);
    add_stat(io_errors);
#undef add_stat

#define add_fixed32(n) _vmnetfs_fuse_add_file(stats, #n, &u32_fixed_ops, &img->n)
    add_fixed32(chunk_size);
#undef add_fixed

    _vmnetfs_fuse_add_file(stats, "chunks", &chunks_ops, img);
}
