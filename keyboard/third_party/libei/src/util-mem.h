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
#include <stdlib.h>

/**
 * Use: _cleanup_(somefunction) struct foo *bar;
 */
#define _cleanup_(_x) __attribute__((cleanup(_x)))

/**
 * Use: _cleanup_unref_(foo) struct foo *bar;
 *
 * This requires foo_unrefp() to be present, use DEFINE_UNREF_CLEANUP_FUNC.
 */
#define _unref_(_type) __attribute__((cleanup(_type##_unrefp))) struct _type

static inline void
_free_ptr_(void *p)
{
	free(*(void **)p);
}
/**
 * Use: _cleanup_free_ char *data;
 */
#define _cleanup_free_ _cleanup_(_free_ptr_)

/**
 * Use:
 * DEFINE_TRIVIAL_CLEANUP_FUNC(struct foo *, foo_unref)
 * _cleanup_(foo_unrefp) struct foo *bar;
 */
#define DEFINE_TRIVIAL_CLEANUP_FUNC(_type, _func)		\
	static inline void _func##p(_type *_p) {		\
		if (*_p)					\
			_func(*_p);				\
	}							\
	struct __useless_struct_to_allow_trailing_semicolon__

/**
 * Define a cleanup function for the struct type foo with a matching
 * foo_unref(). Use:
 * DEFINE_UNREF_CLEANUP_FUNC(foo)
 * _unref_(foo) struct foo *bar;
 */
#define DEFINE_UNREF_CLEANUP_FUNC(_type)		\
	static inline void _type##_unrefp(struct _type **_p) {	\
		if (*_p)					\
			_type##_unref(*_p);			\
	}							\
	struct __useless_struct_to_allow_trailing_semicolon__

static inline void *
_steal(void *ptr)
{
	void **original = (void **)ptr;
	void *swapped = *original;
	*original = NULL;
	return swapped;
}

/**
 * Resets the pointer content and resets the data to NULL.
 * This circumvents _cleanup_ handling for that pointer.
 * Use:
 *   _cleanup_free_ char *data = malloc();
 *   return steal(&data);
 *
 */
#define steal(ptr_) \
  (typeof(*ptr_))_steal(ptr_)

/**
 * Resets the pointer content to -1 and returns
 * the original value.
 */
static inline int
steal_fd(int *ptr)
{
	if (ptr) {
		int original = *ptr;
		*ptr = -1;
		return original;
	}
	return -1;
}

/**
 * Never-failing calloc with a size limit check.
 */
static inline void *
xalloc(size_t size)
{
	void *p;

	/* We never need to alloc anything more than 1.5 MB so we can assume
	 * if we ever get above that something's going wrong */
	if (size > 1536 * 1024)
		assert(!"bug: internal malloc size limit exceeded");

	p = calloc(1, size);
	if (!p)
		abort();

	return p;
}

static inline void *
xrealloc(void *ptr, size_t size)
{
	void *tmp = realloc(ptr, size);
	assert(tmp);
	return tmp;
}
