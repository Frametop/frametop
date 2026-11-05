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

#include <errno.h>
#include <stdbool.h>

#include "util-bits.h"
#include "util-io.h"
#include "util-macros.h"
#include "util-mem.h"
#include "util-strings.h"

#include "ei-proto.h"
#include "libei-private.h"

static void
ei_seat_destroy(struct ei_seat *seat)
{
	free(seat->name);
}

_public_ OBJECT_IMPLEMENT_REF(ei_seat);
_public_ OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_seat);

static OBJECT_IMPLEMENT_CREATE(ei_seat);
static OBJECT_IMPLEMENT_PARENT(ei_seat, ei);
_public_
OBJECT_IMPLEMENT_GETTER(ei_seat, name, const char *);
_public_
OBJECT_IMPLEMENT_SETTER(ei_seat, user_data, void *);
OBJECT_IMPLEMENT_GETTER_AS_REF(ei_seat, proto_object, const struct brei_object *);

_public_ struct ei *
ei_seat_get_context(struct ei_seat *seat)
{
	assert(seat);
	return ei_seat_parent(seat);
}

object_id_t
ei_seat_get_id(struct ei_seat *seat)
{
	return seat->proto_object.id;
}

static struct brei_result *
handle_msg_destroyed(struct ei_seat *seat, uint32_t serial)
{
	struct ei *ei = ei_seat_get_context(seat);

	ei_update_serial(ei, serial);

	log_debug(ei, "server removed seat %s", seat->name);
	ei_seat_remove(seat);

	return NULL;
}

static struct brei_result *
handle_msg_name(struct ei_seat *seat, const char *name)
{
	if (seat->name != NULL)
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "EIS sent the seat name twice");

	seat->name = xstrdup(name);

	return 0;
}

static struct brei_result *
handle_msg_capability(struct ei_seat *seat, uint64_t mask, const char *interface)
{
	struct ei *ei = ei_seat_get_context(seat);

	assert(ARRAY_LENGTH(EI_INTERFACE_NAMES) == ARRAY_LENGTH(seat->capabilities.map));

	for (size_t i = 0; i < ARRAY_LENGTH(EI_INTERFACE_NAMES); i++) {
		if (streq(EI_INTERFACE_NAMES[i], interface)) {
			if (seat->capabilities.map[i] != 0)
				return brei_result_new(
					EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					"EIS sent the seat capabilities for %s twice",
					interface);
			log_debug(ei,
				  "seat %#" PRIx64 " has cap %s as %#" PRIx64,
				  ei_seat_get_id(seat),
				  interface,
				  mask);
			seat->capabilities.map[i] = mask;
			return 0;
		}
	}

	/* EIS must not send anything we didn't announce as supported */
	return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
			       "EIS sent an unsupported interface %s",
			       interface);
}

static struct brei_result *
handle_msg_done(struct ei_seat *seat)
{
	struct ei *ei = ei_seat_get_context(seat);

	seat->state = EI_SEAT_STATE_DONE;
	log_debug(ei, "Added seat '%s'", seat->name);

	ei_queue_seat_added_event(seat);

	return NULL;
}

#define DISCONNECT_IF_INVALID_ID(seat_, id_) do { \
	if (!brei_is_server_id(id_)) { \
		struct ei *ei_ = ei_seat_get_context(seat_); \
		log_bug(ei_, "Received invalid object id %#" PRIx64 ". Disconnecting", id_); \
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL, "Received invalid object id %#" PRIx64 ".", id_); \
	} \
} while(0)

static struct brei_result *
handle_msg_device(struct ei_seat *seat, object_id_t id, uint32_t version)
{
	DISCONNECT_IF_INVALID_ID(seat, id);

	struct ei *ei = ei_seat_get_context(seat);

	DISCONNECT_IF_INVALID_VERSION(ei, ei_device, id, version);

	log_debug(ei, "Added device %#" PRIx64 "@v%u", id, version);
	/* device is in the seat's device list */
	struct ei_device *device = ei_device_new(seat, id, version);
	/* this list "owns" the ref for this device */
	list_append(&seat->devices, &device->link);

	return NULL;
}

