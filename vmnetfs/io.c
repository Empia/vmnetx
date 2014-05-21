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

#include <string.h>
#include <inttypes.h>
#include "vmnetfs-private.h"

struct chunk_state {
    GMutex *lock;
    GHashTable *chunk_locks;
    uint64_t image_size;
    struct vmnetfs_pollable *image_size_pll;
    bool image_closed;
};

struct chunk_lock {
    uint64_t chunk;
    struct vmnetfs_cond *available;
    bool busy;
    uint32_t waiters;
};

struct stream_state {
    uint64_t start_chunk;
    uint64_t chunks;
    GThread *thread;
    gint stop;  /* atomic operations only */

    /* Private to stream thread */
    char *buf;
    struct vmnetfs_cursor cur;
};

static struct chunk_state *chunk_state_new(uint64_t initial_size)
{
    struct chunk_state *cs;

    cs = g_slice_new0(struct chunk_state);
    cs->lock = g_mutex_new();
    cs->chunk_locks = g_hash_table_new(g_int64_hash, g_int64_equal);
    cs->image_size = initial_size;
    cs->image_size_pll = _vmnetfs_pollable_new();
    return cs;
}

static void chunk_state_free(struct chunk_state *cs)
{
    g_assert(g_hash_table_size(cs->chunk_locks) == 0);

    _vmnetfs_pollable_free(cs->image_size_pll);
    g_hash_table_destroy(cs->chunk_locks);
    g_mutex_free(cs->lock);
    g_slice_free(struct chunk_state, cs);
}

/* chunk_state lock must be held.  When reducing the image size, races
   with other writers must have been avoided by the caller.  Do not call
   this function directly! */
static bool _set_image_size(struct vmnetfs_image *img, uint64_t new_size,
        GError **err)
{
    if (!_vmnetfs_ll_modified_set_size(img, img->chunk_state->image_size,
            new_size, err)) {
        return false;
    }
    img->chunk_state->image_size = new_size;
    _vmnetfs_bit_group_resize(img->bitmaps, (new_size + img->chunk_size - 1) /
            img->chunk_size);
    _vmnetfs_pollable_change(img->chunk_state->image_size_pll);
    return true;
}

/* chunk_state lock must be held. */
static bool expand_image(struct vmnetfs_image *img, uint64_t new_size,
        GError **err)
{
    g_assert(new_size > img->chunk_state->image_size);
    return _set_image_size(img, new_size, err);
}

/* chunk_state lock must be held.
   Returns false if the lock was not acquired because the FUSE request
   was interrupted.  Optionally stores the current image size in
   *image_size; this will not be reduced to impinge on the specified chunk
   while the chunk lock is held. */
static bool G_GNUC_WARN_UNUSED_RESULT _chunk_trylock(struct chunk_state *cs,
        uint64_t chunk, uint64_t *image_size, GError **err)
{
    struct chunk_lock *cl;
    bool ret = true;

    cl = g_hash_table_lookup(cs->chunk_locks, &chunk);
    if (cl != NULL) {
        cl->waiters++;
        while (cl->busy &&
                !_vmnetfs_cond_wait(cl->available, cs->lock)) {}
        if (cl->busy) {
            /* We were interrupted, give up.  If we were interrupted but
               also acquired the lock, we pretend we weren't interrupted
               so that we never have to free the lock in this path. */
            g_set_error(err, VMNETFS_IO_ERROR, VMNETFS_IO_ERROR_INTERRUPTED,
                    "Operation interrupted");
            ret = false;
        } else {
            cl->busy = true;
        }
        cl->waiters--;
    } else {
        cl = g_slice_new0(struct chunk_lock);
        cl->chunk = chunk;
        cl->available = _vmnetfs_cond_new();
        cl->busy = true;
        g_hash_table_replace(cs->chunk_locks, &cl->chunk, cl);
    }
    if (ret && image_size) {
        *image_size = cs->image_size;
    }
    return ret;
}

