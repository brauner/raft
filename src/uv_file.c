#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <xfs/xfs.h>

#include <uv.h>

#include "aio.h"
#include "assert.h"
#include "uv_file.h"

/**
 Handle flags
 */
enum {
    UV__FILE_CREATING = 0x0001,
    UV__FILE_CLOSING = 0x0002,
    UV__FILE_CLOSED = 0x0004
};

#if defined(RWF_NOWAIT)
/**
 * Try to write @fd using a memory-aligned buffer of size @size. If the write is
 * successful, @ok will be set to #true.
 */
static int probe_block_size(int fd, size_t size, bool *ok)
{
    void *buf;                  /* Buffer to use for the probe write */
    aio_context_t ctx = 0;      /* KAIO context handle */
    struct iocb iocb;           /* KAIO request object */
    struct iocb *iocbs = &iocb; /* Because the io_submit() API sucks */
    struct io_event event;      /* KAIO response object */
    int rv;

    /* Setup the KAIO context handle */
    rv = io_setup(1, &ctx);
    if (rv == -1) {
        /* UNTESTED: in practice this should fail only with ENOMEM */
        return uv_translate_sys_error(errno);
    }

    /* Allocate the write buffer */
    buf = aligned_alloc(size, size);
    if (buf == NULL) {
        /* UNTESTED: define a configurable allocator that can fail? */
        return UV_ENOMEM;
    }
    memset(buf, 0, size);

    /* Prepare the KAIO request object */
    memset(&iocb, 0, sizeof iocb);
    iocb.aio_lio_opcode = IOCB_CMD_PWRITE;
    iocb.aio_buf = (uint64_t)buf;
    iocb.aio_nbytes = size;
    iocb.aio_offset = 0;
    iocb.aio_fildes = fd;
    iocb.aio_reqprio = 0;
    iocb.aio_rw_flags |= RWF_NOWAIT | RWF_DSYNC;

    /* Submit the KAIO request */
    rv = io_submit(ctx, 1, &iocbs);
    if (rv == -1) {
        /* UNTESTED: in practice this should fail only with ENOMEM */
        free(buf);
        io_destroy(ctx);
        return uv_translate_sys_error(errno);
    }

    /* Fetch the response: will block until done. */
    do {
        rv = io_getevents(ctx, 1, 1, &event, NULL);
    } while (rv == -1 && errno == EINTR);

    assert(rv == 1);

    /* Release the write buffer. */
    free(buf);

    /* Release the KAIO context handle. */
    io_destroy(ctx);

    /* We expect the write to either succeed or fail with EAGAIN (which means
     * this is not the correct block size). */
    if (event.res > 0) {
        *ok = true;
    } else {
        if (event.res != -EAGAIN) {
            /* UNTESTED: this should basically fail only because of disk errors,
             * since we allocated the file with posix_fallocate. */
            return uv_translate_sys_error(-event.res);
        }
        *ok = false;
    }

    return 0;
}
#endif

/**
 * Run blocking syscalls involved in file creation (e.g. posix_fallocate()).
 *
 * This is called in UV's threadpool.
 */
static void uv__file_create_work_cb(uv_work_t *work);

/**
 * Sync the directory where @path lives in. This is necessary in order to ensure
 * that the new entry is saved in the directory inode. Called in a thread as
 * part of file creation.
 */
static int uv__file_create_work_sync_dir(const char *path);

/**
 * Attempt to use direct I/O. If we can't, check if the file system does not
 * support direct I/O and ignore the error in that case. Called in a thread as
 * part of file creation.
 */
static int uv__file_create_work_set_direct_io(struct uv__file *f);

/**
 * Main loop callback run after @uv__file_create_work_cb has returned. It
 * will normally start the eventfd poller to receive notification about
 * completed writes and invoke the create request callback.
 */
static void uv__file_create_after_work_cb(uv_work_t *work, int status);

