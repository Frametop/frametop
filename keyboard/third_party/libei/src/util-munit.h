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

/* This is a wrapper around munit to make it faster to use for the simple
 * type of test cases.
 *
 * Use with the MUNIT_TEST macro like this:
 *
 * MUNIT_TEST(some_test) {
 *	return MUNIT_OK;
 * }
 *
 * int main(int argc, char **argv) {
 *	return munit_tests_run(argc, argv);
 * }
 *
 */

#pragma once

#include <munit.h>

/* Random number, bumb if need be */
#define MUNIT_TEST_MAX_PARAMS 64

/**
 * Put at the top of the file somewhere, declares the start/stop for the test section we need.
 */
#define DECLARE_TEST_SECTION() \
	extern const struct test_function __start_test_functions_section, __stop_test_functions_section

/**
 * Helper to loop through each test.
 */
#define foreach_test(t_) \
	for (const struct test_function *t_ = &__start_test_functions_section; \
	     t_ < &__stop_test_functions_section; \
	     t_++)

typedef MunitResult (*munit_test_func_t)(const MunitParameter params[], void *user_data);

struct test_function {
	const char *name;       /* function name */
	const char *file;       /* file name */
	munit_test_func_t func; /* test function */
	const char *params[MUNIT_TEST_MAX_PARAMS];
} __attribute__((aligned(16)));

/**
 * Put at the top of the file somewhere, declares the start/stop for the setup section we need.
 */
#define DECLARE_GLOBAL_SETUP_SECTION() \
	extern const struct global_setup_function __start_setup_functions_section, __stop_setup_functions_section

/**
 * Helper to loop through each setup function.
 */
#define foreach_setup(s_) \
	for (const struct global_setup_function *s_ = &__start_setup_functions_section; \
	     s_ < &__stop_setup_functions_section; \
	     s_++)

struct munit_setup {
	int argc;
	char **argv;
	void *userdata;
};

typedef void (*munit_setup_func_t)(struct munit_setup *setup);

struct global_setup_function {
	const char *name;        /* function name */
	munit_setup_func_t func; /* setup function */
} __attribute__((aligned(16)));

/**
 * Defines a struct test_function in a custom ELF section that we can then
 * loop over in munit_tests_run() to extract the tests. This removes the
 * need of manually adding the tests to a suite or listing them somewhere.
 */
#define MUNIT_TEST(_func) \
static MunitResult _func(const MunitParameter params[], void *user_data);	\
static const struct test_function _test_##_func					\
__attribute__((used))								\
__attribute__((section("test_functions_section"))) = {				\
	.name = #_func,								\
	.func = _func,								\
	.file = __FILE__,							\
	.params = { NULL },							\
};										\
static MunitResult _func(const MunitParameter params[], void *user_data)

/**
 * Same as MUNIT test but takes a list of strings that define parameters
 * and their values. Parameter names must be prefixed with @, for example:
 * MUNIT_TEST_WITH_PARAMS(test_foo, "@x", "1", "2", "@y", "100", "200") {
 *    const char *x = MUNIT_TEST_PARAM("@x");
 *    const char *y = MUNIT_TEST_PARAM("@y");
 * }
 */
#define MUNIT_TEST_WITH_PARAMS(_func, ...) \
static MunitResult _func(const MunitParameter params[], void *user_data);	\
static const struct test_function _test_##_func					\
__attribute__((used))								\
__attribute__((section("test_functions_section"))) = {				\
	.name = #_func,								\
	.func = _func,								\
	.file = __FILE__,							\
	.params = { __VA_ARGS__, NULL },					\
};										\
static MunitResult _func(const MunitParameter params[], void *user_data)

/* Retrieve the test parameter with the given name */
#define MUNIT_TEST_PARAM(name_) \
	munit_parameters_get(params, name_)

/**
 * Defines a struct global_setup_function in a custom ELF section that we can
 * then find in munit_tests_run() to do setup based on argv and/or pass userdata
 * to the tests.
 *
 * Note that there can only be one of those per process.
 *
 * The argument to this function is a pointer to a struct munit_setup that has
 * argc/argv and a (initially NULL) userdata pointer.
 */
#define MUNIT_GLOBAL_SETUP(_func) \
static void _func(struct munit_setup *setup); 					\
static const struct global_setup_function _setup_##_func			\
__attribute__((used))								\
__attribute__((section("setup_functions_section"))) = {				\
	.name = #_func,								\
	.func = _func,								\
};										\
static void _func(struct munit_setup *setup)

int
munit_tests_run(int argc, char **argv);
