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

#if HAVE_MEMFD_CREATE
#include <fcntl.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

#include "util-io.h"
#include "util-mem.h"
#include "util-memfile.h"
#include "util-object.h"

struct memfile {
	struct object object;
	int fd;
	size_t size;
};

static void
memfile_destroy(struct memfile *memfile)
{
	if (memfile->fd != -1)
		close(memfile->fd);
}

static OBJECT_IMPLEMENT_CREATE(memfile);
OBJECT_IMPLEMENT_UNREF_CLEANUP(memfile);
OBJECT_IMPLEMENT_REF(memfile);
OBJECT_IMPLEMENT_GETTER(memfile, fd, int);
OBJECT_IMPLEMENT_GETTER(memfile, size, size_t);

struct memfile *
memfile_new(const char *data, size_t sz)
{
	_unref_(memfile) *memfile = memfile_create(NULL);

	_cleanup_close_ int fd = memfd_create("memfile", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0)
		return NULL;

	if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) < 0)
		return NULL;

	int rc;
	with_signals_blocked(SIGALRM)
	{
		rc = SYSCALL(posix_fallocate(fd, 0, sz));
	}
	if (rc < 0)
		return NULL;

	memfile->fd = fd;
	memfile->size = sz;
	fd = -1;

	void *map = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, memfile->fd, 0);
	if (map == MAP_FAILED)
		return NULL;

	memcpy(map, data, sz);
	munmap(map, sz);

	return steal(&memfile);
}
#endif