/**
 * Callback fired when the event fd associated with AIO write requests should be
 * ready for reading (i.e. when a write has completed).
 */
static void uv__file_write_poll_cb(uv_poll_t *poller, int status, int events);

/**
 * Run blocking syscalls involved in a file write request.
 *
 * Perform a KAIO write request and synchronously wait for it to complete.
 */
static void uv__file_write_work_cb(uv_work_t *work);

/**
 * Callback run after @uv__file_write_work_cb has returned. It normally
 * invokes the write request callback.
 */
static void uv__file_write_after_work_cb(uv_work_t *work, int status);

/**
 * Remove the request from the queue of inflight writes and invoke the request
 * callback if set.
 */
static void uv__file_write_finish(struct uv__file_write *req);

/**
 * Close the poller if there's no infiglht create or write request.
 */
static void uv__file_maybe_closed(struct uv__file *f);

/**
 * Invoked at the end of the close sequence. It invokes the close callback.
 */
static void uv__file_poll_close_cb(struct uv_handle_s *handle);

int uv__file_block_size(const char *dir, size_t *size)
{
    struct statfs fs_info; /* To get the type code of the underlying fs */
    struct stat info;      /* To get the block size reported by the fs */
    const char *filename;  /* Filename of the probe file */
    char *path;            /* Full path of the probe file */
    int fd;                /* File descriptor of the probe file */
    int flags;             /* To hold the current fcntl flags */
    int rv;

    assert(dir != NULL);
    assert(size != NULL);

#if !defined(RWF_NOWAIT)
    /* If NOWAIT is not supported, just return 4096. In practice it should
     * always work fine. */
    *size = 4096;
    return 0;
#else

    filename = ".probe-XXXXXX";
    path = malloc(strlen(dir) + strlen("/") + strlen(filename) + 1);
    if (path == NULL) {
        rv = UV_ENOMEM;
        goto err;
    }

    /* Create a temporary probe file. */
    sprintf(path, "%s/%s", dir, filename);

    fd = mkstemp(path);
    if (fd == -1) {
        rv = uv_translate_sys_error(errno);
        goto err_after_path_alloc;
    }

    unlink(path);

    /* For XFS, we can use the dedicated API to figure out the optimal block
     * size */
    if (platform_test_xfs_fd(fd)) {
        struct dioattr attr;

        rv = xfsctl(path, fd, XFS_IOC_DIOINFO, &attr);

        if (rv != 0) {
            /* UNTESTED: since the path and fd are valid, can this ever fail? */
            rv = uv_translate_sys_error(errno);
            goto err_after_file_open;
        }

        /* TODO: this was taken from seastar's code which has this comment:
         *
         *   xfs wants at least the block size for writes
         *   FIXME: really read the block size
         *
         * We should check with the xfs folks what's going on here and possibly
         * how to figure the device block size.
         */
        *size = attr.d_miniosz > 4096 ? attr.d_miniosz : 4096;

        goto out;
    }

    /* Get the file system type */
    rv = fstatfs(fd, &fs_info);
    if (rv == -1) {
        /* UNTESTED: in practice ENOMEM should be the only failure mode */
        rv = uv_translate_sys_error(errno);
        goto err_after_file_open;
    }

    /* Special-case the file systems that do not support O_DIRECT/NOWAIT */
    switch (fs_info.f_type) {
        case 0x01021994: /* tmpfs */
            /* 4096 is ok. */
            *size = 4096;
            goto out;

        case 0x2fc12fc1: /* ZFS */
            /* Let's use whatever stat() returns. */
            rv = fstat(fd, &info);
            if (rv != 0) {
                /* UNTESTED: ENOMEM should be the only failure mode */
                rv = uv_translate_sys_error(errno);
                goto err_after_file_open;
            }
            /* TODO: The block size indicated in the ZFS case seems way to
             * high. Reducing it to 4096 seems safe since at the moment ZFS does
             * not support async writes. We might want to change this once ZFS
             * async support is released. */
            *size = info.st_blksize > 4096 ? 4096 : info.st_blksize;
            goto out;
    }

    /* For all other file systems, we try to probe the correct size by trial and
     * error. */
    rv = posix_fallocate(fd, 0, 4096);
    if (rv != 0) {
        rv = uv_translate_sys_error(rv);
        goto err_after_file_open;
    }

    flags = fcntl(fd, F_GETFL);
    rv = fcntl(fd, F_SETFL, flags | O_DIRECT);
    if (rv == -1) {
        /* UNTESTED: this should actually never fail, for the file systems we
         * currently support. */
        rv = uv_translate_sys_error(errno);
        goto err_after_file_open;
    }

    *size = 4096;
    while (*size >= 512) {
        bool ok = false;

        rv = probe_block_size(fd, *size, &ok);
        if (rv != 0) {
            /* UNTESTED: all syscalls performed by underlying code should fail
             * at most with ENOMEM. */
            goto err_after_file_open;
        }

        if (ok) {
            goto out;
        }

        *size = *size / 2;
    }

    /* UNTESTED: at least one of the probed block sizes should work for the file
     * systems we currently support. */
    rv = UV_EINVAL;
    goto err_after_file_open;

out:
    close(fd);
    free(path);

    return 0;

err_after_file_open:
    close(fd);

err_after_path_alloc:
    free(path);

err:
    assert(rv != 0);

    return rv;
#endif /* RWF_NOWAIT */
}

