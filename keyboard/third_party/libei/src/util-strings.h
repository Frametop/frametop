/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2008 Kristian Høgsberg
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
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif
#ifdef HAVE_XLOCALE_H
#include <xlocale.h>
#endif

#if defined(__DragonFly__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "util-macros.h"
#include "util-mem.h"

#define streq(s1, s2) (strcmp((s1), (s2)) == 0)
#define strneq(s1, s2, n) (strncmp((s1), (s2), (n)) == 0)

/**
 * Returns the length of the string including the trailing zero.
 */
static inline size_t
strlen0(const char *str)
{
	return str ? strlen(str) + 1 : 0;
}

/**
 * strdup guaranteed to succeed. If the input string is NULL, the output
 * string is NULL. If the input string is a string pointer, we strdup or
 * abort on failure.
 */
static inline char *
xstrdup(const char *str)
{
	char *s;

	if (!str)
		return NULL;

	s = strdup(str);
	if (!s)
		abort();
	return s;
}

static inline bool _printf_(3, 0)
xvsnprintf(char *buf, size_t sz, const char *format, va_list ap)
{
	int rc = vsnprintf(buf, sz, format, ap);

	return rc >= 0 && (size_t)rc < sz;
}

static inline bool _printf_(3, 4)
xsnprintf(char *buf, size_t sz, const char *format, ...)
{
	va_list ap;
	int rc;

	va_start(ap, format);
	rc = xvsnprintf(buf, sz, format, ap);
	va_end(ap);

	return rc;
}

static inline char * _printf_(1, 0)
xvaprintf(const char *fmt, va_list args)
{
	char *str;
	int len;

	len = vasprintf(&str, fmt, args);

	if (len == -1)
		return NULL;

	return str;
}
/**
 * A version of asprintf that returns the allocated string or NULL on error.
 */
static inline char * _printf_(1, 2)
xaprintf(const char *fmt, ...)
{
	va_list args;
	char *str;

	va_start(args, fmt);
	str = xvaprintf(fmt, args);
	va_end(args);

	return str;
}

static inline bool
xatoi_base(const char *str, int *val, int base)
{
	char *endptr;
	long v;

	assert(base == 10 || base == 16 || base == 8);

	errno = 0;
	v = strtol(str, &endptr, base);
	if (errno > 0)
		return false;
	if (str == endptr)
		return false;
	if (*str != '\0' && *endptr != '\0')
		return false;

	if (v > INT_MAX || v < INT_MIN)
		return false;

	*val = v;
	return true;
}

static inline bool
xatoi(const char *str, int *val)
{
	return xatoi_base(str, val, 10);
}

static inline bool
xatou_base(const char *str, unsigned int *val, int base)
{
	char *endptr;
	unsigned long v;

	assert(base == 10 || base == 16 || base == 8);

	errno = 0;
	v = strtoul(str, &endptr, base);
	if (errno > 0)
		return false;
	if (str == endptr)
		return false;
	if (*str != '\0' && *endptr != '\0')
		return false;

	if (v > UINT_MAX)
		return false;

	*val = v;
	return true;
}

static inline bool
xatou(const char *str, unsigned int *val)
{
	return xatou_base(str, val, 10);
}

static inline bool
xatod(const char *str, double *val)
{
	char *endptr;
	double v;
#ifdef HAVE_LOCALE_H
	locale_t c_locale;
#endif
	size_t slen = strlen(str);

	/* We don't have a use-case where we want to accept hex for a double
	 * or any of the other values strtod can parse */
	for (size_t i = 0; i < slen; i++) {
		char c = str[i];

		if (isdigit(c))
			continue;
		switch (c) {
		case '+':
		case '-':
		case '.':
			break;
		default:
			return false;
		}
	}

#ifdef HAVE_LOCALE_H
	/* Create a "C" locale to force strtod to use '.' as separator */
	c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
	if (c_locale == (locale_t)0)
		return false;

	errno = 0;
	v = strtod_l(str, &endptr, c_locale);
	freelocale(c_locale);
#else
	/* No locale support in provided libc, assume it already uses '.' */
	errno = 0;
	v = strtod(str, &endptr);
#endif
	if (errno > 0)
		return false;
	if (str == endptr)
		return false;
	if (*str != '\0' && *endptr != '\0')
		return false;
	if (v != 0.0 && !isnormal(v))
		return false;

	*val = v;
	return true;
}

size_t
strv_len(char **strv);
/* Takes ownership of the string and appends it to strv, s is set to NULL */
char **
strv_append_take(char **strv, char **s);
char **
strv_append_strdup(char **strv, const char *s);
char **
strv_from_string(const char *string, const char *separator);
char *
strv_join(char **strv, const char *separator);

