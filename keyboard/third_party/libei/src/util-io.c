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

#include "config.h"

#include "util-io.h"
#include "util-macros.h"

_Static_assert(EAGAIN == EWOULDBLOCK, "Currently not supported, please file a bug");

int
xread_with_fds(int fd, void *buf, size_t count, int **fds)
{
	const size_t MAX_FDS = 32;
	char control[CMSG_SPACE(MAX_FDS * sizeof(int))];

	struct iovec iov = {
		.iov_base = buf,
		.iov_len = count,
	};

	struct msghdr msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = sizeof(control),
	};

	int received = xerrno(SYSCALL(recvmsg(fd, &msg, 0)));

	if (received > 0) {
		*fds = NULL;

		_cleanup_free_ int *fd_return = xalloc((MAX_FDS + 1) * sizeof(int));
		size_t idx = 0;

		for (struct cmsghdr *hdr = CMSG_FIRSTHDR(&msg); hdr; hdr = CMSG_NXTHDR(&msg, hdr)) {
			if (hdr->cmsg_level != SOL_SOCKET || hdr->cmsg_type != SCM_RIGHTS)
				continue;

			size_t nfds = (hdr->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			int *fd = (int *)CMSG_DATA(hdr);
			for (size_t i = 0; i < nfds; i++) {
				fd_return[idx++] = *fd;
				fd++;
				if (idx >= MAX_FDS)
					break;
			}
		}
		fd_return[idx] = -1;
		*fds = steal(&fd_return);
	}
	return received;
}

int
xsend_with_fd(int fd, const void *buf, size_t len, int *fds)
{
	size_t nfds = 0;

	for (nfds = 0; fds != NULL && fds[nfds] != -1; nfds++) {
		/* noop */
	}

	if (nfds == 0)
		return xsend(fd, buf, len);

	const size_t MAX_FDS = 32;
	if (nfds > MAX_FDS) {
		return -EINVAL;
	}

	char control[CMSG_SPACE(nfds * sizeof(int))];
	struct cmsghdr *header = (struct cmsghdr *)control;

	memset(control, 0, sizeof(control));

	struct iovec iov = {
		.iov_base = (void *)buf,
		.iov_len = len,
	};

	struct msghdr msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = sizeof(control),
	};

	header->cmsg_len = CMSG_LEN(nfds * sizeof(int));
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	memcpy(CMSG_DATA(CMSG_FIRSTHDR(&msg)), fds, nfds * sizeof(int));

	return xerrno(SYSCALL(sendmsg(fd, &msg, MSG_NOSIGNAL)));
}

/* consider this struct opaque */
struct iobuf {
	size_t sz;
	size_t len;
	uint8_t *data;
	int fds[32];
};

struct iobuf *
iobuf_new(size_t size)
{
	struct iobuf *buf = xalloc(sizeof(*buf));
	uint8_t *data = xalloc(size);

	*buf = (struct iobuf){
		.sz = size,
		.len = 0,
		.data = data,
	};

	int *fd;
	ARRAY_FOR_EACH(buf->fds, fd) {
		*fd = -1;
	}

	return buf;
}

/**
 * The count of data bytes in this buffer.
 */
size_t
iobuf_len(struct iobuf *buf)
{
	return buf->len;
}

/**
 * Drop the first nbytes from the buffer.
 */
void
iobuf_pop(struct iobuf *buf, size_t nbytes)
{
	assert(nbytes <= buf->len);
	if (nbytes == buf->len) {
		buf->len = 0;
	} else {
		memmove(buf->data, buf->data + nbytes, buf->len - nbytes);
		buf->len -= nbytes;
	}
}

/**
 * Pointer to the data bytes. Note that the buffer is considered binary
 * data. The caller must ensure that any strings stored in the buffer are
 * null-terminated.
 *
 * The returned pointer only valid in the immediate scope, any iobuf
 * function may invalidate the pointer.
 */
const uint8_t *
iobuf_data(struct iobuf *buf)
{
	return buf->data;
}

/**
 * Pointer to the first byte after the end of the data bytes.
 *
 * The returned pointer only valid in the immediate scope, any iobuf
 * function may invalidate the pointer.
 */
const uint8_t *
iobuf_data_end(struct iobuf *buf)
{
	return buf->data + buf->len;
}

