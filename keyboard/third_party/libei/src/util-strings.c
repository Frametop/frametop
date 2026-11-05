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

#include "config.h"

#include <string.h>

#include "util-io.h"
#include "util-strings.h"

/**
 * Return the next word in a string pointed to by state before the first
 * separator character. Call repeatedly to tokenize a whole string.
 *
 * @param state Current state
 * @param len String length of the word returned
 * @param separators List of separator characters
 *
 * @return The first word in *state, NOT null-terminated
 */
static const char *
next_word(const char **state, size_t *len, const char *separators)
{
	const char *next = *state;
	size_t l;

	if (!*next)
		return NULL;

	next += strspn(next, separators);
	if (!*next) {
		*state = next;
		return NULL;
	}

	l = strcspn(next, separators);
	*state = next + l;
	*len = l;

	return next;
}

/**
 * Return a null-terminated string array with the tokens in the input
 * string, e.g. "one two\tthree" with a separator list of " \t" will return
 * an array [ "one", "two", "three", NULL ].
 *
 * Use strv_free() to free the array.
 *
 * @param in Input string
 * @param separators List of separator characters
 *
 * @return A null-terminated string array or NULL on errors
 */
char **
strv_from_string(const char *in, const char *separators)
{
	const char *s, *word;
	char **strv = NULL;
	int nelems = 0, idx;
	size_t l;

	assert(in != NULL);

	s = in;
	while (next_word(&s, &l, separators) != NULL)
		nelems++;

	if (nelems == 0)
		return NULL;

	nelems++; /* NULL-terminated */
	strv = xalloc(nelems * sizeof *strv);

	idx = 0;

	s = in;
	while ((word = next_word(&s, &l, separators)) != NULL) {
		char *copy = strndup(word, l);
		if (!copy) {
			strv_free(strv);
			return NULL;
		}

		strv[idx++] = copy;
	}

	return strv;
}

/**
 * Return a newly allocated string with all elements joined by the
 * joiner, same as Python's string.join() basically.
 * A strv of ["one", "two", "three", NULL] with a joiner of ", " results
 * in "one, two, three".
 *
 * An empty strv ([NULL]) returns NULL, same for passing NULL as either
 * argument.
 *
 * @param strv Input string array
 * @param joiner Joiner between the elements in the final string
 *
 * @return A null-terminated string joining all elements
 */
char *
strv_join(char **strv, const char *joiner)
{
	char **s;
	char *str;
	size_t slen = 0;
	size_t count = 0;

	if (!strv || !joiner)
		return NULL;

	if (strv[0] == NULL)
		return NULL;

	for (s = strv, count = 0; *s; s++, count++) {
		slen += strlen(*s);
	}

	assert(slen < 1000);
	assert(strlen(joiner) < 1000);
	assert(count > 0);
	assert(count < 100);

	slen += (count - 1) * strlen(joiner);

	str = xalloc(slen + 1); /* trailing \0 */
	size_t jlen = strlen(joiner);
	size_t offset = 0;
	for (s = strv; *s; s++) {
		size_t l = strlen(*s);
		memcpy(str + offset, *s, l);
		offset += l;
		--count;
		if (count > 0) {
			memcpy(str + offset, joiner, jlen);
			offset += jlen;
		}
	}

	return str;
}

char *
strreplace(const char *string, const char *separator, const char *replacement)
{
	assert(string != NULL);
	assert(string[0] != '\0');

	/* Enough to replace every character in the string with the
	 * replacement. This will blow up on extremely long strings with long
	 * replacements, but meh. It saves us having to write resizing code.
	 */
	size_t slen = strlen(string);
	size_t splen = strlen(separator);

	const char *current = string;
	const char *next;

	next = strstr(current, separator);
	if (!next) /* No separator found */
		return xstrdup(string);

	size_t rlen = strlen(replacement);
	size_t max = slen * max(rlen, 1);
	char *r = xalloc(max + 1); /* the result, one extra for terminating \0 */
	char *destptr = r;

	while (next) {
		size_t len = next - current;
		/* silently truncate because we really don't care about this
		 * case */
		if (destptr + len > r + max)
			break;

		/* Copy the source string over, then append the separator */
		memcpy(destptr, current, len);
		destptr += len;
		if (destptr + rlen > r + max)
			break;
		memcpy(destptr, replacement, rlen);
		destptr += rlen;

		current = next + splen;
		if (current > string + max)
			break;
		next = strstr(current, separator);
	}

	size_t len = strlen(current);
	/* silently truncate because we really don't care about this
	 * case */
	if (destptr + len <= r + max) {
		memcpy(destptr, current, len);
		destptr += len;
	}

	return xrealloc(r, (destptr - r) + 1);
}

