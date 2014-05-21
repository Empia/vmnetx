/*
 * vmnetfs - virtual machine network execution virtual filesystem
 *
 * Copyright (C) 2006-2014 Carnegie Mellon University
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

#ifndef VMNETFS_PRIVATE_H
#define VMNETFS_PRIVATE_H

#include <sys/stat.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include "config.h"

struct vmnetfs {
    GHashTable *images;
    struct vmnetfs_fuse *fuse;
    struct vmnetfs_log *log;
    GMainLoop *glib_loop;
    char *censored_config;
};

enum fetch_mode {
    FETCH_MODE_DEMAND,
    FETCH_MODE_STREAM,
};

struct vmnetfs_image {
    char *url;
    char *username;
    char *password;
    GList *cookies;
    char *read_base;
    uint64_t fetch_offset;
    uint64_t initial_size;
    uint32_t chunk_size;
    char *etag;
    time_t last_modified;
    enum fetch_mode fetch_mode;

    /* io */
    struct connection_pool *cpool;
    struct chunk_state *chunk_state;
    struct stream_state *stream;
    struct bitmap_group *bitmaps;
    struct bitmap *accessed_map;
    struct bitmap *fetched_map;

    /* ll_pristine */
    struct bitmap *present_map;

    /* ll_modified */
    int write_fd;
    struct bitmap *modified_map;

    /* stats */
    struct vmnetfs_stream_group *io_stream;
    struct vmnetfs_stat *bytes_read;
    struct vmnetfs_stat *bytes_written;
    struct vmnetfs_stat *chunk_fetch_skips;
    struct vmnetfs_stat *chunk_fetches;
    struct vmnetfs_stat *chunk_dirties;
    struct vmnetfs_stat *io_errors;
};

struct vmnetfs_fuse {
    struct vmnetfs *fs;
    char *mountpoint;
    struct vmnetfs_fuse_dentry *root;
    struct fuse *fuse;
    struct fuse_chan *chan;
};

struct vmnetfs_fuse_fh {
    const struct vmnetfs_fuse_ops *ops;
    void *data;
    void *buf;
    uint64_t length;
    uint64_t change_cookie;
    bool blocking;
};

struct fuse_pollhandle;
struct vmnetfs_fuse_ops {
    int (*getattr)(void *dentry_ctx, struct stat *st);
    int (*truncate)(void *dentry_ctx, uint64_t length);
    int (*open)(void *dentry_ctx, struct vmnetfs_fuse_fh *fh);
    int (*read)(struct vmnetfs_fuse_fh *fh, void *buf, uint64_t start,
            uint64_t count);
    int (*write)(struct vmnetfs_fuse_fh *fh, const void *buf,
            uint64_t start, uint64_t count);
    int (*poll)(struct vmnetfs_fuse_fh *fh, struct fuse_pollhandle *ph,
            bool *readable);
    void (*release)(struct vmnetfs_fuse_fh *fh);
    bool nonseekable;
};

struct vmnetfs_cursor {
    /* All fields are read-only */
    uint64_t chunk;
    uint64_t offset;
    uint64_t length;
    uint64_t io_offset;  // offset in the entire I/O operation

    struct vmnetfs_image *img;
    uint64_t start;
    uint64_t count;
};

#define VMNETFS_CONFIG_ERROR _vmnetfs_config_error_quark()
#define VMNETFS_FUSE_ERROR _vmnetfs_fuse_error_quark()
#define VMNETFS_IO_ERROR _vmnetfs_io_error_quark()
#define VMNETFS_STREAM_ERROR _vmnetfs_stream_error_quark()
#define VMNETFS_TRANSPORT_ERROR _vmnetfs_transport_error_quark()

enum VMNetFSConfigError {
    VMNETFS_CONFIG_ERROR_INVALID_CONFIG,
    VMNETFS_CONFIG_ERROR_INVALID_SCHEMA,
};

enum VMNetFSFUSEError {
    VMNETFS_FUSE_ERROR_FAILED,
    VMNETFS_FUSE_ERROR_BAD_MOUNTPOINT,
};

enum VMNetFSIOError {
    VMNETFS_IO_ERROR_EOF,
    VMNETFS_IO_ERROR_PREMATURE_EOF,
    VMNETFS_IO_ERROR_INVALID_CACHE,
    VMNETFS_IO_ERROR_INTERRUPTED,
};

enum VMNetFSStreamError {
    VMNETFS_STREAM_ERROR_NONBLOCKING,
    VMNETFS_STREAM_ERROR_CLOSED,
};

enum VMNetFSTransportError {
    VMNETFS_TRANSPORT_ERROR_FATAL,
    VMNETFS_TRANSPORT_ERROR_NETWORK,
};

/* fuse */
struct vmnetfs_fuse *_vmnetfs_fuse_new(struct vmnetfs *fs, GError **err);
void _vmnetfs_fuse_run(struct vmnetfs_fuse *fuse);
void _vmnetfs_fuse_terminate(struct vmnetfs_fuse *fuse);
void _vmnetfs_fuse_free(struct vmnetfs_fuse *fuse);
struct vmnetfs_fuse_dentry *_vmnetfs_fuse_add_dir(
        struct vmnetfs_fuse_dentry *parent, const char *name);