/**
 * Return the next available file descriptor in this buffer or -1.
 * The fd is removed from this buffer and belongs to the caller.
 */
int
iobuf_take_fd(struct iobuf *buf)
{
	int fd = buf->fds[0];
	if (fd != -1)
		memmove(buf->fds, buf->fds + 1, (ARRAY_LENGTH(buf->fds) - 1) * sizeof(*buf->fds));
	return fd;
}

static inline void
iobuf_resize(struct iobuf *buf, size_t to_size)
{
	uint8_t *newdata = xrealloc(buf->data, to_size);

	buf->data = newdata;
	buf->sz = to_size;
}

static inline void
iobuf_extend(struct iobuf *buf, size_t extra)
{
	size_t newsize = buf->len + extra;
	if (newsize > buf->sz)
		iobuf_resize(buf, newsize);
}

/**
 * Remove the data bytes from the buffer. The caller must free() the data.
 * The buffer state is the same as iobuf_new() after this call.
 */
uint8_t *
iobuf_take_data(struct iobuf *buf)
{
	uint8_t *data = buf->data;

	buf->data = NULL;
	buf->len = 0;
	iobuf_resize(buf, buf->sz);

	return data;
}

/**
 * Append len bytes to the buffer. If the data exceeds the current buffer
 * size it is resized automatically.
 */
void
iobuf_append(struct iobuf *buf, const void *data, size_t len)
{
	if (len == 0)
		return;

	iobuf_extend(buf, len);
	memcpy(buf->data + buf->len, data, len);
	buf->len += len;
}

void
iobuf_append_u32(struct iobuf *buf, uint32_t data)
{
	size_t len = 4;

	iobuf_extend(buf, len);
	memcpy(buf->data + buf->len, &data, len);
	buf->len += len;
}

void
iobuf_append_u64(struct iobuf *buf, uint64_t data)
{
	size_t len = 8;

	iobuf_extend(buf, len);
	memcpy(buf->data + buf->len, &data, len);
	buf->len += len;
}

void
iobuf_append_f32(struct iobuf *buf, float data)
{
	size_t len = 4;

	iobuf_extend(buf, len);
	memcpy(buf->data + buf->len, &data, len);
	buf->len += len;
}

/**
 * Prepend the given data to the buffer.
 */
void
iobuf_prepend(struct iobuf *buf, const void *data, size_t len)
{
	if (len == 0)
		return;

	if (buf->len + len > buf->sz) {
		size_t newsize = buf->len + len;
		iobuf_resize(buf, newsize);
	}
	if (buf->len > 0)
		memmove(buf->data + len, buf->data, buf->len);
	memcpy(buf->data, data, len);
	buf->len += len;
}

int
iobuf_append_fd(struct iobuf *buf, int fd)
{
	/* Array must remain terminated by -1 */
	for (size_t idx = 0; idx < ARRAY_LENGTH(buf->fds) - 1; idx++) {
		if (buf->fds[idx] == -1) {
			int f = dup(fd);
			if (f == -1)
				return -errno;
			buf->fds[idx] = f;
			return 0;
		}
	}

	return -ENOMEM;
}

/**
 * Append all available data from the file descriptor to the pointer. The
 * file descriptor should be in O_NONBLOCK or this call will block. If the
 * data exceeds the current buffer size it is resized automatically.
 *
 * @return The number of bytes read or a negative errno on failure. Zero
 * indicates EOF.
 */
int
iobuf_append_from_fd(struct iobuf *buf, int fd)
{
	char data[1024];
	size_t nread = 0;
	ssize_t rc;
	do {
		rc = xread(fd, data, sizeof(data));
		if (rc == 0 || rc == -EAGAIN) {
			break;
		} else if (rc < 0) {
			return rc;
		}

		iobuf_append(buf, data, rc);
		nread += rc;
	} while (rc == sizeof(data));

	return nread == 0 ? rc : (int)nread;
}

/**
 * Append all available data from the file descriptor to the pointer. The
 * file descriptor should be in O_NONBLOCK or this call will block. If the
 * data exceeds the current buffer size it is resized automatically.
 *
 * Any file descriptors passed through the fd are placed into the struct
 * iobuf's file descriptor array and can be retrieved in-order with
 * iobuf_take_fd().
 *
 * @return The number of bytes read or a negative errno on failure. Zero
 * indicates EOF.
 */