size_t
strv_len(char **strv)
{
	if (!strv)
		return 0;

	size_t size = 1;
	while (*strv) {
		size++;
		strv++;
	}
	return size;
}

char **
strv_append_take(char **strv, char **str)
{
	if (str && *str) {
		size_t len = strv_len(strv) + 1;
		len = max(len, 2);

		char **s = xrealloc(strv, len * sizeof(*strv));
		s[len - 1] = NULL;
		s[len - 2] = *str;
		*str = NULL;
		return s;
	} else {
		return strv;
	}
}

char **
strv_append_strdup(char **strv, const char *str)
{
	char *dup = xstrdup(str);
	return strv_append_take(strv, &dup);
}

bool
strv_find(char **strv, const char *needle, size_t *index_out)
{
	if (!strv)
		return false;

	size_t index = 0;
	char **s = strv;
	while (*s != NULL) {
		if (streq(*s, needle)) {
			if (index_out)
				*index_out = index;
			return true;
		}
		s++;
		index++;
	}

	return false;
}

bool
strv_find_substring(char **strv, const char *needle, size_t *index_out)
{
	if (!strv || !needle)
		return false;

	size_t index = 0;
	char **s = strv;
	while (*s != NULL) {
		if (strstr(*s, needle)) {
			if (index_out)
				*index_out = index;
			return true;
		}
		s++;
		index++;
	}

	return false;
}

char **
strv_from_mem(const uint8_t *buffer, size_t sz, size_t stride)
{
	assert(stride > 0);
	assert(stride <= 16);

	char **strv = xalloc(((sz / stride) + 2) * sizeof *strv);

#define hex(buffer_, sz, line_, offset_) \
		(line_ + offset_ < sz) ? buffer_[line_ + offset_] : 0u
	for (size_t i = 0; i < sz; i += stride) {
		char *str = xaprintf(
			"%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			hex(buffer, sz, i, 0),
			hex(buffer, sz, i, 1),
			hex(buffer, sz, i, 2),
			hex(buffer, sz, i, 3),
			hex(buffer, sz, i, 4),
			hex(buffer, sz, i, 5),
			hex(buffer, sz, i, 6),
			hex(buffer, sz, i, 7),
			hex(buffer, sz, i, 8),
			hex(buffer, sz, i, 9),
			hex(buffer, sz, i, 10),
			hex(buffer, sz, i, 11),
			hex(buffer, sz, i, 12),
			hex(buffer, sz, i, 13),
			hex(buffer, sz, i, 14),
			hex(buffer, sz, i, 15));

		/* Chop the string off at the last value or our stride, whichever applies */
		if (i + stride >= sz)
			str[(sz - i) * 3 - 1] = '\0';
		else if (stride < 16)
			str[stride * 3 - 1] = '\0';
		strv[i / stride] = str;
	}
	return strv;
}

#if _enable_tests_
#include "util-munit.h"

MUNIT_TEST(test_strsplit)
{
	struct strsplit_test {
		const char *string;
		const char *delim;
		const char *results[10];
	} tests[] = { { "one two three", " ", { "one", "two", "three", NULL } },
		      { "one", " ", { "one", NULL } },
		      { "one two ", " ", { "one", "two", NULL } },
		      { "one  two", " ", { "one", "two", NULL } },
		      { " one two", " ", { "one", "two", NULL } },
		      { "one", "\t \r", { "one", NULL } },
		      { "one two three", " t", { "one", "wo", "hree", NULL } },
		      { " one two three", "te", { " on", " ", "wo ", "hr", NULL } },
		      { "one", "ne", { "o", NULL } },
		      { "onene", "ne", { "o", NULL } },
		      { NULL, NULL, { NULL } } };
	struct strsplit_test *t = tests;

	while (t->string) {
		char **strv;
		int idx = 0;
		strv = strv_from_string(t->string, t->delim);
		while (t->results[idx]) {
			munit_assert_string_equal(t->results[idx], strv[idx]);
			idx++;
		}
		munit_assert_ptr_equal(strv[idx], NULL);
		strv_free(strv);
		t++;
	}

	/* Special cases */
	munit_assert_ptr_equal(strv_from_string("", " "), NULL);
	munit_assert_ptr_equal(strv_from_string(" ", " "), NULL);
	munit_assert_ptr_equal(strv_from_string("     ", " "), NULL);
	munit_assert_ptr_equal(strv_from_string("oneoneone", "one"), NULL);

	return MUNIT_OK;
}

