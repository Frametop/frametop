/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Red Hat, Inc.
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

#include <errno.h>
#include <stdbool.h>

#include "util-bits.h"
#include "util-io.h"
#include "util-macros.h"
#include "util-mem.h"
#include "util-strings.h"
#include "util-version.h"

#include "ei-proto.h"
#include "libei-private.h"

static void
ei_callback_destroy(struct ei_callback *callback)
{
	struct ei *ei = ei_callback_get_context(callback);
	ei_unregister_object(ei, &callback->proto_object);
}

OBJECT_IMPLEMENT_REF(ei_callback);
OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_callback);
OBJECT_IMPLEMENT_GETTER(ei_callback, user_data, void *);
OBJECT_IMPLEMENT_SETTER(ei_callback, user_data, void *);
OBJECT_IMPLEMENT_GETTER_AS_REF(ei_callback, proto_object, const struct brei_object *);

static OBJECT_IMPLEMENT_CREATE(ei_callback);
static OBJECT_IMPLEMENT_PARENT(ei_callback, ei);

struct ei *
ei_callback_get_context(struct ei_callback *callback)
{
	assert(callback);
	return ei_callback_parent(callback);
}

object_id_t
ei_callback_get_id(struct ei_callback *callback)
{
	return callback->proto_object.id;
}

uint32_t
ei_callback_get_version(struct ei_callback *callback)
{
	return callback->proto_object.version;
}

static struct brei_result *
handle_msg_done(struct ei_callback *callback, uint64_t callback_data)
{
	callback->func(callback, callback->callback_data, callback_data);
	return NULL;
}

static const struct ei_callback_interface interface = {
	.done = handle_msg_done,
};

const struct ei_callback_interface *
ei_callback_get_interface(struct ei_callback *callback)
{
	return &interface;
}

struct ei_callback *
ei_callback_new(struct ei *ei, ei_callback_func func, void *callback_data)
{
	struct ei_callback *callback = ei_callback_create(&ei->object);

	callback->proto_object.id = ei_get_new_id(ei);
	callback->proto_object.implementation = callback;
	callback->proto_object.interface = &ei_callback_proto_interface;
	callback->proto_object.version = VERSION_V(1);
	callback->callback_data = callback_data;
	ei_register_object(ei, &callback->proto_object);

	callback->func = func;
	list_init(&callback->link);

	return callback; /* ref owned by caller */
}
