
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
#include <stdint.h>
#include <time.h>
#include <unistd.h>

/* Merely for code readability, e.g. timeout = us(100); */
static inline uint64_t
us(uint64_t us)
{
	return us;
}

static inline uint64_t
us2ms(uint64_t us)
{
	return us / 1000;
}

static inline uint64_t
ns2us(uint64_t ns)
{
	return us(ns / 1000);
}

static inline uint64_t
ms2us(uint64_t ms)
{
	return us(ms * 1000);
}

static inline uint64_t
s2us(uint64_t s)
{
	return ms2us(s * 1000);
}

static inline int
now(uint64_t *now_out)
{
	struct timespec ts = { 0, 0 };

	assert(now_out != NULL);

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		*now_out = s2us(ts.tv_sec) + ns2us(ts.tv_nsec);
		return 0;
	} else {
		return -errno;
	}
}

static inline void
msleep(int32_t millis)
{
	usleep(ms2us(millis));
}