MUNIT_TEST(test_kvsplit_double)
{
	struct kvsplit_dbl_test {
		const char *string;
		const char *psep;
		const char *kvsep;
		ssize_t nresults;
		struct {
			double a;
			double b;
		} results[32];
	} tests[] = { { "1:2;3:4;5:6", ";", ":", 3, { { 1, 2 }, { 3, 4 }, { 5, 6 } } },
		      { "1.0x2.3 -3.2x4.5 8.090909x-6.00",
			" ",
			"x",
			3,
			{ { 1.0, 2.3 }, { -3.2, 4.5 }, { 8.090909, -6 } } },

		      { "1:2", "x", ":", 1, { { 1, 2 } } },
		      { "1:2", ":", "x", -1, {} },
		      { "1:2", NULL, "x", -1, {} },
		      { "1:2", "", "x", -1, {} },
		      { "1:2", "x", NULL, -1, {} },
		      { "1:2", "x", "", -1, {} },
		      { "a:b", "x", ":", -1, {} },
		      { "", " ", "x", -1, {} },
		      { "1.2.3.4.5", ".", "", -1, {} },
		      { NULL } };
	struct kvsplit_dbl_test *t = tests;

	while (t->string) {
		struct key_value_double *result = NULL;
		ssize_t npairs;

		npairs = kv_double_from_string(t->string, t->psep, t->kvsep, &result);
		munit_assert_int(npairs, ==, t->nresults);

		for (ssize_t i = 0; i < npairs; i++) {
			munit_assert_double(t->results[i].a, ==, result[i].key);
			munit_assert_double(t->results[i].b, ==, result[i].value);
		}

		free(result);
		t++;
	}

	return MUNIT_OK;
}

MUNIT_TEST(test_strjoin)
{
	struct strjoin_test {
		char *strv[10];
		const char *joiner;
		const char *result;
	} tests[] = { { { "one", "two", "three", NULL }, " ", "one two three" },
		      { { "one", NULL }, "x", "one" },
		      { { "one", "two", NULL }, "x", "onextwo" },
		      { { "one", "two", NULL }, ",", "one,two" },
		      { { "one", "two", NULL }, ", ", "one, two" },
		      { { "one", "two", NULL }, "one", "oneonetwo" },
		      { { "one", "two", NULL }, NULL, NULL },
		      { { "", "", "", NULL }, " ", "  " },
		      { { "a", "b", "c", NULL }, "", "abc" },
		      { { "", "b", "c", NULL }, "x", "xbxc" },
		      { { "", "", "", NULL }, "", "" },
		      { { NULL }, NULL, NULL } };
	struct strjoin_test *t = tests;
	struct strjoin_test nulltest = { { NULL }, "x", NULL };

	while (t->strv[0]) {
		char *str;
		str = strv_join(t->strv, t->joiner);
		if (t->result == NULL)
			munit_assert(str == NULL);
		else
			munit_assert_string_equal(str, t->result);
		free(str);
		t++;
	}

	munit_assert(strv_join(nulltest.strv, "x") == NULL);

	return MUNIT_OK;
}