int uv__file_init(struct uv__file *f, struct uv_loop_s *loop)
{
    int rv;

    f->loop = loop;
    f->flags = 0;
    f->fd = -1;
    f->async = true;
    f->event_fd = -1;

    /* Create an event file descriptor to get notified when a write has
     * completed. */
    f->event_fd = eventfd(0, EFD_NONBLOCK);
    if (f->event_fd < 0) {
        /* UNTESTED: should fail only with ENOMEM */
        rv = uv_translate_sys_error(errno);
        goto err;
    }

    rv = uv_poll_init(f->loop, &f->event_poller, f->event_fd);
    if (rv != 0) {
        /* UNTESTED: with the current libuv implementation this should never
         * fail. */
        goto err_after_event_fd;
    }
    f->event_poller.data = f;

    f->ctx = 0;
    f->events = NULL;
    f->n_events = 0;

    RAFT__QUEUE_INIT(&f->write_queue);

    f->close_cb = NULL;

    return 0;

err_after_event_fd:
    close(f->event_fd);

err:
    assert(rv != 0);

    return rv;
}

int uv__file_create(struct uv__file *f,
                    struct uv__file_create *req,
                    const char *path,
                    size_t size,
                    unsigned max_n_writes,
                    uv__file_create_cb cb)
{
    int flags = O_WRONLY | O_CREAT | O_EXCL; /* Common open flags */
    int rv;

    assert(path != NULL);
    assert(size > 0);
    assert(!uv__file_is_closing(f));

    f->flags |= UV__FILE_CREATING;

#if !defined(RWF_DSYNC)
    /* If per-request synchronous I/O is not supported, open the file with the
     * sync flag. */
    flags |= O_DSYNC;
#endif

    f->events = NULL; /* We'll allocate this in the create callback */
    f->n_events = max_n_writes;

    /* Try to create a brand new file. */
    f->fd = open(path, flags, S_IRUSR | S_IWUSR);
    if (f->fd == -1) {
        rv = uv_translate_sys_error(errno);
        goto err;
    }

    /* Setup the AIO context. */
    rv = io_setup(f->n_events /* Maximum concurrent requests */, &f->ctx);
    if (rv == -1) {
        /* UNTESTED: should fail only with ENOMEM */
        rv = uv_translate_sys_error(errno);
        goto err_after_open;
    }

    /* Initialize the array of re-usable event objects. */
    f->events = calloc(f->n_events, sizeof *f->events);
    if (f->events == NULL) {
        /* UNTESTED: define a configurable allocator that can fail? */
        rv = UV_ENOMEM;
        goto err_after_io_setup;
    }

    req->file = f;
    req->cb = cb;
    req->path = path;
    req->size = size;
    req->status = 0;
    req->work.data = req;

    rv = uv_queue_work(f->loop, &req->work, uv__file_create_work_cb,
                       uv__file_create_after_work_cb);
    if (rv != 0) {
        /* UNTESTED: with the current libuv implementation this can't fail. */
        goto err_after_open;
    }

    return 0;

err_after_io_setup:
    io_destroy(f->ctx);
    f->ctx = 0;

err_after_open:
    close(f->fd);
    unlink(path);
    f->fd = -1;

err:
    assert(rv != 0);

    f->flags &= ~UV__FILE_CREATING;

    return rv;
}

