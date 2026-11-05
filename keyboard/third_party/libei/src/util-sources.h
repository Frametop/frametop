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

/**
 * Source/sink objects.
 */

#pragma once

#include "config.h"

#include <stdbool.h>

struct source;
struct sink;

/**
 * Callback invoked when the source has data available. userdata is the data
 * provided to source_add().
 *
 * If source_enable_write() was called, this dispatch function is also called
 * when writes are possible (and/or data is available to read at the same time).
 */
typedef void (*source_dispatch_t)(struct source *source, void *user_data);

/**
 * Remove source from its sink without destroying it, a source may be
 * re-added to a sink later.
 */
void
source_remove(struct source *source);

struct source *
source_ref(struct source *source);

/**
 * Unref source. When the last reference is dropped, resources
 * are released.
 *
 * Note that due to implementation details, it is not possible to get the
 * refcount to zero by calling source_unref() in the caller, you *must*
 * remove a source with source_remove() to be able to release it fully.
 */
struct source *
source_unref(struct source *source);

int
source_get_fd(struct source *source);

void *
source_get_user_data(struct source *source);

void
source_set_user_data(struct source *source, void *user_data);

/**
 * Create a new source for the given file descriptor with the given dispatch
 * callback. The source's default behavior is that the fd is closed on the
 * call to source_remove().
 *
 * This source does not generate events until added to a sink with
 * sink_add_source().
 *
 * The returned source has a refcount of 1, use source_unref() to release th
 * memory.
 */
struct source *
source_new(int fd, source_dispatch_t dispatch, void *user_data);

void
source_never_close_fd(struct source *s);

/**
 * Enable or disable write notifications on this source. By default we assume
 * our sources only read from the fd and thus their dispatch is only called
 * when there's data available to read.
 *
 * If write is enabled, the dispatch is also called with data available to write.
 */
int
source_enable_write(struct source *source, bool enable);

struct sink *
sink_new(void);

struct sink *
sink_unref(struct sink *sink);

int
sink_dispatch(struct sink *sink);

/**
 * Add the source to the given sink. Use source_remove() to remove the
 * source.
 */
int
sink_add_source(struct sink *sink, struct source *source);

/**
 * The epollfd to monitor for this sink.
 */
int
sink_get_fd(struct sink *sink);