static const struct ei_seat_interface interface = {
	.destroyed = handle_msg_destroyed,
	.name = handle_msg_name,
	.capability = handle_msg_capability,
	.done = handle_msg_done,
	.device = handle_msg_device,
};

const struct ei_seat_interface *
ei_seat_get_interface(struct ei_seat *seat)
{
	return &interface;
}

struct ei_seat *
ei_seat_new(struct ei *ei, object_id_t id, uint32_t version)
{
	struct ei_seat *seat = ei_seat_create(&ei->object);

	seat->proto_object.id = id;
	seat->proto_object.implementation = seat;
	seat->proto_object.interface = &ei_seat_proto_interface;
	seat->proto_object.version = version;
	ei_register_object(ei, &seat->proto_object);

	seat->state = EI_SEAT_STATE_NEW;
	seat->capabilities.bound = 0;

	list_init(&seat->devices);
	list_init(&seat->devices_removed);
	list_init(&seat->link);

	return seat; /* ref owned by caller */
}

void
ei_seat_remove(struct ei_seat *seat)
{
	/* Sigh, this is terrible and needs to be fixed:
	 * if our fd is broken, trying to send any event causes an ei_disconnect(),
	 * which eventually calls in here. So we need to guard this function
	 * against nested callers. */
	if (seat->state == EI_SEAT_STATE_REMOVED)
		return;

	struct ei_device *d;

	/* If the server disconnects us before processing a new device, we
	 * need to clean this up in the library */
	list_for_each_safe(d, &seat->devices, link) {
		/* remove the device */
		ei_device_close(d);
		/* And pretend to process the removed message from
		 * the server */
		ei_device_removed_by_server(d);
	}

	/* Check the seat state again, because the above device removal may
	 * have triggered ei_disconnect() */
	if (seat->state != EI_SEAT_STATE_REMOVED) {
		seat->state = EI_SEAT_STATE_REMOVED;
		list_remove(&seat->link);
		list_init(&seat->link);
		ei_queue_seat_removed_event(seat);

		struct ei *ei = ei_seat_get_context(seat);
		ei_unregister_object(ei, &seat->proto_object);

		ei_seat_unref(seat);
	}
}

_public_ bool
ei_seat_has_capability(struct ei_seat *seat, enum ei_device_capability cap)
{
	switch (cap) {
	case EI_DEVICE_CAP_POINTER:
		/* FIXME: a seat without pointer or pointer_absolute but button
		 * and/or scroll should count as pointer here but that's niche
		 * enough that we can figure that out when needed */
		return seat->capabilities.map[EI_POINTER_INTERFACE_INDEX] != 0;
	case EI_DEVICE_CAP_POINTER_ABSOLUTE:
		return seat->capabilities.map[EI_POINTER_ABSOLUTE_INTERFACE_INDEX] != 0;
	case EI_DEVICE_CAP_KEYBOARD:
		return seat->capabilities.map[EI_KEYBOARD_INTERFACE_INDEX] != 0;
	case EI_DEVICE_CAP_TOUCH:
		return seat->capabilities.map[EI_TOUCHSCREEN_INTERFACE_INDEX] != 0;
	case EI_DEVICE_CAP_SCROLL:
		return seat->capabilities.map[EI_SCROLL_INTERFACE_INDEX] != 0;
	case EI_DEVICE_CAP_BUTTON:
		return seat->capabilities.map[EI_BUTTON_INTERFACE_INDEX] != 0;
	case EI_DEVICE_CAP_TEXT:
		return seat->capabilities.map[EI_TEXT_INTERFACE_INDEX] != 0;
	}
	return false;
}