void _vmnetfs_fuse_add_file(struct vmnetfs_fuse_dentry *parent,
        const char *name, const struct vmnetfs_fuse_ops *ops, void *ctx);
void _vmnetfs_fuse_image_populate(struct vmnetfs_fuse_dentry *dir,
        struct vmnetfs_image *img);
void _vmnetfs_fuse_stats_populate(struct vmnetfs_fuse_dentry *dir,
        struct vmnetfs_image *img);
void _vmnetfs_fuse_stream_populate(struct vmnetfs_fuse_dentry *dir,
        struct vmnetfs_image *img);
void _vmnetfs_fuse_stream_populate_root(struct vmnetfs_fuse_dentry *dir,
        struct vmnetfs *fs);
void _vmnetfs_fuse_misc_populate_root(struct vmnetfs_fuse_dentry *dir,
        struct vmnetfs *fs);
bool _vmnetfs_fuse_interrupted(void);
int _vmnetfs_fuse_readonly_pseudo_file_getattr(void *dentry_ctx,
        struct stat *st);
int _vmnetfs_fuse_buffered_file_read(struct vmnetfs_fuse_fh *fh, void *buf,
        uint64_t start, uint64_t count);
void _vmnetfs_fuse_buffered_file_release(struct vmnetfs_fuse_fh *fh);

/* io */
bool _vmnetfs_io_init(struct vmnetfs_image *img, GError **err);
void _vmnetfs_io_open(struct vmnetfs_image *img);
void _vmnetfs_io_close(struct vmnetfs_image *img);
bool _vmnetfs_io_image_is_closed(struct vmnetfs_image *img);
void _vmnetfs_io_destroy(struct vmnetfs_image *img);
uint64_t _vmnetfs_io_read_chunk(struct vmnetfs_image *img, void *data,
        uint64_t chunk, uint32_t offset, uint32_t length, GError **err);
uint64_t _vmnetfs_io_write_chunk(struct vmnetfs_image *img, const void *data,
        uint64_t chunk, uint32_t offset, uint32_t length, GError **err);
uint64_t _vmnetfs_io_get_image_size(struct vmnetfs_image *img,
        uint64_t *change_cookie);
bool _vmnetfs_io_set_image_size(struct vmnetfs_image *img, uint64_t size,
        GError **err);
bool _vmnetfs_io_image_size_add_poll_handle(struct vmnetfs_image *img,
        struct fuse_pollhandle *ph, uint64_t change_cookie);

/* ll_pristine */
bool _vmnetfs_ll_pristine_init(struct vmnetfs_image *img, GError **err);
void _vmnetfs_ll_pristine_destroy(struct vmnetfs_image *img);
bool _vmnetfs_ll_pristine_read_chunk(struct vmnetfs_image *img, void *data,
        uint64_t chunk, uint32_t offset, uint32_t length, GError **err);
bool _vmnetfs_ll_pristine_write_chunk(struct vmnetfs_image *img, void *data,
        uint64_t chunk, uint32_t length, GError **err);

/* ll_modified */
bool _vmnetfs_ll_modified_init(struct vmnetfs_image *img, GError **err);
void _vmnetfs_ll_modified_destroy(struct vmnetfs_image *img);
bool _vmnetfs_ll_modified_read_chunk(struct vmnetfs_image *img,
        uint64_t image_size, void *data, uint64_t chunk, uint32_t offset,
        uint32_t length, GError **err);
bool _vmnetfs_ll_modified_write_chunk(struct vmnetfs_image *img,
        uint64_t image_size, const void *data, uint64_t chunk,
        uint32_t offset, uint32_t length, GError **err);
bool _vmnetfs_ll_modified_set_size(struct vmnetfs_image *img,
        uint64_t current_size, uint64_t new_size, GError **err);

/* transport */
typedef bool (stream_fn)(void *arg, const void *buf, uint64_t count,
        GError **err);
typedef bool (should_cancel_fn)(void *arg);
bool _vmnetfs_transport_init(void);
struct connection_pool *_vmnetfs_transport_pool_new(GError **err);
void _vmnetfs_transport_pool_free(struct connection_pool *cpool);
bool _vmnetfs_transport_pool_set_cookie(struct connection_pool *cpool,
        const char *cookie, GError **err);
bool _vmnetfs_transport_fetch(struct connection_pool *cpool, const char *url,
        const char *username, const char *password, const char *etag,
        time_t last_modified, void *buf, uint64_t offset, uint64_t length,
        should_cancel_fn *should_cancel, void *should_cancel_arg,
        GError **err);
bool _vmnetfs_transport_fetch_stream_once(struct connection_pool *cpool,
        const char *url, const char *username, const char *password,
        const char *etag, time_t last_modified, stream_fn *callback,
        void *arg, uint64_t offset, uint64_t length,
        should_cancel_fn *should_cancel, void *should_cancel_arg,
        GError **err);