int
iobuf_recv_from_fd(struct iobuf *buf, int fd)
{
	char data[1024];
	size_t nread = 0;
	ssize_t rc;
	do {
		_cleanup_free_ int *fds = NULL;
		rc = xread_with_fds(fd, data, sizeof(data), &fds);
		if (rc == 0 || rc == -EAGAIN) {
			break;
		} else if (rc < 0) {
			return rc;
		}
		iobuf_append(buf, data, rc);

		if (fds) {
			int *fd = fds;
			for (size_t idx = 0; *fd != -1 && idx < ARRAY_LENGTH(buf->fds) - 1; idx++) {
				if (buf->fds[idx] == -1) {
					buf->fds[idx] = *fd;
					fd++;
				}
			}
			/* Close any remaining fds that didn't fit in the buffer
			 * to prevent fd leaks */
			while (*fd != -1) {
				xclose(*fd);
				fd++;
			}
		}

		nread += rc;
	} while (rc == sizeof(data));

	return nread == 0 ? rc : (int)nread;
}

int
iobuf_send(struct iobuf *buf, int fd)
{
	return xsend_with_fd(fd, buf->data, buf->len, buf->fds);
}

/**
 * Release the memory associated with this iobuf. Use iobuf_take_data()
 * prevent the data from being free()d.
 */
struct iobuf *
iobuf_free(struct iobuf *buf)
{
	if (buf) {
		free(buf->data);
		buf->sz = 0;
		buf->len = 0;
		buf->data = NULL;

		int fd;
		while ((fd = iobuf_take_fd(buf)) != -1)
			xclose(fd);
		free(buf);
	}
	return NULL;
}

#if _enable_tests_
#include "util-munit.h"
#include "util-strings.h"

MUNIT_TEST(test_iobuf_new)
{
	/* test allocation and freeing a buffer */
	struct iobuf *buf = iobuf_new(10);
	munit_assert_size(buf->sz, ==, 10);

	munit_assert_size(buf->len, ==, 0);
	munit_assert_size(iobuf_len(buf), ==, 0);

	buf = iobuf_free(buf);
	munit_assert_null(buf);

	return MUNIT_OK;
}

MUNIT_TEST(test_iobuf_cleanup)
{
	/* Test the attribute(cleanup) define. This test needs to run in
	 * valgrind --leak-check=full to be really useful */
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(10);
	_cleanup_iobuf_ struct iobuf *nullbuf = NULL;

	assert(buf);
	assert(nullbuf == NULL);

	return MUNIT_OK;
}

MUNIT_TEST(test_iobuf_take_fd)
{
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(10);
	const size_t nfds = ARRAY_LENGTH(buf->fds);
	int *last_fd = &buf->fds[nfds - 1]; /* always -1 */

	for (size_t i = 0; i < nfds - 1; i++) {
		buf->fds[i] = 10 + i;
	}

	for (size_t i = 0; i < nfds - 1; i++) {
		int fd = iobuf_take_fd(buf);
		munit_assert_int(fd, ==, 10 + i);
		munit_assert_int(*last_fd, ==, -1);
	}

	int fd = iobuf_take_fd(buf);
	munit_assert_int(fd, ==, -1);

	return MUNIT_OK;
}