int uv__file_write(struct uv__file *f,
                   struct uv__file_write *req,
                   const uv_buf_t bufs[],
                   unsigned n,
                   size_t offset,
                   uv__file_write_cb cb)
{
    int rv;
    struct iocb *iocbs = &req->iocb;

    assert(!uv__file_is_closing(f));

    /* TODO: at the moment the io-uv-writer isn't actually supposed to leverage
     *       the support for concurrent writes, so ensure that we're getting
     *       write requests sequentially. */
    if (f->n_events == 1) {
        assert(RAFT__QUEUE_IS_EMPTY(&f->write_queue));
    }

    assert(f->fd >= 0);
    assert(f->event_fd >= 0);
    assert(f->ctx != 0);

    assert(req != NULL);

    assert(bufs != NULL);
    assert(n > 0);

    req->file = f;
    req->cb = cb;

    memset(&req->iocb, 0, sizeof req->iocb);

    req->iocb.aio_data = (uint64_t)req;
    req->iocb.aio_lio_opcode = IOCB_CMD_PWRITEV;
    req->iocb.aio_buf = (uint64_t)bufs;
    req->iocb.aio_nbytes = n;
    req->iocb.aio_offset = offset;
    req->iocb.aio_fildes = f->fd;
    req->iocb.aio_reqprio = 0;

    RAFT__QUEUE_PUSH(&f->write_queue, &req->queue);

#if defined(RWF_HIPRI)
    /* High priority request, if possible */
    req->iocb.aio_rw_flags |= RWF_HIPRI;
#endif

#if defined(RWF_DSYNC)
    /* Use per-request synchronous I/O if available. Otherwise, we have opened
     * the file with O_DSYNC. */
    req->iocb.aio_rw_flags |= RWF_DSYNC;
#endif

#if defined(RWF_NOWAIT)
    /* If io_submit can be run in a 100% non-blocking way, we'll try to write
     * without using the threadpool, unless we had previously detected that this
     * mode is not supported. */
    if (f->async) {
        req->iocb.aio_flags |= IOCB_FLAG_RESFD;
        req->iocb.aio_resfd = f->event_fd;
        req->iocb.aio_rw_flags |= RWF_NOWAIT;
    }
#else
    /* Since there's no support for NOWAIT, io_submit might occasionally block
     * and we need to run it in the threadpool. */
    f->async = false;
#endif /* RWF_NOWAIT */

#if defined(RWF_NOWAIT)
    /* Try to submit the write request asynchronously */
    if (f->async) {
        rv = io_submit(f->ctx, 1, &iocbs);

        /* If no error occurred, we're done, the write request was
         * submitted. */
        if (rv != -1) {
            assert(rv == 1); /* TODO: can 0 be returned? */
            goto done;
        }

        /* Check the reason of the error. */
        switch (errno) {
            case EOPNOTSUPP:
                /* NOWAIT is not supported, fallback to sync mode */
                f->async = false;
            case EAGAIN:
                /* Submitting the write would block, or NOWAIT is not
                 * supported. Let's run this request in the threadpool. */
                req->iocb.aio_flags &= ~IOCB_FLAG_RESFD;
                req->iocb.aio_resfd = 0;
                req->iocb.aio_rw_flags &= ~RWF_NOWAIT;
                break;
            default:
                /* Unexpected error */
                rv = uv_translate_sys_error(errno);
                goto err;
        }
    }
#endif /* RWF_NOWAIT */

    /* If we got here it means we need to run io_submit in the threadpool. */
    req->work.data = req;

    rv = uv_queue_work(f->loop, &req->work, uv__file_write_work_cb,
                       uv__file_write_after_work_cb);
    if (rv != 0) {
        /* UNTESTED: with the current libuv implementation this can't fail. */
        goto err;
    }

done:
    return 0;

err:
    assert(rv != 0);

    RAFT__QUEUE_REMOVE(&req->queue);

    return rv;
}