bool
strv_find(char **strv, const char *needle, size_t *index_out);
bool
strv_find_substring(char **strv, const char *needle, size_t *index_out);

static inline void
strv_free(char **strv)
{
	char **s = strv;

	if (!strv)
		return;

	while (*s != NULL) {
		free(*s);
		*s = (char *)0x1; /* detect use-after-free */
		s++;
	}

	free(strv);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(char **, strv_free);

char *
strreplace(const char *string, const char *separator, const char *replacement);

/**
 * Creates a list of strings representing the buffer similar to hexdump, i.e.
 * a buffer of [0, 1, 2, 3, ...] with a stride of 4 is split into
 * the strv of ["00 01 02 03", "04 05 06 07", ...].
 */
char **
strv_from_mem(const uint8_t *buffer, size_t sz, size_t stride);

struct key_value_str {
	char *key;
	char *value;
};

struct key_value_double {
	double key;
	double value;
};

static inline ssize_t
kv_double_from_string(const char *string,
		      const char *pair_separator,
		      const char *kv_separator,
		      struct key_value_double **result_out)

{
	char **pairs;
	char **pair;
	struct key_value_double *result = NULL;
	ssize_t npairs = 0;
	unsigned int idx = 0;

	if (!pair_separator || pair_separator[0] == '\0' || !kv_separator ||
	    kv_separator[0] == '\0')
		return -1;

	pairs = strv_from_string(string, pair_separator);
	if (!pairs)
		return -1;

	for (pair = pairs; *pair; pair++)
		npairs++;

	if (npairs == 0)
		goto error;

	result = xalloc(npairs * sizeof *result);

	for (pair = pairs; *pair; pair++) {
		char **kv = strv_from_string(*pair, kv_separator);
		double k, v;

		if (!kv || !kv[0] || !kv[1] || kv[2] || !xatod(kv[0], &k) || !xatod(kv[1], &v)) {
			strv_free(kv);
			goto error;
		}

		result[idx].key = k;
		result[idx].value = v;
		idx++;

		strv_free(kv);
	}

	strv_free(pairs);

	*result_out = result;

	return npairs;

error:
	strv_free(pairs);
	free(result);
	return -1;
}

/**
 * Strip any of the characters in what from the beginning and end of the
 * input string.
 *
 * @return a newly allocated string with none of "what" at the beginning or
 * end of string
 */
static inline char *
strstrip(const char *input, const char *what)
{
	char *str, *last;

	str = xstrdup(&input[strspn(input, what)]);
	if (!str)
		return NULL;

	last = str;

	for (char *c = str; *c != '\0'; c++) {
		if (!strchr(what, *c))
			last = c + 1;
	}

	*last = '\0';

	return str;
}

/**
 * Return true if str ends in suffix, false otherwise. If the suffix is the
 * empty string, strendswith() always returns false.
 */
static inline bool
strendswith(const char *str, const char *suffix)
{
	size_t slen = strlen(str);
	size_t suffixlen = strlen(suffix);
	size_t offset;

	if (slen == 0 || suffixlen == 0 || suffixlen > slen)
		return false;

	offset = slen - suffixlen;
	return strneq(&str[offset], suffix, suffixlen);
}

static inline bool
strstartswith(const char *str, const char *prefix)
{
	size_t prefixlen = strlen(prefix);

	return prefixlen > 0 ? strneq(str, prefix, prefixlen) : false;
}

/**
 * Return the content of /proc/$pid/cmdline as newly allocated string.
 */
static inline char *
cmdline_as_str(void)
{
#ifdef KERN_PROC_ARGS
	int mib[] = {
		CTL_KERN,
#if defined(__NetBSD__) || defined(__OpenBSD__)
		KERN_PROC_ARGS,
		getpid(),
		KERN_PROC_ARGV,
#else
		KERN_PROC,
		KERN_PROC_ARGS,
		getpid(),
#endif
	};
	size_t len;
	if (sysctl(mib, ARRAY_LENGTH(mib), NULL, &len, NULL, 0))
		return NULL;

	_cleanup_free_ char *procargs = xalloc(len);
	if (sysctl(mib, ARRAY_LENGTH(mib), procargs, &len, NULL, 0))
		return NULL;

	return steal(&procargs);
#else
	int fd = open("/proc/self/cmdline", O_RDONLY);
	if (fd != -1) {
		char buffer[1024] = { 0 };
		int len = read(fd, buffer, sizeof(buffer) - 1);
		close(fd);
		return len > 0 ? xstrdup(buffer) : NULL;
	}
#endif
	return NULL;
}