MUNIT_TEST(test_iobuf_append_prepend)
{
	/* Test appending data */
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(10);

	/* append data without a resize */
	const char data[] = "foo";
	iobuf_append(buf, data, 3);
	size_t expected_size = 3;

	munit_assert_size(buf->len, ==, expected_size);
	munit_assert_size(iobuf_len(buf), ==, expected_size);
	munit_assert_size(buf->sz, ==, 10);

	/* we don't have a trailing \0 */
	const uint8_t *bufdata = iobuf_data(buf);
	munit_assert_char(bufdata[0], ==, 'f');
	munit_assert_char(bufdata[1], ==, 'o');
	munit_assert_char(bufdata[2], ==, 'o');

	/* prepend data without resize */
	const char prepend_data[] = "bar";
	iobuf_prepend(buf, prepend_data, 3);
	expected_size += 3;

	munit_assert_size(buf->len, ==, expected_size);
	munit_assert_size(iobuf_len(buf), ==, expected_size);
	munit_assert_size(buf->sz, ==, 10);

	/* we don't have a trailing \0 */
	bufdata = iobuf_data(buf);
	munit_assert_char(bufdata[0], ==, 'b');
	munit_assert_char(bufdata[1], ==, 'a');
	munit_assert_char(bufdata[2], ==, 'r');
	munit_assert_char(bufdata[3], ==, 'f');
	munit_assert_char(bufdata[4], ==, 'o');
	munit_assert_char(bufdata[5], ==, 'o');

	/* Now append enough data to force a buffer resize */

	const char data2[] = "data forcing resize";
	iobuf_append(buf, data2, sizeof(data2)); /* includes \0 */
	expected_size += sizeof(data2);

	munit_assert_size(iobuf_len(buf), ==, expected_size);
	munit_assert_size(buf->sz, ==, expected_size);
	/* now we have a trailing \0 */
	munit_assert_string_equal((const char *)iobuf_data(buf), "barfoodata forcing resize");

	/* and again with prepending */
	const char prepend_data2[] = "second resize";
	iobuf_prepend(buf, prepend_data2, strlen(prepend_data2)); /* does not include \0 */
	expected_size += strlen(prepend_data2);

	munit_assert_size(iobuf_len(buf), ==, expected_size);
	munit_assert_size(buf->sz, ==, expected_size);
	munit_assert_string_equal((const char *)iobuf_data(buf),
				  "second resizebarfoodata forcing resize");

	return MUNIT_OK;
}

MUNIT_TEST(test_iobuf_append_values)
{
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(10);

	iobuf_append_u32(buf, -1);
	size_t expected_size = 4;

	munit_assert_size(buf->len, ==, expected_size);
	munit_assert_size(iobuf_len(buf), ==, expected_size);
	munit_assert_size(buf->sz, ==, 10);

	const uint8_t *bufdata = iobuf_data(buf);
	munit_assert_int(bufdata[0], ==, 0xff);
	munit_assert_int(bufdata[1], ==, 0xff);
	munit_assert_int(bufdata[2], ==, 0xff);
	munit_assert_int(bufdata[3], ==, 0xff);

	free(iobuf_take_data(buf)); /* drops and replaces current buffer */

	iobuf_append_u64(buf, 0xabababababababab);
	expected_size = 8;

	munit_assert_size(buf->len, ==, expected_size);
	munit_assert_size(iobuf_len(buf), ==, expected_size);
	munit_assert_size(buf->sz, ==, 10);

	bufdata = iobuf_data(buf);
	munit_assert_int((unsigned char)bufdata[0], ==, 0xab);
	munit_assert_int((unsigned char)bufdata[1], ==, 0xab);
	munit_assert_int((unsigned char)bufdata[2], ==, 0xab);
	munit_assert_int((unsigned char)bufdata[3], ==, 0xab);
	munit_assert_int((unsigned char)bufdata[4], ==, 0xab);
	munit_assert_int((unsigned char)bufdata[5], ==, 0xab);
	munit_assert_int((unsigned char)bufdata[6], ==, 0xab);
	munit_assert_int((unsigned char)bufdata[7], ==, 0xab);

	free(iobuf_take_data(buf));

	return MUNIT_OK;
}

MUNIT_TEST(test_iobuf_prepend_empty_buffer)
{
	/* Test prepending data */
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(10);

	const char data[] = "foo";
	iobuf_prepend(buf, data, 3);
	size_t expected_size = 3;

	munit_assert_size(buf->len, ==, expected_size);
	munit_assert_size(iobuf_len(buf), ==, expected_size);
	munit_assert_size(buf->sz, ==, 10);

	/* we don't have a trailing \0 */
	const uint8_t *bufdata = iobuf_data(buf);
	munit_assert_char(bufdata[0], ==, 'f');
	munit_assert_char(bufdata[1], ==, 'o');
	munit_assert_char(bufdata[2], ==, 'o');

	return MUNIT_OK;
}

MUNIT_TEST(test_iobuf_pop)
{
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(10);
	const char data[] = "foobar";
	iobuf_append(buf, data, strlen(data));

	munit_assert_size(iobuf_len(buf), ==, 6);
	iobuf_pop(buf, 3);
	munit_assert_size(iobuf_len(buf), ==, 3);

	/* we don't have a trailing \0 */
	const uint8_t *bufdata = iobuf_data(buf);
	munit_assert_char(bufdata[0], ==, 'b');
	munit_assert_char(bufdata[1], ==, 'a');
	munit_assert_char(bufdata[2], ==, 'r');

	return MUNIT_OK;
}