/* Returns false if the lock was not acquired because the FUSE request
   was interrupted.  Optionally stores the current image size in
   *image_size; this will not be reduced to impinge on the specified chunk
   while the chunk lock is held. */
static bool G_GNUC_WARN_UNUSED_RESULT chunk_trylock(struct vmnetfs_image *img,
        uint64_t chunk, uint64_t *image_size, GError **err)
{
    struct chunk_state *cs = img->chunk_state;
    bool ret;

    g_mutex_lock(cs->lock);
    ret = _chunk_trylock(cs, chunk, image_size, err);
    g_mutex_unlock(cs->lock);
    return ret;
}

/* Returns false if the lock was not acquired because the FUSE request
   was interrupted.  Ensures that the image size is at least needed_size.
   Optionally stores the resulting image size in *image_size; this will not
   be reduced to impinge on the specified chunk while the chunk lock is
   held. */
static bool G_GNUC_WARN_UNUSED_RESULT chunk_trylock_ensure_size(
        struct vmnetfs_image *img, uint64_t chunk, uint64_t needed_size,
        uint64_t *image_size, GError **err)
{
    struct chunk_state *cs = img->chunk_state;
    bool ret = true;

    g_mutex_lock(cs->lock);
    if (cs->image_size < needed_size) {
        ret = expand_image(img, needed_size, err);
    }
    if (ret) {
        ret = _chunk_trylock(cs, chunk, image_size, err);
    }
    g_mutex_unlock(cs->lock);
    return ret;
}

/* chunk_state lock must be held. */
static void _chunk_unlock(struct chunk_state *cs, uint64_t chunk)
{
    struct chunk_lock *cl;

    cl = g_hash_table_lookup(cs->chunk_locks, &chunk);
    g_assert(cl != NULL);
    if (cl->waiters > 0) {
        cl->busy = false;
        _vmnetfs_cond_signal(cl->available);
    } else {
        g_hash_table_remove(cs->chunk_locks, &chunk);
        _vmnetfs_cond_free(cl->available);
        g_slice_free(struct chunk_lock, cl);
    }
}

static void chunk_unlock(struct vmnetfs_image *img, uint64_t chunk)
{
    struct chunk_state *cs = img->chunk_state;

    g_mutex_lock(cs->lock);
    _chunk_unlock(cs, chunk);
    g_mutex_unlock(cs->lock);
}

static bool io_interrupted(void *data G_GNUC_UNUSED)
{
    return _vmnetfs_fuse_interrupted();
}

/* Fetch the specified byte range from the image. */
static bool fetch_data(struct vmnetfs_image *img, void *buf, uint64_t start,
        uint64_t count, GError **err)
{
    return _vmnetfs_transport_fetch(img->cpool, img->url, img->username,
            img->password, img->etag, img->last_modified, buf,
            start + img->fetch_offset, count, io_interrupted, NULL, err);
}

static bool stream_callback(void *arg, const void *buf, uint64_t count,
        GError **err)
{
    struct vmnetfs_image *img = arg;
    struct stream_state *state = img->stream;
    struct vmnetfs_cursor *cur = &state->cur;
    const char *data = buf;
    uint64_t cur_count = 0;

    while (_vmnetfs_cursor_chunk(cur, cur_count) && count > 0) {
        cur_count = MIN(count, cur->length);
        memcpy(state->buf + cur->offset, data, cur_count);
        data += cur_count;
        count -= cur_count;

        if (cur_count == cur->length) {
            /* End of chunk */
            if (!_vmnetfs_ll_pristine_write_chunk(img, state->buf,
                    cur->chunk, cur->offset + cur->length, err)) {
                return false;
            }
            if (cur->offset + cur->length == img->chunk_size) {
                /* We are advancing to the next chunk, so release lock */
                chunk_unlock(img, cur->chunk);
            }
        }
    }
    /* transport should ensure we don't receive more data than requested */
    g_assert(count == 0);

    return true;
}

