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

#include "util-io.h"
#include "util-macros.h"
#include "util-mem.h"
#include "util-object.h"
#include "util-sources.h"
#include "util-strings.h"

#include "libei-private.h"
#include "libei.h"

struct ei_socket {
	struct object object;
};

static inline void
ei_socket_destroy(struct ei_socket *socket)
{
}

static OBJECT_IMPLEMENT_CREATE(ei_socket);
static OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_socket);

static void
interface_socket_destroy(struct ei *ei, void *backend)
{
	struct ei_socket *socket = backend;
	ei_socket_unref(socket);
}

static const struct ei_backend_interface interface = {
	.destroy = interface_socket_destroy,
};

_public_ int
ei_setup_backend_socket(struct ei *ei, const char *socketpath)
{
	assert(ei);
	assert(!ei->backend);

	struct ei_socket *ei_socket = ei_socket_create(&ei->object);

	ei->backend = ei_socket;
	ei->backend_interface = interface;

	if (!socketpath)
		socketpath = getenv("LIBEI_SOCKET");

	if (!socketpath || socketpath[0] == '\0')
		return -ENOENT;

	_cleanup_free_ char *path = NULL;
	if (socketpath[0] == '/') {
		path = xstrdup(socketpath);
	} else {
		const char *xdg = getenv("XDG_RUNTIME_DIR");
		if (!xdg)
			return -ENOTDIR;
		path = xaprintf("%s/%s", xdg, socketpath);
	}

	int sockfd = xconnect(path);
	if (sockfd < 0)
		return sockfd;

	return ei_set_socket(ei, sockfd);
}