MUNIT_TEST(test_iobuf_append_short)
{
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(10);

	/* Append only the first few bytes out of a larger data field, i.e.
	 * make sure we honor the length parameter */
	const char data[] = "foobar";
	const char nullbyte = '\0';
	iobuf_append(buf, data, 3);
	iobuf_append(buf, &nullbyte, 1);

	munit_assert_size(buf->len, ==, 4);
	munit_assert_size(iobuf_len(buf), ==, 4);
	munit_assert_size(buf->sz, ==, 10);
	munit_assert_string_equal((const char *)iobuf_data(buf), "foo");

	return MUNIT_OK;
}

MUNIT_TEST(test_iobuf_append_fd)
{
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(10);
	int fds[2];

	int rc = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds);
	munit_assert_int(rc, ==, 0);

	int wr = fds[0], rd = fds[1];

	/* write some data */
	const char data[] = "foobar";
	int wlen = xwrite(wr, data, 4);
	munit_assert_int(wlen, ==, 4);

	/* read that data */
	int rlen = iobuf_append_from_fd(buf, rd);
	munit_assert_int(rlen, ==, 4);

	munit_assert_size(iobuf_len(buf), ==, 4);

	/* so we can do strcmp */
	const char nullbyte = '\0';
	iobuf_append(buf, &nullbyte, 1);
	munit_assert_string_equal((const char *)iobuf_data(buf), "foob");

	/* read when there's nothing waiting */
	int blocking_read = iobuf_append_from_fd(buf, rd);
	munit_assert_int(blocking_read, ==, -EAGAIN);

	const char largebuffer[2048] = { 0xaa };

	/* read data exactly our internal buffer size */
	wlen = xwrite(wr, largebuffer, 1024);
	munit_assert_int(wlen, ==, 1024);
	int read_1024 = iobuf_append_from_fd(buf, rd);
	munit_assert_int(read_1024, ==, 1024);

	/* read data exactly our internal buffer size + 1*/
	wlen = xwrite(wr, largebuffer, 1025);
	munit_assert_int(wlen, ==, 1025);
	int read_1025 = iobuf_append_from_fd(buf, rd);
	munit_assert_int(read_1025, ==, 1025);

	/* close write side, read nothing */
	xclose(wr);
	int read_none = iobuf_append_from_fd(buf, rd);
	munit_assert_int(read_none, ==, 0);

	/* close read side, expect error */
	xclose(rd);
	int read_fail = iobuf_append_from_fd(buf, rd);
	munit_assert_int(read_fail, ==, -EBADF);

	return MUNIT_OK;
}

MUNIT_TEST(test_iobuf_append_fd_too_many)
{
	_cleanup_fclose_ FILE *fp = tmpfile();
	munit_assert_ptr_not_null(fp);
	int fd = fileno(fp);

	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(20);
	const size_t nfds = ARRAY_LENGTH(buf->fds);
	int *last_fd = &buf->fds[nfds - 1]; /* always -1 */
	int err = 0;
	size_t count = 0;

	/* 32 fds hardcoded in the struct, last one is always -1 */
	for (count = 0; err == 0 && count < nfds + 1; count++) {
		err = iobuf_append_fd(buf, fd);
		munit_assert_int(*last_fd, ==, -1);
	}

	munit_assert_int(count, ==, 32);
	munit_assert_int(err, ==, -ENOMEM);

	return MUNIT_OK;
}

MUNIT_TEST(test_iobuf_recv_fd)
{
	int fds[2];
	int rc = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds);
	munit_assert_int(rc, ==, 0);

	_cleanup_close_ int left = fds[0];
	_cleanup_close_ int right = fds[1];
	_cleanup_fclose_ FILE *fp = tmpfile();
	munit_assert_ptr_not_null(fp);

	/* actual message data to be sent */
	char data[] = "some data\n";

	/* Send the fd from left to right */
	_cleanup_iobuf_ struct iobuf *sender = iobuf_new(20);
	iobuf_append(sender, data, sizeof(data));
	iobuf_append_fd(sender, fileno(fp));
	int sendrc = iobuf_send(sender, left);
	munit_assert_int(sendrc, ==, sizeof(data));

	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(64);
	rc = iobuf_recv_from_fd(buf, right);
	munit_assert_int(rc, ==, sizeof(data));

	_cleanup_close_ int fd = iobuf_take_fd(buf);
	munit_assert_int(fd, !=, -1);

	return MUNIT_OK;
}

