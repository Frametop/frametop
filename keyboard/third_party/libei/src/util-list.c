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

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "util-list.h"

void
list_init(struct list *list)
{
	list->prev = list;
	list->next = list;
}

void
list_insert(struct list *list, struct list *elm)
{
	assert((list->next != NULL && list->prev != NULL) ||
	       !"list->next|prev is NULL, possibly missing list_init()");
	assert(((elm->next == NULL && elm->prev == NULL) || list_empty(elm)) ||
	       !"elm->next|prev is not NULL, list node used twice?");

	elm->prev = list;
	elm->next = list->next;
	list->next = elm;
	elm->next->prev = elm;
}

void
list_append(struct list *list, struct list *elm)
{
	assert((list->next != NULL && list->prev != NULL) ||
	       !"list->next|prev is NULL, possibly missing list_init()");
	assert(((elm->next == NULL && elm->prev == NULL) || list_empty(elm)) ||
	       !"elm->next|prev is not NULL, list node used twice?");

	elm->next = list;
	elm->prev = list->prev;
	list->prev = elm;
	elm->prev->next = elm;
}

void
list_remove(struct list *elm)
{
	assert((elm->next != NULL && elm->prev != NULL) ||
	       !"list->next|prev is NULL, possibly missing list_init()");

	elm->prev->next = elm->next;
	elm->next->prev = elm->prev;
	elm->next = NULL;
	elm->prev = NULL;
}

bool
list_empty(const struct list *list)
{
	assert((list->next != NULL && list->prev != NULL) ||
	       !"list->next|prev is NULL, possibly missing list_init()");

	return list->next == list;
}

#if _enable_tests_
#include "util-macros.h"
#include "util-munit.h"

MUNIT_TEST(test_list_insert)
{
	struct list_test {
		int val;
		struct list node;
	} tests[] = {
		{ .val = 1 },
		{ .val = 2 },
		{ .val = 3 },
		{ .val = 4 },
	};
	struct list_test *t;
	struct list head;

	list_init(&head);
	munit_assert(list_empty(&head));

	ARRAY_FOR_EACH(tests, t) {
		list_insert(&head, &t->node);
	}

	int val = 4;
	list_for_each(t, &head, node) {
		munit_assert_int(t->val, ==, val);
		val--;
	}

	munit_assert_int(val, ==, 0);
	return MUNIT_OK;
}

MUNIT_TEST(test_list_append)
{
	struct list_test {
		int val;
		struct list node;
	} tests[] = {
		{ .val = 1 },
		{ .val = 2 },
		{ .val = 3 },
		{ .val = 4 },
	};
	struct list_test *t;
	struct list head;

	list_init(&head);
	munit_assert(list_empty(&head));

	ARRAY_FOR_EACH(tests, t) {
		list_append(&head, &t->node);
	}

	int val = 1;
	list_for_each(t, &head, node) {
		munit_assert_int(t->val, ==, val);
		val++;
	}
	munit_assert_int(val, ==, 5);

	return MUNIT_OK;
}

MUNIT_TEST(test_list_nth)
{
	struct list_test {
		int val;
		struct list node;
	} tests[] = {
		{ .val = 1 },
		{ .val = 2 },
		{ .val = 3 },
		{ .val = 4 },
	};
	struct list_test *t;
	struct list head;

	list_init(&head);
	ARRAY_FOR_EACH(tests, t) {
		list_append(&head, &t->node);
	}

	int idx = 0;
	ARRAY_FOR_EACH(tests, t) {
		struct list_test *nth = list_nth_entry(struct list_test, &head, node, idx);
		munit_assert_int(nth->val, ==, tests[idx].val);
		idx++;
	}

	munit_assert(list_nth_entry(struct list_test, &head, node, 10) == NULL);
	list_init(&head);
	munit_assert(list_nth_entry(struct list_test, &head, node, 0) == NULL);
	munit_assert(list_nth_entry(struct list_test, &head, node, 1) == NULL);

	return MUNIT_OK;
}

MUNIT_TEST(list_first_last)
{
	struct list_test {
		int val;
		struct list node;
	} tests[] = {
		{ .val = 1 },
		{ .val = 2 },
		{ .val = 3 },
		{ .val = 4 },
	};
	struct list_test *t;
	struct list head;

	list_init(&head);
	ARRAY_FOR_EACH(tests, t) {
		list_append(&head, &t->node);
	}

	struct list_test *first = list_first_entry(&head, t, node);
	munit_assert_int(first->val, ==, 1);
	first = list_first_entry_by_type(&head, struct list_test, node);
	munit_assert_int(first->val, ==, 1);

	struct list_test *last = list_last_entry(&head, t, node);
	munit_assert_int(last->val, ==, 4);
	last = list_last_entry_by_type(&head, struct list_test, node);
	munit_assert_int(last->val, ==, 4);

	return MUNIT_OK;
}

MUNIT_TEST(list_foreach)
{
	struct list_test {
		int val;
		struct list node;
	} tests[] = {
		{ .val = 1 },
		{ .val = 2 },
		{ .val = 3 },
		{ .val = 4 },
	};
	struct list_test *t;
	struct list head;

	list_init(&head);
	ARRAY_FOR_EACH(tests, t) {
		list_append(&head, &t->node);
	}

	unsigned int idx = 0;
	list_for_each(t, &head, node) {
		munit_assert_int(t->val, ==, tests[idx++].val);
	}

	list_for_each_backwards(t, &head, node)
	{
		munit_assert_int(t->val, ==, tests[--idx].val);
	}

	return MUNIT_OK;
}

#endif