void uv__file_close(struct uv__file *f, uv__file_close_cb cb)
{
    int rv;

    assert(!uv__file_is_closing(f));

    f->flags |= UV__FILE_CLOSING;
    f->close_cb = cb;

    if (f->fd != -1) {
        rv = close(f->fd);
        assert(rv == 0);
    }

    uv__file_maybe_closed(f);
}

bool uv__file_is_closing(struct uv__file *f)
{
    return (f->flags & (UV__FILE_CLOSING | UV__FILE_CLOSED)) != 0;
}

static void uv__file_create_work_cb(uv_work_t *work)
{
    struct uv__file_create *req; /* Create file request object */
    struct uv__file *f;          /* File handle */
    int rv;

    req = work->data;
    f = req->file;

    assert(f->flags & UV__FILE_CREATING);

    /* Allocate the desired size. */
    rv = posix_fallocate(f->fd, 0, req->size);
    if (rv != 0) {
        /* From the manual page:
         *
         *   posix_fallocate() returns zero on success, or an error number on
         *   failure.  Note that errno is not set.
         */
        errno = rv;
        goto err;
    }

    /* Sync the file and its directory */
    rv = fsync(f->fd);
    if (rv == -1) {
        /* UNTESTED: should fail only in case of disk errors */
        goto err;
    }
    rv = uv__file_create_work_sync_dir(req->path);
    if (rv == -1) {
        /* UNTESTED: should fail only in case of disk errors */
        goto err;
    }

    /* Set direct I/O if possible. */
    rv = uv__file_create_work_set_direct_io(f);
    if (rv == -1) {
        goto err;
    }

    req->status = 0;

    return;

err:
    req->status = uv_translate_sys_error(errno);
}

static int uv__file_create_work_sync_dir(const char *path)
{
    char *dir; /* The directory portion of path */
    int fd;    /* Directory file descriptor */
    int rv;

    dir = malloc(strlen(path) + 1);
    if (dir == NULL) {
        errno = ENOMEM;
        return -1;
    }

    strcpy(dir, path);
    dirname(dir);

    fd = open(dir, O_RDONLY | O_DIRECTORY);
    free(dir);
    if (fd == -1) {
        /* UNTESTED: since the directory has been already accessed, this
         * shouldn't fail */
        return -1;
    }

    rv = fsync(fd);

    close(fd);

    return rv;
}

static int uv__file_create_work_set_direct_io(struct uv__file *f)
{
    int flags;             /* Current fcntl flags */
    struct statfs fs_info; /* To check the file system type */
    int rv;

    flags = fcntl(f->fd, F_GETFL);
    rv = fcntl(f->fd, F_SETFL, flags | O_DIRECT);

    if (rv == -1) {
        if (errno != EINVAL) {
            /* UNTESTED: the parameters are ok, so this should never happen. */
            return -1;
        }

        rv = fstatfs(f->fd, &fs_info);
        if (rv == -1) {
            /* UNTESTED: in practice ENOMEM should be the only failure mode */
            return -1;
        }

        switch (fs_info.f_type) {
            case 0x01021994: /* TMPFS_MAGIC */
            case 0x2fc12fc1: /* ZFS magic */
                /* If director I/O is not supported, then io_submit will be
                 * blocking. */
                f->async = false;
                break;
            default:
                /* UNTESTED: this is an unsupported file system. */
                errno = EINVAL;
                return -1;
        }
    }

    return 0;
}