static bool stream_should_stop(void *arg)
{
    struct vmnetfs_image *img = arg;

    return g_atomic_int_get(&img->stream->stop);
}

static bool do_stream(struct vmnetfs_image *img, GError **err)
{
    struct stream_state *state = img->stream;
    struct chunk_state *cs = img->chunk_state;
    uint64_t offset = state->start_chunk * img->chunk_size;
    uint64_t chunk;
    GError *my_err = NULL;

    /* Set up */
    _vmnetfs_cursor_start(img, &state->cur, offset,
            img->initial_size - offset);

    /* Fetch data */
    state->buf = g_malloc(img->chunk_size);
    _vmnetfs_transport_fetch_stream_once(img->cpool, img->url,
            img->username, img->password, img->etag, img->last_modified,
            stream_callback, img, img->fetch_offset + offset,
            img->initial_size - offset, stream_should_stop, img, &my_err);
    g_free(state->buf);
    /* transport will report short reads */

    /* Release remaining chunk locks */
    g_mutex_lock(cs->lock);
    for (chunk = state->cur.chunk; chunk < state->chunks; chunk++) {
        _chunk_unlock(cs, chunk);
    }
    g_mutex_unlock(cs->lock);

    /* Handle fetch errors */
    if (my_err) {
        g_propagate_prefixed_error(err, my_err,
                "Image streaming failed after %"G_GUINT64_FORMAT" bytes: ",
                state->cur.io_offset);
        return false;
    }

    return true;
}

static void *stream_thread(void *data)
{
    struct vmnetfs_image *img = data;
    GError *my_err = NULL;

    if (!do_stream(img, &my_err)) {
        if (!g_error_matches(my_err, VMNETFS_IO_ERROR,
                VMNETFS_IO_ERROR_INTERRUPTED)) {
            g_warning("%s", my_err->message);
        }
        g_clear_error(&my_err);
    }
    return NULL;
}

/* Must run before FUSE starts serving requests. */
static bool stream_start(struct vmnetfs_image *img, GError **err)
{
    struct chunk_state *cs = img->chunk_state;
    uint64_t chunks = (img->initial_size + img->chunk_size - 1) /
            img->chunk_size;
    uint64_t start_chunk;
    uint64_t chunk;

    g_assert(!img->stream);

    /* If we already have the last chunk, we don't need to stream */
    if (_vmnetfs_bit_test(img->present_map, chunks - 1)) {
        return true;
    }

    /* Find first missing chunk */
    for (start_chunk = 0; start_chunk < chunks; start_chunk++) {
        if (!_vmnetfs_bit_test(img->present_map, start_chunk)) {
            break;
        }
    }

    /* Lock chunks to be streamed */
    g_mutex_lock(cs->lock);
    for (chunk = start_chunk; chunk < chunks; chunk++) {
        if (!_chunk_trylock(cs, chunk, NULL, err)) {
            goto bad_locked;
        }
    }
    g_mutex_unlock(cs->lock);

    /* Allocate state */
    img->stream = g_slice_new0(struct stream_state);
    img->stream->start_chunk = start_chunk;
    img->stream->chunks = chunks;

    /* Start streamer */
    img->stream->thread = g_thread_create(stream_thread, img, TRUE, err);
    if (!img->stream->thread) {
        goto bad;
    }

    return true;

bad:
    g_mutex_lock(cs->lock);
bad_locked:
    do {
        _chunk_unlock(cs, --chunk);
    } while (chunk > start_chunk);
    g_mutex_unlock(cs->lock);

    if (img->stream) {
        g_slice_free(struct stream_state, img->stream);
        img->stream = NULL;
    }
    return false;
}

static void stream_stop(struct vmnetfs_image *img)
{
    if (img->stream) {
        g_atomic_int_set(&img->stream->stop, 1);
    }
}

