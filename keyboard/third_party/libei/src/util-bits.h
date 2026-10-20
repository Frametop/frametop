/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2013-2015 Red Hat, Inc.
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
#include <stdbool.h>
#include <stddef.h>

#define bit(x_) (1UL << (x_))
#define NBITS(b) (b * 8)
#define LONG_BITS (sizeof(long) * 8)
#define NLONGS(x) (((x) + LONG_BITS - 1) / LONG_BITS)
#define NCHARS(x) ((size_t)(((x) + 7) / 8))

#define flag_fits(mask_, b_) !!((unsigned long)(b_) < (unsigned long)NBITS(sizeof(mask_)))
#define flag_is_set(mask_, b_) (flag_fits(mask_, b_) && !!((mask_) & bit(b_)))
#define flag_set(mask_, b_) do { if (flag_fits(mask_, b_)) { (mask_) |= bit(b_); } } while(0)
#define flag_clear(mask_, b_) do { if (flag_fits(mask_, b_)) { (mask_) &= ~bit(b_); } } while(0)

/** Add all bits in m_ to the existing mask */
#define mask_add(mask_, m_) (mask_) |= (m_)
/** Remove all bits in m_ from the existing mask */
#define mask_remove(mask_, m_) (mask_) &= ~(m_)
/** True if any of the bits in m_ are set in mask */
#define mask_any(mask_, m_) (((mask_) & (m_)) != 0)
/** True if all of the bits in m_ are set in mask */
#define mask_all(mask_, m_) (((mask_) & (m_)) == (m_))
/** True if none of the bits in m_ are set in mask */
#define mask_none(mask_, m_) (((mask_) & (m_)) == 0)

/* This bitfield helper implementation is taken from from libevdev-util.h,
 * except that it has been modified to work with arrays of unsigned chars
 */

static inline bool
bit_is_set(const unsigned char *array, int bit)
{
	return !!(array[bit / 8] & (1 << (bit % 8)));
}

static inline void
set_bit(unsigned char *array, int bit)
{
	array[bit / 8] |= (1 << (bit % 8));
}

static inline void
clear_bit(unsigned char *array, int bit)
{
	array[bit / 8] &= ~(1 << (bit % 8));
}

static inline bool
long_bit_is_set(const unsigned long *array, int bit)
{
	return !!(array[bit / LONG_BITS] & (1ULL << (bit % LONG_BITS)));
}

static inline void
long_set_bit(unsigned long *array, int bit)
{
	array[bit / LONG_BITS] |= (1ULL << (bit % LONG_BITS));
}

static inline void
long_clear_bit(unsigned long *array, int bit)
{
	array[bit / LONG_BITS] &= ~(1ULL << (bit % LONG_BITS));
}

static inline void
long_set_bit_state(unsigned long *array, int bit, int state)
{
	if (state)
		long_set_bit(array, bit);
	else
		long_clear_bit(array, bit);
}

static inline bool
long_any_bit_set(unsigned long *array, size_t size)
{
	unsigned long i;

	assert(size > 0);

	for (i = 0; i < size; i++)
		if (array[i] != 0)
			return true;
	return false;
}
