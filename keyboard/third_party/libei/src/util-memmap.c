/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Red Hat, Inc.
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

#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

#include "util-mem.h"
#include "util-memmap.h"
#include "util-object.h"

struct memmap {
	struct object object;
	void *data;
	size_t size;
};

static void
memmap_destroy(struct memmap *memmap)
{
	if (memmap->data) {
		munmap(memmap->data, memmap->size);
	}
}

static OBJECT_IMPLEMENT_CREATE(memmap);
OBJECT_IMPLEMENT_UNREF_CLEANUP(memmap);
OBJECT_IMPLEMENT_REF(memmap);
OBJECT_IMPLEMENT_GETTER(memmap, size, size_t);
OBJECT_IMPLEMENT_GETTER(memmap, data, void *);

struct memmap *
memmap_new(int fd, size_t sz)
{
	if (sz == 0)
		return NULL;

	_unref_(memmap) *memmap = memmap_create(NULL);

	void *map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED)
		return NULL;

	memmap->data = map;
	memmap->size = sz;

	return steal(&memmap);
}