static void uv__file_create_after_work_cb(uv_work_t *work, int status)
{
    struct uv__file_create *req;
    struct uv__file *f;
    int rv;

    assert(work != NULL);

    assert(status == 0); /* We don't cancel worker requests */

    req = work->data;

    f = req->file;

    /* If we were closed, abort here. */
    if (uv__file_is_closing(f)) {
        unlink(req->path);
        req->status = UV_ECANCELED;
        goto out;
    }

    /* If no error occurred, start polling the event file descriptor. */
    if (req->status == 0) {
        rv = uv_poll_start(&f->event_poller, UV_READABLE,
                           uv__file_write_poll_cb);
        if (rv != 0) {
            /* UNTESTED: the underlying libuv calls should never fail. */
            req->status = rv;

            io_destroy(f->ctx);
            close(f->event_fd);
            close(f->fd);
            unlink(req->path);
        }
    }

out:
    if (req->cb != NULL) {
        req->cb(req, req->status);
    }

    f->flags &= ~UV__FILE_CREATING;

    if (uv__file_is_closing(f)) {
        uv__file_maybe_closed(f);
    }
}

static void uv__file_write_poll_cb(uv_poll_t *poller, int status, int events)
{
    struct uv__file *f = poller->data; /* File handle */
    uint64_t completed;                /* True if the write is complete */
    int rv;
    unsigned i;

    assert(f != NULL);
    assert(f->event_fd >= 0);

    /* TODO: it's not clear when polling could fail. In this case we should
     * probably mark all pending requests as failed. */
    assert(status == 0);

    assert(events & UV_READABLE);

    /* Read the event file descriptor */
    rv = read(f->event_fd, &completed, sizeof completed);
    if (rv != sizeof completed) {
        /* UNTESTED: According to eventfd(2) this is the only possible failure
         * mode, meaning that epoll has indicated that the event FD is not yet
         * ready. */
        assert(errno == EAGAIN);
        return;
    }

    /* TODO: this assertion fails in unit tests */
    /* assert(completed == 1); */

    /* Try to fetch the write responses.
     *
     * If we got here a least one write should have completed and io_events
     * should return immediately without blocking. */
    do {
        rv = io_getevents(f->ctx, 1, f->n_events, f->events, NULL);
    } while (rv == -1 && errno == EINTR);

    assert(rv >= 1);

    for (i = 0; i < (unsigned)rv; i++) {
        struct io_event *event = &f->events[i];
        struct uv__file_write *req = (void *)event->data;

        /* If we are closing, we mark the write as canceled, although
         * technically it might have worked. */
        if (uv__file_is_closing(f)) {
            req->status = UV_ECANCELED;
            goto finish;
        }

#if defined(RWF_NOWAIT)
        /* If we got EAGAIN, it means it was not possible to perform the write
         * asynchronously, so let's fall back to the threadpool. */
        if (event->res == -EAGAIN) {
            req->iocb.aio_flags &= ~IOCB_FLAG_RESFD;
            req->iocb.aio_resfd = 0;
            req->iocb.aio_rw_flags &= ~RWF_NOWAIT;

            req->work.data = req;
            rv = uv_queue_work(f->loop, &req->work, uv__file_write_work_cb,
                               uv__file_write_after_work_cb);
            if (rv != 0) {
                /* UNTESTED: with the current libuv implementation this should
                 * never fail. */
                req->status = rv;
                goto finish;
            }

            return;
        }
#endif /* RWF_NOWAIT */

        req->status = event->res;

    finish:

        uv__file_write_finish(req);
    }

    /* If we've been closed, let's see if we can stop the poller and fire the
     * close callback. */
    if (uv__file_is_closing(f)) {
        uv__file_maybe_closed(f);
    }
}