MUNIT_TEST(test_strstrip)
{
	struct strstrip_test {
		const char *string;
		const char *expected;
		const char *what;
	} tests[] = { { "foo", "foo", "1234" },
		      { "\"bar\"", "bar", "\"" },
		      { "'bar'", "bar", "'" },
		      { "\"bar\"", "\"bar\"", "'" },
		      { "'bar'", "'bar'", "\"" },
		      { "\"bar\"", "bar", "\"" },
		      { "\"\"", "", "\"" },
		      { "\"foo\"bar\"", "foo\"bar", "\"" },
		      { "\"'foo\"bar\"", "foo\"bar", "\"'" },
		      { "abcfooabcbarbca", "fooabcbar", "abc" },
		      { "xxxxfoo", "foo", "x" },
		      { "fooyyyy", "foo", "y" },
		      { "xxxxfooyyyy", "foo", "xy" },
		      { "x xfooy y", " xfooy ", "xy" },
		      { " foo\n", "foo", " \n" },
		      { "", "", "abc" },
		      { "", "", "" },
		      { NULL, NULL, NULL } };
	struct strstrip_test *t = tests;

	while (t->string) {
		char *str;
		str = strstrip(t->string, t->what);
		munit_assert_ptr_not_null(str);
		munit_assert_string_equal(str, t->expected);
		free(str);
		t++;
	}
	return MUNIT_OK;
}

MUNIT_TEST(test_strstartswith)
{
	struct strstartswith_test {
		const char *string;
		const char *suffix;
		bool expected;
	} tests[] = {
		{ "foobar", "foo", true },    { "foobar", "bar", false },
		{ "foobar", "foobar", true }, { "foo", "foobar", false },
		{ "foo", "", false },         { "", "", false },
		{ "foo", "", false },         { NULL, NULL, false },
	};

	for (struct strstartswith_test *t = tests; t->string; t++) {
		munit_assert_int(strstartswith(t->string, t->suffix), ==, t->expected);
	}

	return MUNIT_OK;
}

MUNIT_TEST(test_strendswith)
{
	struct strendswith_test {
		const char *string;
		const char *suffix;
		bool expected;
	} tests[] = {
		{ "foobar", "bar", true },    { "foobar", "foo", false },
		{ "foobar", "foobar", true }, { "foo", "foobar", false },
		{ "foobar", "", false },      { "", "", false },
		{ "", "foo", false },         { NULL, NULL, false },
	};

	for (struct strendswith_test *t = tests; t->string; t++) {
		munit_assert_int(strendswith(t->string, t->suffix), ==, t->expected);
	}

	return MUNIT_OK;
}

MUNIT_TEST(test_strreplace)
{
	struct strtest {
		const char *string;
		const char *separator;
		const char *replacement;
		const char *expected;
	} tests[] = {
		{ "teststring", "-", ".", "teststring" },
		{ "test-string", "-", ".", "test.string" },
		{ "test.string.", ".", "xyz", "testxyzstringxyz" },
		{ "ftestfstringf", "f", "", "teststring" },
		{ "xxx", "x", "y", "yyy" },
		{ "xyz", "x", "y", "yyz" },
		{ "xyz", "xy", "y", "yz" },
		{ .string = NULL },
	};

	for (struct strtest *t = tests; t->string; t++) {
		_cleanup_free_ char *s = strreplace(t->string, t->separator, t->replacement);
		munit_assert_string_equal(t->expected, s);
	}

	return MUNIT_OK;
}

MUNIT_TEST(test_cmdline_as_str)
{
	_cleanup_free_ char *from_function = cmdline_as_str();
	char cmdline[PATH_MAX];

	xsnprintf(cmdline, sizeof(cmdline), "/proc/%i/cmdline", getpid());

	_cleanup_close_ int fd = open(cmdline, O_RDONLY);
	munit_assert_int(fd, >=, 0);
	int len = read(fd, cmdline, sizeof(cmdline) - 1);
	munit_assert_int(len, >=, 0);
	cmdline[len] = '\0';

	munit_assert_ptr_not_null(from_function);
	munit_assert_string_equal(cmdline, from_function);

	return MUNIT_OK;
}

MUNIT_TEST(test_strlen0)
{
	munit_assert_int(strlen0(NULL), ==, 0);
	munit_assert_int(strlen0(""), ==, 1);
	munit_assert_int(strlen0("foo"), ==, 4);

	return MUNIT_OK;
}

