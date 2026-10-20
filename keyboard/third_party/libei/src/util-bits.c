/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Red Hat, Inc.
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

#include "util-bits.h"

#if _enable_tests_
#include "util-munit.h"

MUNIT_TEST(test_bits_flag_32)
{
	uint32_t mask = 0;

	munit_assert_true(flag_fits(mask, 0));
	munit_assert_true(flag_fits(mask, 31));
	munit_assert_false(flag_fits(mask, 32));
	munit_assert_false(flag_fits(mask, -1));

	flag_set(mask, 0);
	munit_assert_true(flag_is_set(mask, 0));
	munit_assert_false(flag_is_set(mask, 31));
	munit_assert_false(flag_is_set(mask, 32));
	flag_set(mask, 31);
	munit_assert_true(flag_is_set(mask, 0));
	munit_assert_true(flag_is_set(mask, 31));
	munit_assert_false(flag_is_set(mask, 32));
	flag_set(mask, 32); /* silently ignored */
	munit_assert_true(flag_is_set(mask, 0));
	munit_assert_true(flag_is_set(mask, 31));
	munit_assert_false(flag_is_set(mask, 32));

	munit_assert_int(mask, ==, 0x80000001);

	flag_clear(mask, 0);
	munit_assert_false(flag_is_set(mask, 0));
	munit_assert_true(flag_is_set(mask, 31));
	munit_assert_false(flag_is_set(mask, 32));
	flag_clear(mask, 31);
	munit_assert_false(flag_is_set(mask, 0));
	munit_assert_false(flag_is_set(mask, 31));
	munit_assert_false(flag_is_set(mask, 32));
	flag_clear(mask, 32);
	munit_assert_false(flag_is_set(mask, 0));
	munit_assert_false(flag_is_set(mask, 31));
	munit_assert_false(flag_is_set(mask, 32));

	return MUNIT_OK;
}

MUNIT_TEST(test_bits_flag_8)
{
	uint8_t mask = 0;

	munit_assert_true(flag_fits(mask, 0));
	munit_assert_true(flag_fits(mask, 7));
	munit_assert_false(flag_fits(mask, 8));
	munit_assert_false(flag_fits(mask, -1));

	flag_set(mask, 0);
	munit_assert_true(flag_is_set(mask, 0));
	munit_assert_false(flag_is_set(mask, 7));
	munit_assert_false(flag_is_set(mask, 8));
	flag_set(mask, 7);
	munit_assert_true(flag_is_set(mask, 0));
	munit_assert_true(flag_is_set(mask, 7));
	munit_assert_false(flag_is_set(mask, 8));
	flag_set(mask, 8); /* silently ignored */
	munit_assert_true(flag_is_set(mask, 0));
	munit_assert_true(flag_is_set(mask, 7));
	munit_assert_false(flag_is_set(mask, 8));

	munit_assert_int(mask, ==, 0x81);

	flag_clear(mask, 0);
	munit_assert_false(flag_is_set(mask, 0));
	munit_assert_true(flag_is_set(mask, 7));
	munit_assert_false(flag_is_set(mask, 8));
	flag_clear(mask, 7);
	munit_assert_false(flag_is_set(mask, 0));
	munit_assert_false(flag_is_set(mask, 7));
	munit_assert_false(flag_is_set(mask, 8));
	flag_clear(mask, 8);
	munit_assert_false(flag_is_set(mask, 0));
	munit_assert_false(flag_is_set(mask, 7));
	munit_assert_false(flag_is_set(mask, 8));

	return MUNIT_OK;
}

MUNIT_TEST(test_bits_mask)
{
	munit_assert_true(mask_any(5, 3));
	munit_assert_true(mask_any(5, 1));
	munit_assert_false(mask_any(5, 2));

	munit_assert_true(mask_all(5, 5));
	munit_assert_true(mask_all(5, 1));
	munit_assert_true(mask_all(5, 4));

	munit_assert_false(mask_all(5, 6));
	munit_assert_false(mask_all(5, 3));

	munit_assert_true(mask_all(13, 5));
	munit_assert_true(mask_all(13, 12));

	munit_assert_true(mask_none(21, 10));
	munit_assert_false(mask_none(21, 5));

	uint8_t mask = 0;
	mask_add(mask, 5);
	munit_assert_int(mask, ==, 5);
	mask_add(mask, 2);
	munit_assert_int(mask, ==, 7);
	mask_remove(mask, 2);
	munit_assert_int(mask, ==, 5);

	return MUNIT_OK;
}

#endif