bool _vmnetfs_io_init(struct vmnetfs_image *img, GError **err)
{
    GList *cur;

    img->bitmaps = _vmnetfs_bit_group_new((img->initial_size +
            img->chunk_size - 1) / img->chunk_size);
    if (!_vmnetfs_ll_pristine_init(img, err)) {
        _vmnetfs_bit_group_free(img->bitmaps);
        return false;
    }
    if (!_vmnetfs_ll_modified_init(img, err)) {
        _vmnetfs_ll_pristine_destroy(img);
        _vmnetfs_bit_group_free(img->bitmaps);
        return false;
    }
    img->cpool = _vmnetfs_transport_pool_new(err);
    if (img->cpool == NULL) {
        _vmnetfs_ll_modified_destroy(img);
        _vmnetfs_ll_pristine_destroy(img);
        _vmnetfs_bit_group_free(img->bitmaps);
        return false;
    }
    for (cur = img->cookies; cur != NULL; cur = cur->next) {
        if (!_vmnetfs_transport_pool_set_cookie(img->cpool, cur->data, err)) {
            _vmnetfs_transport_pool_free(img->cpool);
            _vmnetfs_ll_modified_destroy(img);
            _vmnetfs_ll_pristine_destroy(img);
            _vmnetfs_bit_group_free(img->bitmaps);
            return false;
        }
    }
    img->accessed_map = _vmnetfs_bit_new(img->bitmaps, false);
    img->chunk_state = chunk_state_new(img->initial_size);
    return true;
}

/* Cannot fail because we have already committed to launch. */
void _vmnetfs_io_open(struct vmnetfs_image *img)
{
    GError *my_err = NULL;

    if (img->fetch_mode == FETCH_MODE_STREAM) {
        if (!stream_start(img, &my_err)) {
            g_warning("Couldn't start streaming: %s", my_err->message);
            g_clear_error(&my_err);
        }
    }
}

void _vmnetfs_io_close(struct vmnetfs_image *img)
{
    struct chunk_state *cs = img->chunk_state;

    stream_stop(img);
    _vmnetfs_bit_group_close(img->bitmaps);

    g_mutex_lock(cs->lock);
    cs->image_closed = true;
    _vmnetfs_pollable_change(cs->image_size_pll);
    g_mutex_unlock(cs->lock);
}

bool _vmnetfs_io_image_is_closed(struct vmnetfs_image *img)
{
    struct chunk_state *cs = img->chunk_state;
    bool ret;

    g_mutex_lock(cs->lock);
    ret = cs->image_closed;
    g_mutex_unlock(cs->lock);
    return ret;
}

void _vmnetfs_io_destroy(struct vmnetfs_image *img)
{
    if (img == NULL) {
        return;
    }
    if (img->stream) {
        stream_stop(img);
        g_thread_join(img->stream->thread);
        g_slice_free(struct stream_state, img->stream);
    }
    _vmnetfs_ll_modified_destroy(img);
    _vmnetfs_ll_pristine_destroy(img);
    chunk_state_free(img->chunk_state);
    _vmnetfs_bit_free(img->accessed_map);
    _vmnetfs_bit_group_free(img->bitmaps);
    _vmnetfs_transport_pool_free(img->cpool);
}

