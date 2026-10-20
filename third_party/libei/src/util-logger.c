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

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "util-logger.h"
#include "util-object.h"

struct logger {
	struct object object;
	enum logger_priority priority;
	logger_log_func_t handler;
	void *user_data;
	char *prefix;
};

_printf_(7, 0) static void
logger_default_log_func(struct logger *logger,
			const char *prefix,
			enum logger_priority priority,
			const char *file,
			int lineno,
			const char *func,
			const char *format,
			va_list args)
{
	const char *msgtype;

	switch (priority) {
	case LOGGER_DEBUG:
		msgtype = "debug";
		break;
	case LOGGER_INFO:
		msgtype = "info";
		break;
	case LOGGER_WARN:
		msgtype = "warn";
		break;
	case LOGGER_ERROR:
		msgtype = "error";
		break;
	default:
		msgtype = "<invalid msgtype>";
		prefix = "<invalid priority>";
		break;
	}

	fprintf(stderr, "%s: %s: ", prefix, msgtype);
	vfprintf(stderr, format, args);
}

void
log_msg_va(struct logger *logger,
	   enum logger_priority priority,
	   const char *file,
	   int lineno,
	   const char *func,
	   const char *format,
	   va_list args)
{
	if (logger->handler && logger->priority <= priority)
		logger->handler(logger, logger->prefix, priority, file, lineno, func, format, args);
}

void
log_msg(struct logger *logger,
	enum logger_priority priority,
	const char *file,
	int lineno,
	const char *func,
	const char *format,
	...)
{
	va_list args;

	va_start(args, format);
	log_msg_va(logger, priority, file, lineno, func, format, args);
	va_end(args);
}

static void
logger_destroy(struct logger *logger)
{
	free(logger->prefix);
}

static OBJECT_IMPLEMENT_CREATE(logger);
OBJECT_IMPLEMENT_UNREF_CLEANUP(logger);
OBJECT_IMPLEMENT_SETTER(logger, priority, enum logger_priority);
OBJECT_IMPLEMENT_GETTER(logger, priority, enum logger_priority);
OBJECT_IMPLEMENT_SETTER(logger, user_data, void *);
OBJECT_IMPLEMENT_GETTER(logger, user_data, void *);
OBJECT_IMPLEMENT_SETTER(logger, handler, logger_log_func_t);

struct logger *
logger_new(const char *prefix, void *user_data)
{
	struct logger *logger = logger_create(NULL);
	logger->prefix = prefix ? strdup(prefix) : NULL;
	logger->user_data = user_data;
	logger->priority = LOGGER_WARN;
	logger->handler = logger_default_log_func;

	return logger;
}
