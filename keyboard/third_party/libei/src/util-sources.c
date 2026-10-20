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

#include <assert.h>
#include <errno.h>
#include <sys/epoll.h>

#include "util-io.h"
#include "util-list.h"
#include "util-object.h"
#include "util-sources.h"

struct sink {
	struct object object;
	int epollfd;
	struct list sources;
	struct list sources_removed;
};

enum source_close_behavior {
	SOURCE_CLOSE_FD_ON_REMOVE = 1, /* default */
	SOURCE_CLOSE_FD_ON_DESTROY,
	SOURCE_CLOSE_FD_NEVER,
};

struct source {
	struct object object;
	struct sink *sink;
	struct list link; /* sink.sources or sink.sources_removed */
	source_dispatch_t dispatch;
	void *user_data;
	enum source_close_behavior close_behavior;
	int fd;
	bool is_active;
};

OBJECT_IMPLEMENT_REF(source);
OBJECT_IMPLEMENT_UNREF_CLEANUP(source);
OBJECT_IMPLEMENT_GETTER(source, fd, int);
OBJECT_IMPLEMENT_GETTER(source, user_data, void *);
OBJECT_IMPLEMENT_SETTER(source, user_data, void *);

/**
 * Remove the source, closing the fd. The source is tagged as removed and
 * will be removed whenever sink_dispatch() finishes (or is called next).
 */
void
source_remove(struct source *source)
{
	if (!source || !source->is_active)
		return;

	epoll_ctl(source->sink->epollfd, EPOLL_CTL_DEL, source->fd, NULL);
	if (source->close_behavior == SOURCE_CLOSE_FD_ON_REMOVE)
		source->fd = xclose(source->fd);
	source->is_active = false;
	source_unref(source);

	/* Note: sources list was the owner of the source, new owner
	   is the removed list */
	list_remove(&source->link);
	list_append(&source->sink->sources_removed, &source->link);
	source->sink = NULL;
}

/* Ignore, use source_unref() */
static void
source_destroy(struct source *source)
{
	/* We expect source_remove() to be called before we ever get here */
	assert(!source->is_active);

	if (source->close_behavior == SOURCE_CLOSE_FD_ON_DESTROY)
		source->fd = xclose(source->fd);
}

static OBJECT_IMPLEMENT_CREATE(source);

struct source *
source_new(int sourcefd, source_dispatch_t dispatch, void *user_data)
{
	struct source *source = source_create(NULL);

	source->dispatch = dispatch;
	source->user_data = user_data;
	source->fd = sourcefd;
	source->close_behavior = SOURCE_CLOSE_FD_ON_REMOVE;
	source->is_active = false;
	list_init(&source->link);

	return source;
}

void
source_never_close_fd(struct source *s)
{
	s->close_behavior = SOURCE_CLOSE_FD_NEVER;
}

static void
sink_destroy(struct sink *sink)
{
	struct source *s;
	list_for_each_safe(s, &sink->sources, link) {
		source_remove(s);
	}
	list_for_each_safe(s, &sink->sources_removed, link) {
		source_unref(s);
	}
	xclose(sink->epollfd);
}

OBJECT_IMPLEMENT_UNREF_CLEANUP(sink);
static OBJECT_IMPLEMENT_CREATE(sink);

int
sink_get_fd(struct sink *sink)
{
	assert(sink);
	return sink->epollfd;
}

struct sink *
sink_new(void)
{
	int fd = epoll_create1(EPOLL_CLOEXEC);
	if (fd < 0)
		return NULL;

	struct sink *sink = sink_create(NULL);

	sink->epollfd = fd;
	list_init(&sink->sources);
	list_init(&sink->sources_removed);

	return sink;
}

int
sink_dispatch(struct sink *sink)
{
	struct epoll_event ep[32];
	int count = epoll_wait(sink->epollfd, ep, sizeof(ep) / sizeof(ep[0]), 0);
	if (count < 0)
		return -errno;

	for (int i = 0; i < count; ++i) {
		struct source *source = ep[i].data.ptr;
		if (source->fd == -1)
			continue;

		source->dispatch(source, source->user_data);
	}

	struct source *s;
	list_for_each_safe(s, &sink->sources_removed, link) {
		list_remove(&s->link);
		list_init(&s->link);
		source_unref(s);
	}

	return 0;
}

int
sink_add_source(struct sink *sink, struct source *source)
{
	struct epoll_event e = {
		.events = EPOLLIN,
		.data.ptr = source_ref(source),
	};

	int rc = xerrno(epoll_ctl(sink->epollfd, EPOLL_CTL_ADD, source_get_fd(source), &e));
	if (rc < 0) {
		source_unref(source);
		return rc;
	}

	source->is_active = true;
	source->sink = sink;
	source_ref(source);
	list_append(&sink->sources, &source->link);

	return 0;
}

int
source_enable_write(struct source *source, bool enable)
{
	assert(source->is_active);

	struct epoll_event e = {
		.events = EPOLLIN | (enable ? EPOLLOUT : 0),
		.data.ptr = source, /* sink_add_source ref'd, so we don't need to here */
	};

	int rc = xerrno(epoll_ctl(source->sink->epollfd, EPOLL_CTL_MOD, source_get_fd(source), &e));
	if (rc < 0) {
		source_unref(source);
		return rc;
	}
	return 0;
}

