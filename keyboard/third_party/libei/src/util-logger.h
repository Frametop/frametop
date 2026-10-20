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

#include <stdarg.h>

#include "util-macros.h"

struct logger;

enum logger_priority {
	LOGGER_DEBUG = 20,
	LOGGER_INFO = 30,
	LOGGER_WARN = 40,
	LOGGER_ERROR = 50,
};

typedef void (*logger_log_func_t)(struct logger *logger,
				  const char *prefix,
				  enum logger_priority priority,
				  const char *file,
				  int lineno,
				  const char *func,
				  const char *format,
				  va_list args);

void
log_msg(struct logger *logger,
	enum logger_priority priority,
	const char *file,
	int lineno,
	const char *func,
	const char *format,
	...);

void
log_msg_va(struct logger *logger,
	   enum logger_priority priority,
	   const char *file,
	   int lineno,
	   const char *func,
	   const char *format,
	   va_list args);

/* log helpers. The struct T_ needs to have a field called 'logger' */
#define log_debug(T_, ...) \
	log_msg((T_)->logger, LOGGER_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_info(T_, ...) \
	log_msg((T_)->logger, LOGGER_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_warn(T_, ...) \
	log_msg((T_)->logger, LOGGER_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_error(T_, ...) \
	log_msg((T_)->logger, LOGGER_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_bug(T_, ...) \
	log_msg((T_)->logger, LOGGER_ERROR, __FILE__, __LINE__, __func__, "bug: " __VA_ARGS__)

struct logger *
logger_new(const char *prefix, void *user_data);

struct logger *
logger_unref(struct logger *logger);

void *
logger_get_user_data(struct logger *logger);
void
logger_set_user_data(struct logger *logger, void *user_data);

enum logger_priority
logger_get_priority(struct logger *logger);

void
logger_set_priority(struct logger *logger, enum logger_priority priority);

void
logger_set_handler(struct logger *logger, logger_log_func_t handler);