static uint64_t read_chunk_unlocked(struct vmnetfs_image *img,
        uint64_t image_size, void *data, uint64_t chunk, uint32_t offset,
        uint32_t length, GError **err)
{
    g_assert(offset < img->chunk_size);
    g_assert(offset + length <= img->chunk_size);

    if (chunk * img->chunk_size + offset >= image_size) {
        g_set_error(err, VMNETFS_IO_ERROR, VMNETFS_IO_ERROR_EOF,
                "End of file");
        return false;
    }
    length = MIN(image_size - chunk * img->chunk_size - offset, length);
    _vmnetfs_bit_set(img->accessed_map, chunk);
    if (_vmnetfs_bit_test(img->modified_map, chunk)) {
        if (!_vmnetfs_ll_modified_read_chunk(img, image_size, data, chunk,
                offset, length, err)) {
            return 0;
        }
    } else {
        /* If two vmnetfs instances are working out of the same pristine
           cache, they will redundantly fetch chunks due to our failure to
           keep the present map up to date. */
        if (!_vmnetfs_bit_test(img->present_map, chunk)) {
            uint64_t start = chunk * img->chunk_size;
            uint64_t count = MIN(img->initial_size - start, img->chunk_size);
            void *buf = g_malloc(count);

            _vmnetfs_u64_stat_increment(img->chunk_fetches, 1);
            if (!fetch_data(img, buf, start, count, err)) {
                g_free(buf);
                return 0;
            }
            bool ok = _vmnetfs_ll_pristine_write_chunk(img, buf, chunk,
                    count, err);
            g_free(buf);
            if (!ok) {
                return 0;
            }
        }
        if (!_vmnetfs_ll_pristine_read_chunk(img, data, chunk, offset,
                length, err)) {
            return 0;
        }
    }
    return length;
}

uint64_t _vmnetfs_io_read_chunk(struct vmnetfs_image *img, void *data,
        uint64_t chunk, uint32_t offset, uint32_t length, GError **err)
{
    uint64_t image_size;
    uint64_t ret;

    if (!chunk_trylock(img, chunk, &image_size, err)) {
        return false;
    }
    ret = read_chunk_unlocked(img, image_size, data, chunk, offset, length,
            err);
    chunk_unlock(img, chunk);
    return ret;
}

/* chunk lock must be held. */
static bool copy_to_modified(struct vmnetfs_image *img, uint64_t image_size,
        uint64_t chunk, GError **err)
{
    uint64_t count;
    uint64_t read_count;
    void *buf;
    bool ret;
    GError *my_err = NULL;

    count = MIN(img->initial_size - chunk * img->chunk_size, img->chunk_size);
    buf = g_malloc(count);

    _vmnetfs_u64_stat_increment(img->chunk_dirties, 1);
    read_count = read_chunk_unlocked(img, image_size, buf, chunk, 0, count,
            &my_err);
    if (read_count != count) {
        if (!my_err) {
            g_set_error(err, VMNETFS_IO_ERROR, VMNETFS_IO_ERROR_PREMATURE_EOF,
                    "Short count %"PRIu64"/%"PRIu64, read_count, count);
        } else {
            g_propagate_error(err, my_err);
        }
        g_free(buf);
        return false;
    }
    ret = _vmnetfs_ll_modified_write_chunk(img, image_size, buf, chunk,
            0, count, err);

    g_free(buf);
    return ret;
}

static bool lock_and_copy_to_modified(struct vmnetfs_image *img,
        uint64_t chunk, GError **err) {
    uint64_t image_size;
    bool ret = true;

    if (!chunk_trylock(img, chunk, &image_size, err)) {
        return false;
    }
    /* If the chunk is still unmodified, and has not been truncated away
       while we had the lock released, copy it to the modified cache. */
    if (chunk * img->chunk_size < image_size &&
            !_vmnetfs_bit_test(img->modified_map, chunk)) {
        ret = copy_to_modified(img, image_size, chunk, err);
    }
    chunk_unlock(img, chunk);
    return ret;
}

