/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "util-mem.h"
#include "util-strings.h"

/**
 * Blocks the zero-terminated list of signals
 *
 * @return the previous set of signals to pass to signals_release()
 */
static inline sigset_t
signals_block(int signal, ...)
{
	sigset_t old_mask;
	sigset_t new_mask;
	va_list sigs;
	va_start(sigs, signal);
	int sigcount = 0;

	sigemptyset(&new_mask);

	while (signal != 0) {
		sigaddset(&new_mask, signal);
		signal = va_arg(sigs, int);

		assert(++sigcount < 16); /* likely missing zero-terminator */
	}
	sigprocmask(SIG_BLOCK, &new_mask, &old_mask);

	va_end(sigs);

	return old_mask;
}

/**
 * Release signals and revert back to the old sigmask
 *
 * @param mask: The previous set of signals as returned by signals_block().
 * @return Always 0
 */
static inline int
signals_release(sigset_t mask)
{
	sigprocmask(SIG_SETMASK, &mask, NULL);
	return 0;
}

/**
 * Python-like context manager for blocking signals, e.g.
 *
 * void foo(void) {
 *     ...
 *     with_signals_blocked(SIGALARM, SIGINT) {
 *          do_something();
 *     }
 *     // signals are unblocked again.
 */
#define with_signals_blocked(...) \
    for (_cleanup_(signal_blocked_destroy) struct _sigblock b_ = { .mask = signals_block(__VA_ARGS__, 0), .is_blocked = true };  \
	 b_.is_blocked; \
	 b_.is_blocked = false)

struct _sigblock {
	sigset_t mask;
	bool is_blocked;
};

static inline void
signal_blocked_destroy(struct _sigblock *s)
{
	signals_release(s->mask);
}

/**
 * Wrapper to convert an errno-setting syscall into a
 * value-or-negative-errno.
 *
 * Use: int rc = xerrno(foo(bar));
 */
static inline int
xerrno(int value)
{
	return value < 0 ? -errno : value;
}

/**
 * Wrapper around close() that blocks the SIGALRM signal. It checks for
 * fd != -1 to satisfy coverity and friends and always returns -1.
 */
static inline int
xclose(int fd)
{
	if (fd > -1) {
		/* Not SYSCALL(), see libei MR!261#note_2131802 */
		close(fd);
	}

	return -1;
}

DEFINE_TRIVIAL_CLEANUP_FUNC(int, xclose);
#define _cleanup_close_ _cleanup_(xclosep)

DEFINE_TRIVIAL_CLEANUP_FUNC(FILE *, fclose);
#define _cleanup_fclose_ _cleanup_(fclosep)

/**
 * Wrapper around read() that blocks the SIGALRM signal.
 * Returns the number of bytes read or a negative  errno on failure.
 */
static inline int
xread(int fd, void *buf, size_t count)
{
	return xerrno(SYSCALL(read(fd, buf, count)));
}

/**
 * Wrapper around read() that blocks the SIGALRM signal.
 * Returns the number of bytes read or a negative errno on failure.
 * Any fds passed along with the message are stored in the -1-terminated
 * allocated fds array, to be freed by the caller. Where no fds were
 * passed, the array is NULL.
 */
int
xread_with_fds(int fd, void *buf, size_t count, int **fds);

/**
 * Wrapper around write() that blocks the SIGALRM signal.
 * Returns the number of bytes written or a negative errno on failure.
 */
static inline int
xwrite(int fd, const void *buf, size_t count)
{
	return xerrno(SYSCALL(write(fd, buf, count)));
}

/**
 * Wrapper around send() that always sets MSG_NOSIGNAL and blocks the
 * SIGALRM signal.
 * Returns the number of bytes written or a negative errno on failure.
 */
static inline int
xsend(int fd, const void *buf, size_t len)
{
	return xerrno(SYSCALL(send(fd, buf, len, MSG_NOSIGNAL)));
}

/**
 * Wrapper around pipe2() that always blocks the SIGALRM signal.
 */
static inline int
xpipe2(int pipefd[2], int flags)
{
	return SYSCALL(pipe2(pipefd, flags));
}

/**
 * Wrapper around dup() that always blocks the SIGALRM signal.
 */
static inline int
xdup(int fd)
{
	return SYSCALL(dup(fd));
}

/**
 * Wrapper around send() that always sets MSG_NOSIGNAL and allows appending
 * file descriptors to the message.
 *
 * @param fds Array of file descriptors, terminated by -1.
 */
int
xsend_with_fd(int fd, const void *buf, size_t len, int *fds);