#if _enable_tests_
#include <fcntl.h>
#include <signal.h>

#include "util-macros.h"
#include "util-munit.h"

MUNIT_TEST(test_sink)
{
	struct sink *sink = sink_new();
	munit_assert_ptr_not_null(sink);
	sink_dispatch(sink);
	sink_dispatch(sink);

	int fd = sink_get_fd(sink);
	munit_assert_int(fd, !=, -1);

	sink_unref(sink);

	return MUNIT_OK;
}

struct buffer {
	size_t size;
	size_t len;
	char *buffer;
};

static void
read_buffer(struct source *source, void *user_data)
{
	struct buffer *buffer = user_data;
	size_t sz = max(buffer->size, 1024);

	buffer->size = sz;
	buffer->buffer = xrealloc(buffer->buffer, sz);

	int nread = read(source_get_fd(source), buffer->buffer, sz);
	munit_assert_int(nread, >=, 0);

	buffer->len = nread;
}

MUNIT_TEST(test_source)
{
	_unref_(sink) *sink = sink_new();

	int fd[2];
	int rc = pipe2(fd, O_CLOEXEC | O_NONBLOCK);
	munit_assert_int(rc, !=, -1);

	struct buffer buffer = { 0 };
	struct source *s = source_new(fd[0], read_buffer, &buffer);

	munit_assert_int(source_get_fd(s), ==, fd[0]);

	sink_add_source(sink, s);

	/* Nothing to read yet, dispatch is a noop */
	sink_dispatch(sink);
	munit_assert_int(buffer.len, ==, 0);

	const char token[] = "foobar";
	int wrc = write(fd[1], token, sizeof(token));
	munit_assert_int(wrc, ==, sizeof(token));

	/* haven't called dispatch yet */
	munit_assert_int(buffer.len, ==, 0);
	sink_dispatch(sink);
	munit_assert_int(buffer.len, ==, sizeof(token));
	munit_assert_string_equal(buffer.buffer, token);

	/* multiple removals shouldn't matter */
	source_remove(s);
	source_remove(s);
	sink_dispatch(sink);
	source_remove(s);
	sink_dispatch(sink);

	/* source pipe is already closed */
	signal(SIGPIPE, SIG_IGN);
	const char token2[] = "bazbat";
	wrc = write(fd[1], token2, sizeof(token2));
	munit_assert_int(wrc, ==, -1);
	munit_assert_int(errno, ==, EPIPE);

	sink_dispatch(sink);
	source_unref(s);
	sink_dispatch(sink);

	free(buffer.buffer);

	return MUNIT_OK;
}

static void
drain_data(struct source *source, void *user_data)
{
	char buf[1024] = { 0 };
	read(source_get_fd(source), buf, sizeof(buf));
}

MUNIT_TEST(test_source_readd)
{
	_unref_(sink) *sink = sink_new();

	int fd[2];
	int rc = pipe2(fd, O_CLOEXEC | O_NONBLOCK);
	munit_assert_int(rc, !=, -1);

	_unref_(source) *s = source_new(fd[0], drain_data, NULL);
	sink_add_source(sink, s);
	sink_dispatch(sink);
	/* remove and re-add without calling dispatch */
	source_remove(s);
	sink_add_source(sink, s);
	source_remove(s);

	return MUNIT_OK;
}

static void
count_calls(struct source *source, void *user_data)
{
	unsigned int *arg = user_data;
	*arg = *arg + 1;
}

MUNIT_TEST(test_source_write)
{
	_unref_(sink) *sink = sink_new();

	int fd[2];
	int rc = pipe2(fd, O_CLOEXEC | O_NONBLOCK);
	munit_assert_int(rc, !=, -1);

	int read_fd = fd[0];
	int write_fd = fd[1];

	int dispatch_called = 0;
	_unref_(source) *s = source_new(write_fd, count_calls, &dispatch_called);
	sink_add_source(sink, s);
	sink_dispatch(sink);
	sink_dispatch(sink);
	sink_dispatch(sink);

	munit_assert_uint(dispatch_called, ==, 0);

	source_enable_write(s, true);
	sink_dispatch(sink);
	munit_assert_uint(dispatch_called, ==, 1);
	sink_dispatch(sink);
	munit_assert_uint(dispatch_called, ==, 2);

	/* Fill up the buffer */
	do {
		char buf[4096] = { 0 };
		rc = write(write_fd, buf, sizeof(buf));
	} while (rc != -1);
	munit_assert_int(errno, ==, EAGAIN);

	/* Buffer is full, expect our dispatch to NOT be called */
	sink_dispatch(sink);
	munit_assert_uint(dispatch_called, ==, 2);
	sink_dispatch(sink);
	munit_assert_uint(dispatch_called, ==, 2);

	do {
		char buf[406];
		rc = read(read_fd, buf, sizeof(buf));
	} while (rc != -1);
	munit_assert_int(errno, ==, EAGAIN);

	sink_dispatch(sink);
	munit_assert_uint(dispatch_called, ==, 3);

	source_enable_write(s, false);

	sink_dispatch(sink);
	munit_assert_uint(dispatch_called, ==, 3);

	return MUNIT_OK;
}
#endif
