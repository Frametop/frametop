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

#ifdef NDEBUG
#warning "This project relies on assert(). #defining NDEBUG is not recommended"
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "util-list.h"
#include "util-macros.h"
#include "util-object.h"

#include "brei-proto.h"

struct brei_interface;
struct brei_message;
struct brei_object;

struct brei_context;

struct brei_result {
	struct object object;
	int reason; /* enum ei(s)_connection_disconnect_reason */
	char *explanation;
	void *data;
};

struct brei_result *
brei_result_new_success(void *data);

struct brei_result *
brei_result_new_from_neg_errno(int err);

_printf_(2, 3) struct brei_result *
brei_result_new(int reason, const char *format, ...);

OBJECT_DECLARE_GETTER(brei_result, data, void *);
OBJECT_DECLARE_SETTER(brei_result, data, void *);
OBJECT_DECLARE_GETTER(brei_result, reason, int);
OBJECT_DECLARE_GETTER(brei_result, explanation, const char *);
OBJECT_DECLARE_REF(brei_result);
OBJECT_DECLARE_UNREF(brei_result);

enum brei_log_priority {
	BREI_LOG_PRIORITY_DEBUG = 10,
	BREI_LOG_PRIORITY_INFO = 20,
	BREI_LOG_PRIORITY_WARNING = 30,
	BREI_LOG_PRIORITY_ERROR = 40,
};

typedef void (*brei_logfunc_t)(void *context,
			       enum brei_log_priority priority,
			       const char *file,
			       int lineno,
			       const char *func,
			       const char *format,
			       va_list args);

struct brei_context *
brei_context_new(void *user_data);

OBJECT_DECLARE_SETTER(brei_context, log_func, brei_logfunc_t);
OBJECT_DECLARE_SETTER(brei_context, log_context, void *);
OBJECT_DECLARE_REF(brei_context);
OBJECT_DECLARE_UNREF(brei_context);

union brei_arg {
	uint32_t u;
	int32_t i;
	float f;
	int h;
	const char *s;
	object_id_t o;
	object_id_t n;
	uint64_t t;
	int64_t x;
};

struct brei_message {
	const char *name; /* request/event name */
	const char *signature;
	const struct brei_interface **types;
};

typedef struct brei_result *(*brei_event_dispatcher)(void *object,
						     uint32_t opcode,
						     size_t nargs,
						     union brei_arg *args);

struct brei_interface {
	const char *name;
	uint32_t version;
	uint32_t nrequests;
	const struct brei_message *requests;
	uint32_t nevents;
	const struct brei_message *events;

	uint32_t nincoming;
	const struct brei_message *incoming;

	brei_event_dispatcher dispatcher;
};

struct brei_object {
	const struct brei_interface *interface;
	void *implementation; /* pointer to the actual object */
	object_id_t id;       /* protocol object id */
	uint32_t version;     /* protocol object interface version */
	struct list link;     /* in the ei/eis object list */
};

/**
 * Marshal the message for the given object id, opcode and signature into
 * a struct iobuf. On success, the returned result has that iobuf
 * as brei_result_get_data(), otherwise the result is the error.
 */
struct brei_result *
brei_marshal_message(struct brei_context *brei,
		     object_id_t id,
		     uint32_t opcode,
		     const char *signature,
		     size_t nargs,
		     va_list args);

/**
 * Dispatch messages for the data on fd.
 * The callback is called for each protocol message, parsing must be done by
 * the caller.
 *
 * On success, the callback must return the number of bytes consumed in msgdata
 * On failure, the callback must return a negative errno
 *
 * @return NULL on success or a struct with the error reason
 */
struct brei_result *
brei_dispatch(struct brei_context *brei,
	      int fd,
	      int (*lookup_object)(object_id_t object_id,
				   struct brei_object **object,
				   void *user_data),
	      void *user_data);

/**
 * Drain all pending content from the file descriptor and discard it.
 */
void
brei_drain_fd(int fd);

static inline bool
brei_is_server_id(object_id_t id)
{
	return id >= 0xff00000000000000;
}

static inline bool
brei_is_client_id(object_id_t id)
{
	return id != 0 && id < 0xff00000000000000;
}
