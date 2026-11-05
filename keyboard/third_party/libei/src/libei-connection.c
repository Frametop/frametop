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
#include "libei-connection.h"
#include "libei-private.h"

static void
ei_connection_destroy(struct ei_connection *connection)
{
	struct ei *ei = ei_connection_get_context(connection);
	ei_unregister_object(ei, &connection->proto_object);

	/* should be a noop */
	ei_connection_remove_pending_callbacks(connection);
}

OBJECT_IMPLEMENT_REF(ei_connection);
OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_connection);

static OBJECT_IMPLEMENT_CREATE(ei_connection);
static OBJECT_IMPLEMENT_PARENT(ei_connection, ei);
OBJECT_IMPLEMENT_GETTER_AS_REF(ei_connection, proto_object, const struct brei_object *);

uint32_t
ei_connection_get_version(struct ei_connection *connection)
{
	return connection->proto_object.version;
}

object_id_t
ei_connection_get_id(struct ei_connection *connection)
{
	return connection->proto_object.id;
}

struct ei *
ei_connection_get_context(struct ei_connection *connection)
{
	assert(connection);
	return ei_connection_parent(connection);
}

const struct ei_connection_interface *
ei_connection_get_interface(struct ei_connection *connection)
{
	struct ei *ei = ei_connection_parent(connection);
	return ei_get_interface(ei);
}

struct ei_connection *
ei_connection_new(struct ei *ei, object_id_t id, uint32_t version)
{
	struct ei_connection *connection = ei_connection_create(&ei->object);

	connection->proto_object.id = id;
	connection->proto_object.implementation = connection;
	connection->proto_object.interface = &ei_connection_proto_interface;
	connection->proto_object.version = version;
	ei_register_object(ei, &connection->proto_object);

	list_init(&connection->pending_callbacks);

	return connection; /* ref owned by caller */
}

void
ei_connection_remove_pending_callbacks(struct ei_connection *connection)
{
	struct ei_callback *cb;
	list_for_each_safe(cb, &connection->pending_callbacks, link) {
		list_remove(&cb->link);
		ei_connection_sync_callback_unref(cb->user_data);
		ei_callback_unref(cb);
	}
}

static void
ei_connection_sync_callback_destroy(struct ei_connection_sync_callback *callback)
{
	if (callback->destroy)
		callback->destroy(callback);
}

static OBJECT_IMPLEMENT_CREATE(ei_connection_sync_callback);
OBJECT_IMPLEMENT_REF(ei_connection_sync_callback);
OBJECT_IMPLEMENT_UNREF(ei_connection_sync_callback);
OBJECT_IMPLEMENT_GETTER(ei_connection_sync_callback, user_data, void *);
static OBJECT_IMPLEMENT_PARENT(ei_connection_sync_callback, ei);

struct ei *
ei_connection_sync_callback_get_context(struct ei_connection_sync_callback *callback)
{
	return ei_connection_sync_callback_parent(callback);
}

struct ei_connection_sync_callback *
ei_connection_sync_callback_new(struct ei *ei,
				ei_connection_sync_callback_done_t done,
				ei_connection_sync_callback_destroy_t destroy,
				void *user_data)
{
	struct ei_connection_sync_callback *callback =
		ei_connection_sync_callback_create(&ei->object);
	callback->done = done;
	callback->destroy = destroy;
	callback->user_data = user_data;
	return callback;
}

static void
sync_callback(struct ei_callback *callback, void *callback_data, uint64_t proto_data)
{
	_unref_(ei_connection_sync_callback) *data = ei_callback_get_user_data(callback);
	if (data->done)
		data->done(data);

	/* remove from pending callbacks */
	list_remove(&callback->link);
	ei_callback_unref(callback);
}

int
ei_connection_sync(struct ei_connection *connection, struct ei_connection_sync_callback *callback)
{
	struct ei *ei = ei_connection_get_context(connection);

	/* This is double-wrapped because we only use this for debugging purposes for
	 * now. The actual callback calls sync_callback with our connection,
	 * then we extract the user_data on the object and call into the
	 * cb supplied to this function.
	 */
	struct ei_callback *cb = ei_callback_new(ei, sync_callback, connection);
	ei_callback_set_user_data(cb, ei_connection_sync_callback_ref(callback));
	list_append(&connection->pending_callbacks, &cb->link);

	return ei_connection_request_sync(connection,
					  ei_callback_get_id(cb),
					  ei_callback_get_version(cb));
}
