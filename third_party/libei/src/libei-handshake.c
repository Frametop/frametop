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
ei_handshake_destroy(struct ei_handshake *handshake)
{
	struct ei *ei = ei_handshake_get_context(handshake);
	ei_unregister_object(ei, &handshake->proto_object);
}

OBJECT_IMPLEMENT_REF(ei_handshake);
OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_handshake);
OBJECT_IMPLEMENT_GETTER(ei_handshake, user_data, void *);
OBJECT_IMPLEMENT_SETTER(ei_handshake, user_data, void *);
OBJECT_IMPLEMENT_GETTER_AS_REF(ei_handshake, proto_object, const struct brei_object *);

static OBJECT_IMPLEMENT_CREATE(ei_handshake);
static OBJECT_IMPLEMENT_PARENT(ei_handshake, ei);

struct ei *
ei_handshake_get_context(struct ei_handshake *handshake)
{
	assert(handshake);
	return ei_handshake_parent(handshake);
}

uint32_t
ei_handshake_get_version(struct ei_handshake *handshake)
{
	return handshake->proto_object.version;
}

static int
ei_handshake_initialize(struct ei_handshake *setup, uint32_t version)
{
	struct ei *ei = ei_handshake_get_context(setup);
	struct ei_interface_versions *v = &ei->interface_versions;

	ei_handshake_request_handshake_version(setup, v->ei_handshake);

	if (version >= EI_HANDSHAKE_REQUEST_CONTEXT_TYPE_SINCE_VERSION)
		ei_handshake_request_context_type(setup,
						  ei->is_sender
							  ? EI_HANDSHAKE_CONTEXT_TYPE_SENDER
							  : EI_HANDSHAKE_CONTEXT_TYPE_RECEIVER);

	if (version >= EI_HANDSHAKE_REQUEST_NAME_SINCE_VERSION && ei->name != NULL)
		ei_handshake_request_name(setup, ei->name);

	if (version >= EI_HANDSHAKE_REQUEST_INTERFACE_VERSION_SINCE_VERSION) {
		ei_handshake_request_interface_version(setup,
						       EI_CONNECTION_INTERFACE_NAME,
						       v->ei_connection);
		ei_handshake_request_interface_version(setup,
						       EI_CALLBACK_INTERFACE_NAME,
						       v->ei_callback);
		ei_handshake_request_interface_version(setup,
						       EI_PINGPONG_INTERFACE_NAME,
						       v->ei_pingpong);
		ei_handshake_request_interface_version(setup, EI_SEAT_INTERFACE_NAME, v->ei_seat);
		ei_handshake_request_interface_version(setup,
						       EI_DEVICE_INTERFACE_NAME,
						       v->ei_device);
		ei_handshake_request_interface_version(setup,
						       EI_POINTER_INTERFACE_NAME,
						       v->ei_pointer);
		ei_handshake_request_interface_version(setup,
						       EI_POINTER_ABSOLUTE_INTERFACE_NAME,
						       v->ei_pointer_absolute);
		ei_handshake_request_interface_version(setup,
						       EI_SCROLL_INTERFACE_NAME,
						       v->ei_scroll);
		ei_handshake_request_interface_version(setup,
						       EI_BUTTON_INTERFACE_NAME,
						       v->ei_button);
		ei_handshake_request_interface_version(setup,
						       EI_KEYBOARD_INTERFACE_NAME,
						       v->ei_keyboard);
		ei_handshake_request_interface_version(setup,
						       EI_TOUCHSCREEN_INTERFACE_NAME,
						       v->ei_touchscreen);
		ei_handshake_request_interface_version(setup, EI_TEXT_INTERFACE_NAME, v->ei_text);
	}

	ei_handshake_request_finish(setup);

	return 0;
}

static struct brei_result *
handle_msg_handshake_version(struct ei_handshake *setup, uint32_t version)
{
	struct ei *ei = ei_handshake_get_context(setup);
	struct ei_interface_versions *v = &ei->interface_versions;

	uint32_t min_version = min(version, ei->interface_versions.ei_handshake);
	v->ei_handshake = min_version;

	/* Now upgrade our protocol object to the server version (if applicable) */
	setup->proto_object.version = min_version;

	/* Now send all the bits we need to send */
	ei_handshake_initialize(setup, min_version);

	return NULL;
}

static struct brei_result *
handle_msg_interface_version(struct ei_handshake *setup, const char *name, uint32_t version)
{
	struct ei *ei = ei_handshake_get_context(setup);
	struct ei_interface_versions *v = &ei->interface_versions;

	if (streq(name, EI_HANDSHAKE_INTERFACE_NAME)) {
		/* EIS shouldn't send this anyway, let's ignore this */
	}
#define VERSION_UPDATE(iface_) if (streq(name, #iface_)) v->iface_ = min(version, v->iface_);
	/* clang-format off */
	else VERSION_UPDATE(ei_connection)
	else VERSION_UPDATE(ei_callback)
	else VERSION_UPDATE(ei_pingpong)
	else VERSION_UPDATE(ei_seat)
	else VERSION_UPDATE(ei_device)
	else VERSION_UPDATE(ei_pointer)
	else VERSION_UPDATE(ei_pointer_absolute)
	else VERSION_UPDATE(ei_scroll)
	else VERSION_UPDATE(ei_button)
	else VERSION_UPDATE(ei_keyboard)
	else VERSION_UPDATE(ei_touchscreen)
	else VERSION_UPDATE(ei_text)
	/* clang-format on */

#undef VERSION_UPDATE

		return NULL;
}

static void
on_connected(struct ei_connection_sync_callback *callback)
{
	struct ei *ei = ei_connection_sync_callback_get_context(callback);

	/* If we get here, the server didn't immediately disconnect us */
	if (ei->state == EI_STATE_DISCONNECTED)
		return;

	ei_connected(ei);
}

static struct brei_result *
handle_msg_connection(struct ei_handshake *setup, uint32_t serial, object_id_t id, uint32_t version)
{
	struct ei *ei = ei_handshake_get_context(setup);
	assert(setup == ei->handshake);
	/* we're done with our handshake, drop it */
	ei_handshake_unref(steal(&ei->handshake));

	DISCONNECT_IF_INVALID_VERSION(ei, ei_handshake, id, version);

	ei->connection = ei_connection_new(ei, id, version);
	ei->state = EI_STATE_CONNECTING;
	ei_update_serial(ei, serial);

	_unref_(ei_connection_sync_callback) *cb =
		ei_connection_sync_callback_new(ei, on_connected, NULL, NULL);
	/* Send a sync on the connection - EIS should immediately send a
	 * disconnect event where applicable, so if we get through to our
	 * sync callback, we didn't immediately get disconnected */
	ei_connection_sync(ei->connection, cb);

	return NULL;
}

static const struct ei_handshake_interface interface = {
	.handshake_version = handle_msg_handshake_version,
	.interface_version = handle_msg_interface_version,
	.connection = handle_msg_connection,
};

const struct ei_handshake_interface *
ei_handshake_get_interface(struct ei_handshake *handshake)
{
	return &interface;
}

struct ei_handshake *
ei_handshake_new(struct ei *ei, uint32_t version)
{
	struct ei_handshake *handshake = ei_handshake_create(&ei->object);

	handshake->proto_object.id = ei_get_new_id(ei);
	assert(handshake->proto_object.id == 0); /* Special object */
	handshake->proto_object.implementation = handshake;
	handshake->proto_object.interface = &ei_handshake_proto_interface;
	handshake->proto_object.version = version;
	ei_register_object(ei, &handshake->proto_object);

	return handshake; /* ref owned by caller */
}