static void uv__file_write_work_cb(uv_work_t *work)
{
    struct uv__file_write *req; /* Write file request object */
    aio_context_t ctx = 0;      /* KAIO handle */
    struct iocb *iocbs;         /* Pointer to KAIO request object */
    struct io_event event;      /* KAIO response object */
    int rv;

    assert(work != NULL);
    assert(work->data != NULL);

    req = work->data;

    /* If we detect that we've been closed, abort immediately. */
    if (uv__file_is_closing(req->file)) {
        rv = -1;
        errno = ECANCELED;
        goto out;
    }

    iocbs = &req->iocb;

    /* Perform the request using a dedicated context, to avoid synchronization
     * issues between threads when multiple write requests are submitted in
     * parallel. This is suboptimal but in real-world users should use file
     * systems and kernels with proper async write support. */

    rv = io_setup(1 /* Maximum concurrent requests */, &ctx);
    if (rv == -1) {
        /* UNTESTED: should fail only with ENOMEM */
        goto out;
    }

    /* Submit the request */
    rv = io_submit(ctx, 1, &iocbs);
    if (rv == -1) {
        /* UNTESTED: since we're not using NOWAIT and the parameters are valid,
         * this shouldn't fail. */
        goto out_after_io_setup;
    }

    /* Wait for the request to complete */
    do {
        rv = io_getevents(ctx, 1, 1, &event, NULL);
    } while (rv == -1 && errno == EINTR);

    assert(rv == 1);
    rv = 0;

out_after_io_setup:
    io_destroy(ctx);

out:
    if (rv != 0) {
        req->status = uv_translate_sys_error(errno);
    } else {
        req->status = event.res;
    }

    return;
}

static void uv__file_write_after_work_cb(uv_work_t *work, int status)
{
    struct uv__file_write *req; /* Write file request object */
    struct uv__file *f;

    assert(work != NULL);

    assert(status == 0); /* We don't cancel worker requests */

    req = work->data;
    f = req->file;

    /* If we were closed, let's mark the request as canceled, regardless of the
     * actual outcome. */
    if (uv__file_is_closing(req->file)) {
        req->status = UV_ECANCELED;
    }

    uv__file_write_finish(req);

    if (uv__file_is_closing(f)) {
        uv__file_maybe_closed(f);
    }
}

static void uv__file_write_finish(struct uv__file_write *req)
{
    RAFT__QUEUE_REMOVE(&req->queue);

    req->cb(req, req->status);
}

static void uv__file_maybe_closed(struct uv__file *f)
{
    assert((f->flags & UV__FILE_CLOSED) == 0);

    /* If are creating the file we need to wait for the create to finish. */
    if (f->flags & UV__FILE_CREATING) {
        return;
    }

    /* If are writing we need to wait for the writes to finish. */
    if (!RAFT__QUEUE_IS_EMPTY(&f->write_queue)) {
        return;
    }

    if (!uv_is_closing((struct uv_handle_s *)&f->event_poller)) {
        uv_close((struct uv_handle_s *)&f->event_poller,
                 uv__file_poll_close_cb);
    }
}

static void uv__file_poll_close_cb(struct uv_handle_s *handle)
{
    struct uv__file *f = handle->data;
    int rv;

    assert((f->flags & UV__FILE_CLOSED) == 0);
    assert(RAFT__QUEUE_IS_EMPTY(&f->write_queue));

    rv = close(f->event_fd);
    assert(rv == 0);

    if (f->ctx != 0) {
        rv = io_destroy(f->ctx);
        assert(rv == 0);
    }

    free(f->events);

    f->flags |= UV__FILE_CLOSED;

    if (f->close_cb != NULL) {
        f->close_cb(f);
    }
}