/**
 * Connect to the socket in the given path. Returns an fd or a negative
 * errno on failure.
 */
static inline int
xconnect(const char *path)
{
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
		.sun_path = { 0 },
	};
	if (!xsnprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path))
		return -EINVAL;

	_cleanup_close_ int sockfd =
		xerrno(SYSCALL(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0)));
	if (sockfd < 0)
		return sockfd;

	int rc = xerrno(connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)));
	if (rc < 0)
		return rc;

	return steal_fd(&sockfd);
}

/**
 * Create a new iobuf structure with the given size.
 */
struct iobuf *
iobuf_new(size_t size);

/**
 * The count of data bytes in this buffer.
 */
size_t
iobuf_len(struct iobuf *buf);

/**
 * Pointer to the data bytes. Note that the buffer is considered binary
 * data. The caller must ensure that any strings stored in the buffer are
 * null-terminated.
 *
 * The returned pointer only valid in the immediate scope, any iobuf
 * function may invalidate the pointer.
 */
const uint8_t *
iobuf_data(struct iobuf *buf);

/**
 * Pointer to the first byte after the end of the data bytes.
 *
 * The returned pointer only valid in the immediate scope, any iobuf
 * function may invalidate the pointer.
 */
const uint8_t *
iobuf_data_end(struct iobuf *buf);

/**
 * Return the next available file descriptor in this buffer or -1.
 * The fd is removed from this buffer and belongs to the caller.
 */
int
iobuf_take_fd(struct iobuf *buf);

/**
 * Remove the data bytes from the buffer. The caller must free() the data.
 * The buffer state is the same as iobuf_new() after this call.
 */
uint8_t *
iobuf_take_data(struct iobuf *buf);

/**
 * Drop the first nbytes from the buffer.
 */
void
iobuf_pop(struct iobuf *buf, size_t nbytes);

/**
 * Append len bytes to the buffer. If the data exceeds the current buffer
 * size it is resized automatically.
 */
void
iobuf_append(struct iobuf *buf, const void *data, size_t len);

/**
 * Append one 32-bit value to the buffer. If the data exceeds the current buffer
 * size it is resized automatically.
 */
void
iobuf_append_u32(struct iobuf *buf, uint32_t data);

/**
 * Append one 64-bit value to the buffer. If the data exceeds the current buffer
 * size it is resized automatically.
 */
void
iobuf_append_u64(struct iobuf *buf, uint64_t data);

/**
 * Append one 32-bit float to the buffer. If the data exceeds the current buffer
 * size it is resized automatically.
 */
void
iobuf_append_f32(struct iobuf *buf, float data);

/**
 * Prepend len bytes to the buffer. If the data exceeds the current buffer
 * size it is resized automatically.
 */
void
iobuf_prepend(struct iobuf *buf, const void *data, size_t len);

/**
 * Append a file descriptor to the buffer. The file descriptor is dup()ed.
 *
 * Returns zero on success or a negative errno on failure.
 */
int
iobuf_append_fd(struct iobuf *buf, int fd);

/**
 * Append all available data from the file descriptor to the pointer. The
 * file descriptor should be in O_NONBLOCK or this call will block. If the
 * data exceeds the current buffer size it is resized automatically.
 *
 * @return The number of bytes read or a negative errno on failure. Zero
 * indicates EOF.
 */
int
iobuf_append_from_fd(struct iobuf *buf, int fd);

/**
 * Append all available data from the file descriptor to the pointer. The
 * file descriptor should be in O_NONBLOCK or this call will block. If the
 * data exceeds the current buffer size it is resized automatically.
 *
 * Any file descriptors passed through the fd are placed
 *
 * @return The number of bytes read or a negative errno on failure. Zero
 * indicates EOF.
 */
int
iobuf_recv_from_fd(struct iobuf *buf, int fd);

/**
 * Send the data in the buffer (with fds, if need be) and return
 * the number of bytes sent or a negative errno on failure.
 */
int
iobuf_send(struct iobuf *buf, int fd);

/**
 * Release the memory associated with this iobuf. Use iobuf_take_data()
 * prevent the data from being free()d.
 */
struct iobuf *
iobuf_free(struct iobuf *buf);

static inline void
_iobuf_cleanup(struct iobuf **buf)
{
	iobuf_free(*buf);
}
/**
 * Helper macro to auto-free a iobuf struct when the variable goes out of
 * scope. Use like this:
 *    _cleanup_iobuf_ struct iobuf *foo = iobuf_new(64);
 */
#define _cleanup_iobuf_ __attribute__((cleanup(_iobuf_cleanup)))
