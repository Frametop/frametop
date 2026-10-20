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

#include <stdio.h>
#include <unistd.h>

#ifndef HAVE_C23_AUTO
#define auto __auto_type
#endif

#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])
#define ARRAY_FOR_EACH(_arr, _elem) \
	for (size_t _i = 0; _i < ARRAY_LENGTH(_arr) && (_elem = &_arr[_i]); _i++)
/**
 * Use to stringify a switch case value:
 * switch (foo) {
 * CASE_RETURN_STRING(ENUMVALUE);
 * CASE_RETURN_STRING(OTHERVALUE);
 * }
 */
#define CASE_RETURN_STRING(_x) case _x: return #_x

#define SYSCALL(call) ({ \
	int rc_; \
	do { rc_ = call; } while (rc_ == -1 && errno == EINTR); \
	rc_; })

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

#define ANSI_UP			"\x1B[%dA"
#define ANSI_DOWN		"\x1B[%dB"
#define ANSI_RIGHT		"\x1B[%dC"
#define ANSI_LEFT		"\x1B[%dD"

#define _public_ __attribute__((visibility("default")))
#define _printf_(_a, _b) __attribute__((format (printf, _a, _b)))
#define _fallthrough_ __attribute__((fallthrough))
#define _unused_ __attribute__((unused))
#define _packed_ __attribute__((packed))

#define run_only_once \
	static int _once_per_##__func__ = 0; \
	for (; _once_per_##__func__ == 0; _once_per_##__func__ = 1)

#if !defined(trace) && !defined(etrace)
#define trace(...) \
	do { \
		char buf_[1024]; \
		snprintf(buf_, sizeof(buf_), __VA_ARGS__); \
		printf("\x1B[0;34m" "%s():%d - " "\x1B[0;31m" "%s" "\x1B[0m" "\n", __func__, __LINE__, buf_); \
	} while (0)

#define etrace(...) \
	do { \
		char buf_[1024]; \
		snprintf(buf_, sizeof(buf_), __VA_ARGS__); \
		fprintf(stderr, "\x1B[0;34m" "%s():%d - " "\x1B[0;31m" "%s" "\x1B[0m" "\n", __func__, __LINE__, buf_); \
	} while (0)
#endif
