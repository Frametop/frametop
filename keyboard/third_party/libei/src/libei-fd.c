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

#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "util-macros.h"
#include "util-mem.h"
#include "util-object.h"
#include "util-sources.h"
#include "util-strings.h"

#include "libei-private.h"
#include "libei.h"

/**
 * This is the simplest backend: the caller prepares a socket and we just
 * take that and use it.
 */
struct ei_fd {
	struct object object;
};

static inline void
ei_fd_destroy(struct ei_fd *backend)
{
	/* nothing to do here */
}

static OBJECT_IMPLEMENT_CREATE(ei_fd);
static OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_fd);

static void
interface_fd_destroy(struct ei *ei, void *backend)
{
	struct ei_fd *ei_fd = backend;
	ei_fd_unref(ei_fd);
}

static const struct ei_backend_interface interface = {
	.destroy = interface_fd_destroy,
};

_public_ int
ei_setup_backend_fd(struct ei *ei, int fd)
{
	assert(ei);
	assert(!ei->backend);

	struct ei_fd *ei_fd = ei_fd_create(&ei->object);

	ei->backend = ei_fd;
	ei->backend_interface = interface;

	return ei_set_socket(ei, fd);
}
