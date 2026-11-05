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

#include <stdio.h>
#include <time.h>

#include "util-color.h"
#include "util-macros.h"
#include "util-strings.h"

#include "libei-private.h"

struct ei_log_context {
	const char *file;
	const char *func;
	int line;
};

_public_
OBJECT_IMPLEMENT_GETTER(ei_log_context, file, const char *);
_public_
OBJECT_IMPLEMENT_GETTER(ei_log_context, func, const char *);
_public_
OBJECT_IMPLEMENT_GETTER(ei_log_context, line, unsigned int);

static void
ei_default_log_handler(struct ei *ei,
		       enum ei_log_priority priority,
		       const char *message,
		       struct ei_log_context *ctx)
{
	struct lut {
		const char *color;
		const char *prefix;
	} lut[] = {
		{
			.color = ansi_colorcode[RED],
			.prefix = "<undefined>",
		}, /* debug starts at 10 */
		{
			.color = ansi_colorcode[HIGHLIGHT],
			.prefix = "DEBUG",
		},
		{
			.color = ansi_colorcode[GREEN],
			.prefix = "INFO",
		},
		{
			.color = ansi_colorcode[BLUE],
			.prefix = "WARN",
		},
		{
			.color = ansi_colorcode[RED],
			.prefix = "ERROR",
		},
	};
	static time_t last_time = 0;
	const char *reset_code = ansi_colorcode[RESET];

	run_only_once
	{
		if (!isatty(STDERR_FILENO)) {
			struct lut *l;
			ARRAY_FOR_EACH(lut, l)
				l->color = "";
			reset_code = "";
		}
	}

	time_t now = time(NULL);
	char timestamp[64];

	if (last_time != now) {
		struct tm *tm = localtime(&now);
		strftime(timestamp, sizeof(timestamp), "%T", tm);
	} else {
		xsnprintf(timestamp, sizeof(timestamp), "...");
	}

	size_t idx = priority / 10;
	assert(idx < ARRAY_LENGTH(lut));
	fprintf(stderr,
		" %8s | %s%4s%s | %s\n",
		timestamp,
		lut[idx].color,
		lut[idx].prefix,
		reset_code,
		message);

	last_time = now;
}

_public_ void
ei_log_set_handler(struct ei *ei, ei_log_handler log_handler)
{
	ei->log.handler = log_handler ? log_handler : ei_default_log_handler;
}

_public_ void
ei_log_set_priority(struct ei *ei, enum ei_log_priority priority)
{
	switch (priority) {
	case EI_LOG_PRIORITY_DEBUG:
	case EI_LOG_PRIORITY_INFO:
	case EI_LOG_PRIORITY_WARNING:
	case EI_LOG_PRIORITY_ERROR:
		break;
	default:
		abort();
	}
	ei->log.priority = priority;
}

_public_ enum ei_log_priority
ei_log_get_priority(const struct ei *ei)
{
	return ei->log.priority;
}

void
ei_log_msg(struct ei *ei,
	   enum ei_log_priority priority,
	   const char *file,
	   int lineno,
	   const char *func,
	   const char *format,
	   ...)
{
	va_list args;

	va_start(args, format);
	ei_log_msg_va(ei, priority, file, lineno, func, format, args);
	va_end(args);
}

void
ei_log_msg_va(struct ei *ei,
	      enum ei_log_priority priority,
	      const char *file,
	      int lineno,
	      const char *func,
	      const char *format,
	      va_list ap)
{
	if (ei->log.priority > priority || !ei->log.handler)
		return;

	_cleanup_free_ char *message = xvaprintf(format, ap);
	struct ei_log_context ctx = {
		.file = file,
		.line = lineno,
		.func = func,
	};
	ei->log.handler(ei, priority, message, &ctx);
}

#ifdef _enable_tests_
#include "util-munit.h"

struct log_handler_check {
	enum ei_log_priority min_priority;
	const char *expected_message;
};

static void
test_loghandler(struct ei *ei,
		enum ei_log_priority priority,
		const char *message,
		struct ei_log_context *ctx)
{
	struct log_handler_check *check = ei_get_user_data(ei);

	munit_assert_int(priority, >=, check->min_priority);
	munit_assert_string_equal(message, check->expected_message);
	/* Second arg is the line number, if this code ever moves further up,
	 * this test may fail */
	munit_assert_int(ei_log_context_get_line(ctx), >, 170);
	munit_assert_true(strendswith(ei_log_context_get_file(ctx), "libei-log.c"));
	munit_assert_string_equal(ei_log_context_get_func(ctx), "test_log_handler");
}

MUNIT_TEST(test_log_handler)
{
	struct log_handler_check check = { 0 };
	struct ei *ei = ei_new(&check);

	munit_assert_ptr_equal(ei->log.handler, ei_default_log_handler);
	munit_assert_int(ei->log.priority, ==, EI_LOG_PRIORITY_INFO);

	ei_log_set_handler(ei, test_loghandler);

	check.min_priority = EI_LOG_PRIORITY_INFO; /* default */
	check.expected_message = "info message";

	log_debug(ei, "default is below this level");
	log_info(ei, "info message");

	check.expected_message = "error message";
	log_error(ei, "error message");

	check.min_priority = EI_LOG_PRIORITY_ERROR;
	ei_log_set_priority(ei, EI_LOG_PRIORITY_ERROR);
	munit_assert_int(ei->log.priority, ==, EI_LOG_PRIORITY_ERROR);
	log_error(ei, "error message");
	log_warn(ei, "warn message");
	log_info(ei, "info message");
	log_debug(ei, "debug message");

	ei_log_set_priority(ei, EI_LOG_PRIORITY_DEBUG);

	/* Make sure they come through at the right priority */
	check.min_priority = EI_LOG_PRIORITY_ERROR;
	check.expected_message = "error message";
	log_error(ei, "error message");
	check.min_priority = EI_LOG_PRIORITY_WARNING;
	check.expected_message = "warn message";
	log_warn(ei, "warn message");
	check.min_priority = EI_LOG_PRIORITY_INFO;
	check.expected_message = "info message";
	log_info(ei, "info message");
	check.min_priority = EI_LOG_PRIORITY_DEBUG;
	check.expected_message = "debug message";
	log_debug(ei, "debug message");

	/* Can't capture this easily, so this is mostly just a crash test */
	ei_log_set_handler(ei, NULL);
	munit_assert_ptr_equal(ei->log.handler, ei_default_log_handler);
	log_debug(ei, "ignore this, testing NULL log handler");

	ei_unref(ei);

	return MUNIT_OK;
}
#endif
