
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

#include "util-list.h"
#include "util-mem.h"
#include "util-object.h"

#include "brei-shared.h"

struct ei;
struct ei_connection;
struct ei_connection_sync_callback;

/* This is a protocol-only object, not exposed in the API */
struct ei_connection {
	struct object object;
	struct brei_object proto_object;

	/* struct ei_connection_sync_callback */
	struct list pending_callbacks;
};

OBJECT_DECLARE_GETTER(ei_connection, context, struct ei *);
OBJECT_DECLARE_GETTER(ei_connection, version, uint32_t);
OBJECT_DECLARE_GETTER(ei_connection, id, object_id_t);
OBJECT_DECLARE_GETTER(ei_connection, proto_object, const struct brei_object *);
OBJECT_DECLARE_GETTER(ei_connection, interface, const struct ei_connection_interface *);
OBJECT_DECLARE_REF(ei_connection);
OBJECT_DECLARE_UNREF(ei_connection);

struct ei_connection *
ei_connection_new(struct ei *ei, object_id_t id, uint32_t version);

void
ei_connection_remove_pending_callbacks(struct ei_connection *connection);

/**
 * Called when the ei_callback.done event is received after
 * an ei_connection_sync() request.
 */
typedef void (*ei_connection_sync_callback_done_t)(struct ei_connection_sync_callback *callback);

/**
 * Called for each registered callback when the last reference to it is
 * destroyed. This should be used to clean up user_data, if need be.
 *
 * This function is always called, even if disconnected and the done() function is never called.
 */
typedef void (*ei_connection_sync_callback_destroy_t)(struct ei_connection_sync_callback *callback);

struct ei_connection_sync_callback {
	struct object object; /* parent is struct ei */
	ei_connection_sync_callback_done_t done;
	ei_connection_sync_callback_destroy_t destroy;
	void *user_data;
};

struct ei_connection_sync_callback *
ei_connection_sync_callback_new(struct ei *ei,
				ei_connection_sync_callback_done_t done,
				ei_connection_sync_callback_destroy_t destroy,
				void *user_data);

OBJECT_DECLARE_REF(ei_connection_sync_callback);
OBJECT_DECLARE_UNREF(ei_connection_sync_callback);
OBJECT_DECLARE_GETTER(ei_connection_sync_callback, context, struct ei *);
OBJECT_DECLARE_GETTER(ei_connection_sync_callback, user_data, void *);
DEFINE_UNREF_CLEANUP_FUNC(ei_connection_sync_callback);

int
ei_connection_sync(struct ei_connection *connection, struct ei_connection_sync_callback *callback);