MUNIT_TEST(test_strv_from_mem)
{
	uint8_t buf[36];

	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = i;

	{
		_cleanup_(strv_freep) char **strv = strv_from_mem(buf, 16, 16);
		munit_assert(strv != NULL);
		munit_assert(strv[0] != NULL);
		munit_assert_null(strv[1]); /* we expect one line */
		munit_assert_string_equal(strv[0],
					  "00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f");
	}

	{
		_cleanup_(strv_freep) char **strv = strv_from_mem(buf, 8, 16);
		munit_assert(strv != NULL);
		munit_assert(strv[0] != NULL);
		munit_assert_null(strv[1]); /* we expect one line */
		munit_assert_string_equal(strv[0], "00 01 02 03 04 05 06 07");
	}

	{
		_cleanup_(strv_freep) char **strv = strv_from_mem(buf, 8, 4);
		munit_assert(strv != NULL);
		munit_assert(strv[0] != NULL);
		munit_assert(strv[1] != NULL);
		munit_assert_null(strv[2]); /* we expect two lines */
		munit_assert_string_equal(strv[0], "00 01 02 03");
		munit_assert_string_equal(strv[1], "04 05 06 07");
	}

	{
		_cleanup_(strv_freep) char **strv = strv_from_mem(buf, sizeof(buf), 5);
		munit_assert(strv != NULL);
		munit_assert_string_equal(strv[0], "00 01 02 03 04");
		munit_assert_string_equal(strv[1], "05 06 07 08 09");
		munit_assert_string_equal(strv[2], "0a 0b 0c 0d 0e");
		munit_assert_string_equal(strv[3], "0f 10 11 12 13");
		munit_assert_string_equal(strv[4], "14 15 16 17 18");
		munit_assert_string_equal(strv[5], "19 1a 1b 1c 1d");
		munit_assert_string_equal(strv[6], "1e 1f 20 21 22");
		munit_assert_string_equal(strv[7], "23");
		munit_assert_null(strv[8]);
	}

	{
		uint8_t buffer[14];
		memset(buffer, -1, sizeof(buffer));
		_cleanup_(strv_freep) char **strv = strv_from_mem(buffer, sizeof(buffer), 8);
		munit_assert(strv != NULL);
		munit_assert_string_equal(strv[0], "ff ff ff ff ff ff ff ff");
		munit_assert_string_equal(strv[1], "ff ff ff ff ff ff");
		munit_assert_null(strv[2]);
	}

	return MUNIT_OK;
}

MUNIT_TEST(test_xatou)
{
	unsigned int val;

	munit_assert_true(xatou("0", &val));
	munit_assert_uint(val, ==, 0);

	munit_assert_true(xatou("1", &val));
	munit_assert_uint(val, ==, 1);

	munit_assert_true(xatou("123", &val));
	munit_assert_uint(val, ==, 123);

	/* UINT_MAX is the upper boundary, must succeed */
	munit_assert_true(xatou("4294967295", &val));
	munit_assert_uint(val, ==, UINT_MAX);

	/* UINT_MAX + 1 must fail */
	munit_assert_false(xatou("4294967296", &val));

	/* Another random value in the range UINT_MAX < val < LONG_MAX */
	munit_assert_false(xatou("8589934592", &val));

	/* LONG_MAX as string - must fail */
	munit_assert_false(xatou("9223372036854775807", &val));

	/* Overflow beyond ULONG_MAX - strtoul sets errno, must fail */
	munit_assert_false(xatou("18446744073709551616", &val));

	/* negative numbers: strtoul wraps "-1" to ULONG_MAX without
	 * setting errno, but v > UINT_MAX catches it */
	munit_assert_false(xatou("-1", &val));

	/* invalid strings */
	munit_assert_false(xatou("", &val));
	munit_assert_false(xatou("abc", &val));
	munit_assert_false(xatou("123abc", &val));
	munit_assert_false(xatou("12 34", &val));

	/* hex via xatou_base */
	munit_assert_true(xatou_base("ff", &val, 16));
	munit_assert_uint(val, ==, 0xff);

	munit_assert_true(xatou_base("0xff", &val, 16));
	munit_assert_uint(val, ==, 0xff);

	munit_assert_true(xatou_base("FFFFFFFF", &val, 16));
	munit_assert_uint(val, ==, UINT_MAX);

	/* hex UINT_MAX + 1 */
	munit_assert_false(xatou_base("100000000", &val, 16));

	/* octal */
	munit_assert_true(xatou_base("77", &val, 8));
	munit_assert_uint(val, ==, 077);

	munit_assert_true(xatou_base("37777777777", &val, 8));
	munit_assert_uint(val, ==, UINT_MAX);

	/* octal UINT_MAX + 1 */
	munit_assert_false(xatou_base("40000000000", &val, 8));

	return MUNIT_OK;
}

#endif