uint64_t _vmnetfs_io_write_chunk(struct vmnetfs_image *img, const void *data,
        uint64_t chunk, uint32_t offset, uint32_t length, GError **err)
{
    uint64_t image_size;
    uint64_t ret = 0;

    g_assert(offset < img->chunk_size);
    g_assert(offset + length <= img->chunk_size);

    if (!chunk_trylock_ensure_size(img, chunk,
            chunk * img->chunk_size + offset + length, &image_size, err)) {
        return 0;
    }
    _vmnetfs_bit_set(img->accessed_map, chunk);
    if (!_vmnetfs_bit_test(img->modified_map, chunk)) {
        if (!copy_to_modified(img, image_size, chunk, err)) {
            goto out;
        }
    }
    if (_vmnetfs_ll_modified_write_chunk(img, image_size, data, chunk,
            offset, length, err)) {
        ret = length;
    }
out:
    chunk_unlock(img, chunk);
    return ret;
}

uint64_t _vmnetfs_io_get_image_size(struct vmnetfs_image *img,
        uint64_t *change_cookie)
{
    uint64_t ret;

    g_mutex_lock(img->chunk_state->lock);
    ret = img->chunk_state->image_size;
    if (change_cookie != NULL) {
        *change_cookie = _vmnetfs_pollable_get_change_cookie(
                img->chunk_state->image_size_pll);
    }
    g_mutex_unlock(img->chunk_state->lock);
    return ret;
}

bool _vmnetfs_io_set_image_size(struct vmnetfs_image *img, uint64_t size,
        GError **err)
{
    struct chunk_state *cs = img->chunk_state;
    uint64_t chunk;
    bool ret;

    g_mutex_lock(cs->lock);
    if (size > cs->image_size) {
        /* Expand image. */
        ret = expand_image(img, size, err);
        g_mutex_unlock(cs->lock);
        return ret;

    } else if (size < cs->image_size) {
        /* Reduce image. */

        if (size % img->chunk_size > 0 && size < img->initial_size &&
                !_vmnetfs_bit_test(img->modified_map,
                (size - 1) / img->chunk_size)) {
            /* The new last chunk will be a partial chunk within the
               boundaries of the pristine cache, and it is not in the
               modified cache.  Copy it there so that subsequent expansions
               don't reveal the truncated part of the chunk. */
            g_mutex_unlock(cs->lock);
            if (!lock_and_copy_to_modified(img, (size - 1) / img->chunk_size,
                    err)) {
                return false;
            }
            /* Image size may have changed; start over. */
            return _vmnetfs_io_set_image_size(img, size, err);
        }

        /* We can't truncate a chunk currently being accessed.  Truncate
           as far as we can until we hit a busy chunk, then wait for that
           chunk's lock, then start over. */
        chunk = (cs->image_size - 1) / img->chunk_size;
        do {
            if (g_hash_table_lookup(cs->chunk_locks, &chunk)) {
                uint64_t new_size = (chunk + 1) * img->chunk_size;
                ret = true;
                if (new_size < cs->image_size) {
                    ret = _set_image_size(img, new_size, err);
                }
                g_mutex_unlock(cs->lock);
                if (!ret) {
                    return false;
                }
                if (!chunk_trylock(img, chunk, NULL, err)) {
                    return false;
                }
                chunk_unlock(img, chunk);
                /* Start over */
                return _vmnetfs_io_set_image_size(img, size, err);
            }
        } while (chunk > 0 && --chunk >= size / img->chunk_size);

        ret = _set_image_size(img, size, err);
        g_mutex_unlock(cs->lock);
        return ret;

    } else {
        /* Size unchanged. */
        g_mutex_unlock(cs->lock);
        return true;
    }
}

bool _vmnetfs_io_image_size_add_poll_handle(struct vmnetfs_image *img,
        struct fuse_pollhandle *ph, uint64_t change_cookie)
{
    struct chunk_state *cs = img->chunk_state;
    bool changed;

    g_mutex_lock(cs->lock);
    if (cs->image_closed) {
        _vmnetfs_pollable_add_poll_handle(cs->image_size_pll, ph, true);
        changed = true;
    } else {
        changed = _vmnetfs_pollable_add_poll_handle_conditional(
                cs->image_size_pll, ph, change_cookie);
    }
    g_mutex_unlock(cs->lock);
    return changed;
}