MUNIT_TEST(test_pass_fd)
{
	int fds[2];
	int rc = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds);
	munit_assert_int(rc, ==, 0);

	_cleanup_close_ int left = fds[0];
	_cleanup_close_ int right = fds[1];
	FILE *fps[4];
	int sendfds[ARRAY_LENGTH(fps) + 1];

	for (size_t idx = 0; idx < ARRAY_LENGTH(fps); idx++) {
		FILE *fp = tmpfile();
		munit_assert_not_null(fp);
		fps[idx] = fp;
		sendfds[idx] = fileno(fp);
		sendfds[idx + 1] = -1;
	}

	/* actual message data to be sent */
	char data[] = "some data\n";

	/* Send the fd from left to right */
	int sendrc = xsend_with_fd(left, data, sizeof(data), sendfds);
	munit_assert_int(sendrc, ==, sizeof(data));

	/* Write some data to the file on its real fd */
	for (size_t idx = 0; idx < ARRAY_LENGTH(fps); idx++) {
		_cleanup_free_ char *buf = xaprintf("foo %zu\n", idx);
		munit_assert_ptr_not_null(buf);
		FILE *fp = fps[idx];
		munit_assert_ptr_not_null(fp);
		fwrite(buf, strlen(buf) + 1, 1, fp);
		fflush(fp);
	}

	/* Receive the fd on the right */
	_cleanup_free_ int *recvfds = NULL;
	char recvbuf[sizeof(data)];
	int recvrc = xread_with_fds(right, recvbuf, sizeof(recvbuf), &recvfds);
	munit_assert_int(recvrc, ==, sizeof(data));
	munit_assert_string_equal(recvbuf, data);
	munit_assert_ptr_not_null(recvfds);
	munit_assert_int(recvfds[0], !=, -1);
	munit_assert_int(recvfds[1], !=, -1);
	munit_assert_int(recvfds[2], !=, -1);
	munit_assert_int(recvfds[3], !=, -1);
	munit_assert_int(recvfds[4], ==, -1);

	/* Now check that we can read "foo N" from the passed fd */
	for (size_t idx = 0; idx < ARRAY_LENGTH(fps); idx++) {
		_cleanup_close_ int passed_fd = recvfds[idx];
		off_t off = lseek(passed_fd, 0, SEEK_SET);
		munit_assert_int(off, ==, 0);
		char readbuf[64];
		int readrc = xread(passed_fd, readbuf, sizeof(readbuf));

		_cleanup_free_ char *expected = xaprintf("foo %zu\n", idx);
		munit_assert_int(readrc, ==, strlen(expected) + 1);
		munit_assert_string_equal(readbuf, expected);

		/* cleanup */
		FILE *fp = fps[idx];
		fclose(fp);
	}

	return MUNIT_OK;
}

static inline void
sigblock_helper(void)
{
	with_signals_blocked(SIGPIPE, SIGALRM)
	{
		break; /* breaking out of loop must clean up too */
	}
}

MUNIT_TEST(test_signal_blocker)
{
	int rc;
	sigset_t mask;
	int count = 0;

	with_signals_blocked(SIGPIPE, SIGALRM)
	{
		rc = sigprocmask(SIG_BLOCK, NULL, &mask);
		munit_assert_int(rc, !=, -1);

		munit_assert(sigismember(&mask, SIGPIPE));
		munit_assert(sigismember(&mask, SIGALRM));
		munit_assert(!sigismember(&mask, SIGINT)); /* We didn't touch that one */

		++count;
	}
	munit_assert_int(count, ==, 1); /* loop body only entered once */

	rc = sigprocmask(SIG_BLOCK, NULL, &mask);
	munit_assert_int(rc, !=, -1);

	munit_assert(!sigismember(&mask, SIGPIPE));
	munit_assert(!sigismember(&mask, SIGALRM));

	sigblock_helper();

	rc = sigprocmask(SIG_BLOCK, NULL, &mask);
	munit_assert_int(rc, !=, -1);

	munit_assert(!sigismember(&mask, SIGPIPE));
	munit_assert(!sigismember(&mask, SIGALRM));

	return MUNIT_OK;
}

#endif