/* bitmap */
struct bitmap_group *_vmnetfs_bit_group_new(uint64_t initial_bits);
void _vmnetfs_bit_group_free(struct bitmap_group *mgrp);
void _vmnetfs_bit_group_resize(struct bitmap_group *mgrp, uint64_t bits);
void _vmnetfs_bit_group_close(struct bitmap_group *mgrp);
struct bitmap *_vmnetfs_bit_new(struct bitmap_group *mgrp, bool set_on_extend);
void _vmnetfs_bit_free(struct bitmap *map);
void _vmnetfs_bit_set(struct bitmap *map, uint64_t bit);
bool _vmnetfs_bit_test(struct bitmap *map, uint64_t bit);
struct vmnetfs_stream_group *_vmnetfs_bit_get_stream_group(struct bitmap *map);

/* stream */
struct vmnetfs_stream;
typedef void (populate_stream_fn)(struct vmnetfs_stream *strm, void *data);
struct vmnetfs_stream_group *_vmnetfs_stream_group_new(
        populate_stream_fn *populate, void *populate_data);
void _vmnetfs_stream_group_close(struct vmnetfs_stream_group *sgrp);
void _vmnetfs_stream_group_free(struct vmnetfs_stream_group *sgrp);
struct vmnetfs_stream *_vmnetfs_stream_new(struct vmnetfs_stream_group *sgrp);
void _vmnetfs_stream_free(struct vmnetfs_stream *strm);
uint64_t _vmnetfs_stream_read(struct vmnetfs_stream *strm, void *buf,
        uint64_t count, bool blocking, GError **err);
void _vmnetfs_stream_write(struct vmnetfs_stream *strm, const char *fmt, ...);
void _vmnetfs_stream_group_write(struct vmnetfs_stream_group *sgrp,
        const char *fmt, ...);
bool _vmnetfs_stream_add_poll_handle(struct vmnetfs_stream *strm,
        struct fuse_pollhandle *ph);

/* stats */
struct vmnetfs_stat_handle;
struct vmnetfs_stat *_vmnetfs_stat_new(void);
void _vmnetfs_stat_close(struct vmnetfs_stat *stat);
bool _vmnetfs_stat_is_closed(struct vmnetfs_stat *stat);
void _vmnetfs_stat_free(struct vmnetfs_stat *stat);
bool _vmnetfs_stat_add_poll_handle(struct vmnetfs_stat *stat,
        struct fuse_pollhandle *ph, uint64_t change_cookie);
void _vmnetfs_u64_stat_increment(struct vmnetfs_stat *stat, uint64_t val);
uint64_t _vmnetfs_u64_stat_get(struct vmnetfs_stat *stat,
        uint64_t *change_cookie);

/* pollable */
struct vmnetfs_pollable *_vmnetfs_pollable_new(void);
uint64_t _vmnetfs_pollable_get_change_cookie(struct vmnetfs_pollable *pll);
void _vmnetfs_pollable_add_poll_handle(struct vmnetfs_pollable *pll,
        struct fuse_pollhandle *ph, bool changed);
bool _vmnetfs_pollable_add_poll_handle_conditional(
        struct vmnetfs_pollable *pll, struct fuse_pollhandle *ph,
        uint64_t change_cookie);
void _vmnetfs_pollable_change(struct vmnetfs_pollable *pll);
void _vmnetfs_pollable_free(struct vmnetfs_pollable *pll);

/* cond */
struct vmnetfs_cond *_vmnetfs_cond_new(void);
void _vmnetfs_cond_free(struct vmnetfs_cond *cond);
bool _vmnetfs_cond_wait(struct vmnetfs_cond *cond, GMutex *lock);
void _vmnetfs_cond_signal(struct vmnetfs_cond *cond);
void _vmnetfs_cond_broadcast(struct vmnetfs_cond *cond);

/* log */
struct vmnetfs_log *_vmnetfs_log_init(bool stderr);
struct vmnetfs_stream_group *_vmnetfs_log_get_stream_group(
        struct vmnetfs_log *log);
void _vmnetfs_log_close(struct vmnetfs_log *log);
void _vmnetfs_log_destroy(struct vmnetfs_log *log);

/* utility */
GQuark _vmnetfs_config_error_quark(void);
GQuark _vmnetfs_fuse_error_quark(void);
GQuark _vmnetfs_io_error_quark(void);
GQuark _vmnetfs_stream_error_quark(void);
GQuark _vmnetfs_transport_error_quark(void);
bool _vmnetfs_safe_pread(const char *file, int fd, void *buf, uint64_t count,
        uint64_t offset, GError **err);
bool _vmnetfs_safe_pwrite(const char *file, int fd, const void *buf,
        uint64_t count, uint64_t offset, GError **err);
void _vmnetfs_cursor_start(struct vmnetfs_image *img,
        struct vmnetfs_cursor *cur, uint64_t start, uint64_t count);
bool _vmnetfs_cursor_chunk(struct vmnetfs_cursor *cur, uint64_t count);

#endif