static int
ei_seat_send_bind(struct ei_seat *seat, uint64_t capabilities)
{
	struct ei *ei = ei_seat_get_context(seat);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	int rc = ei_seat_request_bind(seat, capabilities);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

static uint64_t
ei_seat_cap_mask(struct ei_seat *seat, enum ei_device_capability cap)
{
	switch (cap) {
	case EI_DEVICE_CAP_POINTER_ABSOLUTE:
		return seat->capabilities.map[EI_POINTER_ABSOLUTE_INTERFACE_INDEX];
	case EI_DEVICE_CAP_POINTER:
		return seat->capabilities.map[EI_POINTER_INTERFACE_INDEX];
	case EI_DEVICE_CAP_KEYBOARD:
		return seat->capabilities.map[EI_KEYBOARD_INTERFACE_INDEX];
	case EI_DEVICE_CAP_TOUCH:
		return seat->capabilities.map[EI_TOUCHSCREEN_INTERFACE_INDEX];
	case EI_DEVICE_CAP_BUTTON:
		return seat->capabilities.map[EI_BUTTON_INTERFACE_INDEX];
	case EI_DEVICE_CAP_SCROLL:
		return seat->capabilities.map[EI_SCROLL_INTERFACE_INDEX];
	case EI_DEVICE_CAP_TEXT:
		return seat->capabilities.map[EI_TEXT_INTERFACE_INDEX];
	}

	return 0;
}

_public_ void
ei_seat_bind_capabilities(struct ei_seat *seat, ...)
{
	switch (seat->state) {
	case EI_SEAT_STATE_DONE:
		break;
	case EI_SEAT_STATE_NEW:
	case EI_SEAT_STATE_REMOVED:
		return;
	}

	uint64_t mask = seat->capabilities.bound;
	enum ei_device_capability cap;

	va_list args;
	va_start(args, seat);
	while ((cap = va_arg(args, enum ei_device_capability)) > 0) {
		mask_add(mask, ei_seat_cap_mask(seat, cap));
	}
	va_end(args);

	if (seat->capabilities.bound == mask)
		return;

	seat->capabilities.bound = mask;
	ei_seat_send_bind(seat, seat->capabilities.bound);
}

_public_ void
ei_seat_unbind_capabilities(struct ei_seat *seat, ...)
{
	switch (seat->state) {
	case EI_SEAT_STATE_DONE:
		break;
	case EI_SEAT_STATE_NEW:
	case EI_SEAT_STATE_REMOVED:
		return;
	}

	uint64_t mask = seat->capabilities.bound;
	enum ei_device_capability cap;

	va_list args;
	va_start(args, seat);
	while ((cap = va_arg(args, enum ei_device_capability)) > 0) {
		mask_remove(mask, ei_seat_cap_mask(seat, cap));
	}
	va_end(args);

	if (seat->capabilities.bound == mask)
		return;

	seat->capabilities.bound = mask;
	if (seat->capabilities.bound == 0) {
		struct ei_device *device;
		list_for_each(device, &seat->devices, link) {
			if (ei_device_has_capability(device, cap))
				ei_device_close(device);
		}
	}

	ei_seat_send_bind(seat, seat->capabilities.bound);
}

_public_ void
ei_seat_request_device_with_capabilities(struct ei_seat *seat, ...)
{
	switch (seat->state) {
	case EI_SEAT_STATE_DONE:
		break;
	case EI_SEAT_STATE_NEW:
	case EI_SEAT_STATE_REMOVED:
		return;
	}

	uint64_t mask = 0;
	enum ei_device_capability cap;

	va_list args;
	va_start(args, seat);
	while ((cap = va_arg(args, enum ei_device_capability)) > 0) {
		mask_add(mask, ei_seat_cap_mask(seat, cap));
	}
	va_end(args);

	if (mask == 0)
		return;

	/* Check if requested capabilities are a subset of bound capabilities */
	if (!mask_all(seat->capabilities.bound, mask)) {
		struct ei *ei = ei_seat_get_context(seat);
		log_bug_client(ei,
			       "Requested capabilities are not a subset of the bound capabilities");
		return;
	}

	ei_seat_request_request_device(seat, mask);
}
