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

#include "util-strings.h"

#include "libei-private.h"

static void
ei_region_destroy(struct ei_region *region)
{
	free(region->mapping_id);
	list_remove(&region->link);
}

_public_ OBJECT_IMPLEMENT_REF(ei_region);
_public_ OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_region);
static OBJECT_IMPLEMENT_CREATE(ei_region);
_public_
OBJECT_IMPLEMENT_GETTER(ei_region, user_data, void *);
_public_
OBJECT_IMPLEMENT_SETTER(ei_region, user_data, void *);
_public_
OBJECT_IMPLEMENT_GETTER(ei_region, x, uint32_t);
_public_
OBJECT_IMPLEMENT_GETTER(ei_region, y, uint32_t);
_public_
OBJECT_IMPLEMENT_GETTER(ei_region, width, uint32_t);
_public_
OBJECT_IMPLEMENT_GETTER(ei_region, height, uint32_t);
_public_
OBJECT_IMPLEMENT_GETTER(ei_region, physical_scale, double);
OBJECT_IMPLEMENT_SETTER(ei_region, physical_scale, double);
_public_
OBJECT_IMPLEMENT_GETTER(ei_region, mapping_id, const char *);

struct ei_region *
ei_region_new(void)
{
	struct ei_region *region = ei_region_create(NULL);

	region->physical_scale = 1.0;
	list_init(&region->link);

	return region;
}

void
ei_region_set_offset(struct ei_region *region, uint32_t x, uint32_t y)
{
	region->x = x;
	region->y = y;
}

void
ei_region_set_size(struct ei_region *region, uint32_t w, uint32_t h)
{
	region->width = w;
	region->height = h;
}

void
ei_region_set_mapping_id(struct ei_region *region, const char *mapping_id)
{
	region->mapping_id = xstrdup(mapping_id);
}

_public_ bool
ei_region_contains(struct ei_region *r, double x, double y)
{
	return (x >= r->x && x < r->x + r->width && y >= r->y && y < r->y + r->height);
}

_public_ bool
ei_region_convert_point(struct ei_region *r, double *x, double *y)
{
	if (ei_region_contains(r, *x, *y)) {
		*x -= r->x;
		*y -= r->y;
		return true;
	}

	return false;
}

#ifdef _enable_tests_
#include "util-munit.h"
MUNIT_TEST(test_region_setters)
{
	_unref_(ei_region) *r = ei_region_new();

	ei_region_set_size(r, 1, 2);
	ei_region_set_offset(r, 3, 4);
	ei_region_set_physical_scale(r, 5.6);
	ei_region_set_mapping_id(r, "foo");

	munit_assert_int(ei_region_get_width(r), ==, 1);
	munit_assert_int(ei_region_get_height(r), ==, 2);
	munit_assert_int(ei_region_get_x(r), ==, 3);
	munit_assert_int(ei_region_get_y(r), ==, 4);
	munit_assert_double(ei_region_get_physical_scale(r), ==, 5.6);
	munit_assert_string_equal(ei_region_get_mapping_id(r), "foo");

	return MUNIT_OK;
}

MUNIT_TEST(test_region_contains)
{
	struct ei_region r = { 0 };

	ei_region_set_size(&r, 100, 200);
	ei_region_set_offset(&r, 300, 400);

	munit_assert_true(ei_region_contains(&r, 300, 400));
	munit_assert_true(ei_region_contains(&r, 399.9, 599.9));

	munit_assert_false(ei_region_contains(&r, 299.9, 400));
	munit_assert_false(ei_region_contains(&r, 300, 399.9));

	munit_assert_false(ei_region_contains(&r, 400.1, 400));
	munit_assert_false(ei_region_contains(&r, 400, 399.9));

	munit_assert_false(ei_region_contains(&r, 299.9, 599.9));
	munit_assert_false(ei_region_contains(&r, 300, 600.1));

	munit_assert_false(ei_region_contains(&r, 400, 599.9));
	munit_assert_false(ei_region_contains(&r, 399, 600));

	return MUNIT_OK;
}

MUNIT_TEST(test_region_convert)
{
	struct ei_region r = { 0 };

	ei_region_set_size(&r, 640, 480);
	ei_region_set_offset(&r, 100, 200);

	double x = 100;
	double y = 200;
	munit_assert_true(ei_region_convert_point(&r, &x, &y));
	munit_assert_double_equal(x, 0, 4 /* precision */);
	munit_assert_double_equal(y, 0, 4 /* precision */);

	x = 101.2;
	y = 202.3;
	munit_assert_true(ei_region_convert_point(&r, &x, &y));
	munit_assert_double_equal(x, 1.2, 4 /* precision */);
	munit_assert_double_equal(y, 2.3, 4 /* precision */);

	x = 99.9;
	y = 199.9;
	munit_assert_false(ei_region_convert_point(&r, &x, &y));
	munit_assert_double_equal(x, 99.9, 4 /* precision */);
	munit_assert_double_equal(y, 199.9, 4 /* precision */);

	return MUNIT_OK;
}
#endif
