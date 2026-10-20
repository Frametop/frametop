/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Red Hat, Inc.
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

#include <stdio.h>

#include "util-list.h"
#include "util-macros.h"
#include "util-mem.h"
#include "util-object.h"

#include "brei-shared.h"
#include "libei-connection.h"
#include "libei-private.h"

struct ei_ping {
	struct object object;
	uint64_t id;
	void *user_data;

	struct ei *context;
	bool is_pending;
	bool is_done;
};

static void
ei_ping_destroy(struct ei_ping *ping)
{
	if (!ping->is_pending)
		ei_unref(ping->context);
}

static OBJECT_IMPLEMENT_CREATE(ei_ping);

_public_ OBJECT_IMPLEMENT_REF(ei_ping);
_public_ OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_ping);
_public_
OBJECT_IMPLEMENT_GETTER(ei_ping, id, uint64_t);
_public_
OBJECT_IMPLEMENT_GETTER(ei_ping, user_data, void *);
_public_
OBJECT_IMPLEMENT_SETTER(ei_ping, user_data, void *);
static OBJECT_IMPLEMENT_GETTER(ei_ping, context, struct ei *);

_public_ struct ei_ping *
ei_new_ping(struct ei *ei)
{
	static uint64_t id = 0;

	struct ei_ping *ping = ei_ping_create(NULL);
	ping->id = ++id;
	/* Ref our context while it's pending (i.e. only the caller has the ref).
	 * Once it's pending we no longer need the ref.
	 */
	ping->context = ei_ref(ei);
	ping->is_pending = false;
	ping->is_done = false;

	return ping;
}

static void
on_pong(struct ei_connection_sync_callback *callback)
{
	struct ei_ping *ping = ei_connection_sync_callback_get_user_data(callback);
	ping->is_done = true;

	struct ei *ei = ei_connection_sync_callback_get_context(callback);
	ei_queue_pong_event(ei, ping);
	/* ei_ping ref is removed in on_destroy */
}

static void
on_destroy(struct ei_connection_sync_callback *callback)
{
	/* This is only called if we never received a pong */
	_unref_(ei_ping) *ping = ei_connection_sync_callback_get_user_data(callback);

	/* We never got a pong because we got disconnected. Queue a fake pong event */
	if (!ping->is_done) {
		struct ei *ei = ei_connection_sync_callback_get_context(callback);
		ei_queue_pong_event(ei, ping);
	}
}

_public_ void
ei_ping(struct ei_ping *ping)
{
	struct ei *ei = ei_ping_get_context(ping);

	ei_unref(ping->context);
	ping->context = ei;
	ping->is_pending = true;

	_unref_(ei_connection_sync_callback) *cb =
		ei_connection_sync_callback_new(ei, on_pong, on_destroy, ei_ping_ref(ping));
	int rc = ei_connection_sync(ei->connection, cb);
	if (rc) {
		ping->is_done = true;
		ei_queue_pong_event(ei, ping);
		ei_disconnect(ei);
	}
}
