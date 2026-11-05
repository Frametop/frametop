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
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "util-io.h"
#include "util-macros.h"
#include "util-object.h"
#include "util-sources.h"
#include "util-strings.h"
#include "util-time.h"
#include "util-version.h"

#include "brei-shared.h"
#include "ei-proto.h"
#include "libei-private.h"
#include "libei.h"

_Static_assert(sizeof(enum ei_device_capability) == sizeof(int), "Invalid enum size");
_Static_assert(sizeof(enum ei_keymap_type) == sizeof(int), "Invalid enum size");
_Static_assert(sizeof(enum ei_event_type) == sizeof(int), "Invalid enum size");
_Static_assert(sizeof(enum ei_log_priority) == sizeof(int), "Invalid enum size");

static void
ei_unsent_free(struct ei_unsent *unsent);

static void
ei_defunct_object_free(struct ei_defunct_object *obj);

static void
ei_destroy(struct ei *ei)
{
	ei_disconnect(ei);

	struct ei_event *e;
	while ((e = ei_get_event(ei)) != NULL)
		ei_event_unref(e);

	struct ei_unsent *unsent;
	list_for_each_safe(unsent, &ei->unsent_queue, node) {
		ei_unsent_free(unsent);
	}

	if (ei->backend_interface.destroy)
		ei->backend_interface.destroy(ei, ei->backend);

	ei->backend = NULL;
	ei_handshake_unref(ei->handshake);
	ei_connection_unref(ei->connection);
	brei_context_unref(ei->brei);

	sink_unref(ei->sink);
	free(ei->name);

	struct ei_defunct_object *obj;
	list_for_each_safe(obj, &ei->defunct_objects, node) {
		ei_defunct_object_free(obj);
	}
}

static OBJECT_IMPLEMENT_CREATE(ei);
_public_ OBJECT_IMPLEMENT_REF(ei);
_public_ OBJECT_IMPLEMENT_UNREF_CLEANUP(ei);
_public_
OBJECT_IMPLEMENT_SETTER(ei, user_data, void *);
_public_
OBJECT_IMPLEMENT_GETTER(ei, user_data, void *);
OBJECT_IMPLEMENT_GETTER(ei, connection, struct ei_connection *);
OBJECT_IMPLEMENT_GETTER(ei, serial, uint32_t);

DEFINE_UNREF_CLEANUP_FUNC(brei_result);
DEFINE_UNREF_CLEANUP_FUNC(ei_pingpong);

struct ei *
ei_get_context(struct ei *ei)
{
	return ei; /* for the protocol bindings */
}

static struct ei *
ei_create_context(bool is_sender, void *user_data)
{
	_unref_(ei) *ei = ei_create(NULL);

	ei->state = EI_STATE_NEW;
	list_init(&ei->event_queue);
	list_init(&ei->seats);
	list_init(&ei->proto_objects);
	list_init(&ei->unsent_queue);
	list_init(&ei->defunct_objects);

	ei->interface_versions = (struct ei_interface_versions){
		.ei_connection = VERSION_V(1),
		.ei_handshake = VERSION_V(1),
		.ei_callback = VERSION_V(1),
		.ei_pingpong = VERSION_V(1),
		.ei_seat = VERSION_V(2),
		.ei_device = VERSION_V(3),
		.ei_pointer = VERSION_V(1),
		.ei_pointer_absolute = VERSION_V(1),
		.ei_scroll = VERSION_V(1),
		.ei_button = VERSION_V(1),
		.ei_keyboard = VERSION_V(1),
		.ei_text = VERSION_V(1),
		.ei_touchscreen = VERSION_V(2),
	};
	/* This must be v1 until the server tells us otherwise */
	ei->handshake = ei_handshake_new(ei, VERSION_V(1));
	ei->next_object_id = 1;
	ei->brei = brei_context_new(ei);
	brei_context_set_log_context(ei->brei, ei);
	brei_context_set_log_func(ei->brei, (brei_logfunc_t)ei_log_msg_va);

	ei_log_set_handler(ei, NULL);
	ei_log_set_priority(ei, EI_LOG_PRIORITY_INFO);
	ei->sink = sink_new();
	if (!ei->sink)
		return NULL;

	ei->user_data = user_data;
	ei->backend = NULL;
	ei->is_sender = is_sender;

	return steal(&ei);
}

object_id_t
ei_get_new_id(struct ei *ei)
{
	static const uint64_t server_range = 0xff00000000000000;
	return ei->next_object_id++ & ~server_range;
}

void
ei_update_serial(struct ei *ei, uint32_t serial)
{
	ei->serial = serial;
}

void
ei_register_object(struct ei *ei, struct brei_object *object)
{
	log_debug(ei,
		  "object %#" PRIx64 " registering %s v%u",
		  object->id,
		  object->interface->name,
		  object->version);
	list_append(&ei->proto_objects, &object->link);
}

static void
ei_defunct_object_free(struct ei_defunct_object *obj)
{
	list_remove(&obj->node);
	free(obj);
}

void
ei_unregister_object(struct ei *ei, struct brei_object *object)
{
	log_debug(ei,
		  "object %#" PRIx64 " deregistering %s v%u",
		  object->id,
		  object->interface->name,
		  object->version);
	list_remove(&object->link);
	struct ei_defunct_object *obj = xalloc(sizeof *obj);
	obj->object_id = object->id;
	obj->time = ei_now(ei);
	list_append(&ei->defunct_objects, &obj->node);
}

_public_ bool
ei_is_sender(struct ei *ei)
{
	return ei->is_sender;
}

_public_ struct ei *
ei_new(void *user_data)
{
	return ei_new_sender(user_data);
}

_public_ struct ei *
ei_new_sender(void *user_data)
{
	return ei_create_context(true, user_data);
}

_public_ struct ei *
ei_new_receiver(void *user_data)
{
	return ei_create_context(false, user_data);
}

_public_ int
ei_get_fd(struct ei *ei)
{
	return sink_get_fd(ei->sink);
}

_public_ void
ei_dispatch(struct ei *ei)
{
	sink_dispatch(ei->sink);
}

static void
update_event_timestamp(struct ei_event *event, uint64_t time)
{
	switch (event->type) {
	case EI_EVENT_POINTER_MOTION:
	case EI_EVENT_POINTER_MOTION_ABSOLUTE:
	case EI_EVENT_BUTTON_BUTTON:
	case EI_EVENT_SCROLL_DELTA:
	case EI_EVENT_SCROLL_STOP:
	case EI_EVENT_SCROLL_CANCEL:
	case EI_EVENT_SCROLL_DISCRETE:
	case EI_EVENT_KEYBOARD_KEY:
	case EI_EVENT_TOUCH_DOWN:
	case EI_EVENT_TOUCH_UP:
	case EI_EVENT_TOUCH_MOTION:
	case EI_EVENT_TEXT_KEYSYM:
	case EI_EVENT_TEXT_UTF8:
		if (event->timestamp != 0) {
			log_bug(ei_event_get_context(event),
				"Unexpected timestamp for event of type: %s",
				ei_event_type_to_string(event->type));
			return;
		}
		event->timestamp = time;
		break;
	default:
		log_bug(ei_event_get_context(event),
			"Unexpected event %s in pending queue event",
			ei_event_type_to_string(event->type));
		return;
	}
}

static void
queue_event(struct ei *ei, struct ei_event *event)
{
	struct ei_device *device = ei_event_get_device(event);
	struct list *queue = &ei->event_queue;
	const char *prefix = "";

	switch (event->type) {
	case EI_EVENT_POINTER_MOTION:
	case EI_EVENT_POINTER_MOTION_ABSOLUTE:
	case EI_EVENT_BUTTON_BUTTON:
	case EI_EVENT_SCROLL_DELTA:
	case EI_EVENT_SCROLL_STOP:
	case EI_EVENT_SCROLL_CANCEL:
	case EI_EVENT_SCROLL_DISCRETE:
	case EI_EVENT_KEYBOARD_KEY:
	case EI_EVENT_TOUCH_DOWN:
	case EI_EVENT_TOUCH_UP:
	case EI_EVENT_TOUCH_MOTION:
	case EI_EVENT_TEXT_KEYSYM:
	case EI_EVENT_TEXT_UTF8:
		prefix = "pending ";
		queue = &device->pending_event_queue;
		break;
	case EI_EVENT_FRAME:
		/* silently discard empty frames */
		if (list_empty(&device->pending_event_queue)) {
			ei_event_unref(event);
			return;
		}

		struct ei_event *pending;
		list_for_each_safe(pending, &device->pending_event_queue, link) {
			update_event_timestamp(pending, event->timestamp);
			list_remove(&pending->link);
			list_append(&ei->event_queue, &pending->link);
		}
		break;
	default:
		if (device && !list_empty(&device->pending_event_queue))
			ei_queue_frame_event(device, ei_now(ei));
		break;
	}

	log_debug(ei,
		  "queuing %sevent type %s (%u)",
		  prefix,
		  ei_event_type_to_string(event->type),
		  event->type);

	list_append(queue, &event->link);
}

void
ei_queue_connect_event(struct ei *ei)
{
	struct ei_event *e = ei_event_new(ei);
	e->type = EI_EVENT_CONNECT;

	queue_event(ei, e);
}

void
ei_queue_disconnect_event(struct ei *ei)
{
	struct ei_event *e = ei_event_new(ei);
	e->type = EI_EVENT_DISCONNECT;

	queue_event(ei, e);
}

void
ei_queue_pong_event(struct ei *ei, struct ei_ping *ping)
{
	struct ei_event *e = ei_event_new(ei);
	e->type = EI_EVENT_PONG;
	e->pong.ping = ei_ping_ref(ping);

	queue_event(ei, e);
}

void
ei_sync_event_send_done(struct ei_event *e)
{
	struct ei *ei = ei_event_get_context(e);
	_unref_(ei_pingpong) *pp = steal(&e->sync.pingpong);
	log_debug(ei_event_get_context(e),
		  "object %#" PRIx64 ": ping pong done",
		  ei_pingpong_get_id(pp));

	if (ei->state < EI_STATE_DISCONNECTED)
		ei_pingpong_request_done(pp, 0);
}

void
ei_queue_sync_event(struct ei *ei, struct ei_pingpong *ping)
{
	struct ei_event *e = ei_event_new(ei);
	e->type = EI_EVENT_SYNC;
	e->sync.pingpong = ei_pingpong_ref(ping);

	queue_event(ei, e);
}

void
ei_queue_seat_added_event(struct ei_seat *seat)
{
	struct ei *ei = ei_seat_get_context(seat);

	struct ei_event *e = ei_event_new(ei);
	e->type = EI_EVENT_SEAT_ADDED;
	e->seat = ei_seat_ref(seat);

	queue_event(ei, e);
}

void
ei_queue_seat_removed_event(struct ei_seat *seat)
{
	struct ei *ei = ei_seat_get_context(seat);

	struct ei_event *e = ei_event_new(ei);
	e->type = EI_EVENT_SEAT_REMOVED;
	e->seat = ei_seat_ref(seat);

	queue_event(ei, e);
}

static void
queue_device_added_event(struct ei_device *device)
{
	struct ei *ei = ei_device_get_context(device);
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_DEVICE_ADDED;

	queue_event(ei, e);
}

static void
queue_device_removed_event(struct ei_device *device)
{
	struct ei *ei = ei_device_get_context(device);
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_DEVICE_REMOVED;

	queue_event(ei, e);
}

void
ei_queue_device_paused_event(struct ei_device *device)
{
	struct ei *ei = ei_device_get_context(device);
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_DEVICE_PAUSED;

	queue_event(ei, e);
}

void
ei_queue_device_resumed_event(struct ei_device *device)
{
	struct ei *ei = ei_device_get_context(device);
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_DEVICE_RESUMED;

	queue_event(ei, e);
}

void
ei_queue_keyboard_modifiers_event(struct ei_device *device, const struct ei_xkb_modifiers *mods)
{
	struct ei *ei = ei_device_get_context(device);
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_KEYBOARD_MODIFIERS;
	e->modifiers = *mods;

	queue_event(ei, e);
}

void
ei_queue_frame_event(struct ei_device *device, uint64_t time)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_FRAME;
	e->timestamp = time;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_device_start_emulating_event(struct ei_device *device, uint32_t sequence)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_DEVICE_START_EMULATING;
	e->start_emulating.sequence = sequence;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_device_stop_emulating_event(struct ei_device *device)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_DEVICE_STOP_EMULATING;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_pointer_rel_event(struct ei_device *device, double dx, double dy)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_POINTER_MOTION;
	e->pointer.dx = dx;
	e->pointer.dy = dy;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_pointer_abs_event(struct ei_device *device, double x, double y)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_POINTER_MOTION_ABSOLUTE;
	e->pointer.absx = x;
	e->pointer.absy = y;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_pointer_button_event(struct ei_device *device, uint32_t button, bool is_press)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_BUTTON_BUTTON;
	e->pointer.button = button;
	e->pointer.button_is_press = is_press;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_pointer_scroll_event(struct ei_device *device, double x, double y)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_SCROLL_DELTA;
	e->pointer.sx = x;
	e->pointer.sy = y;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_pointer_scroll_discrete_event(struct ei_device *device, int32_t x, int32_t y)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_SCROLL_DISCRETE;
	e->pointer.sdx = x;
	e->pointer.sdy = y;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_pointer_scroll_stop_event(struct ei_device *device, bool x, bool y)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_SCROLL_STOP;
	e->pointer.stop_x = x;
	e->pointer.stop_y = y;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_pointer_scroll_cancel_event(struct ei_device *device, bool x, bool y)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_SCROLL_CANCEL;
	e->pointer.stop_x = x;
	e->pointer.stop_y = y;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_keyboard_key_event(struct ei_device *device, uint32_t key, bool is_press)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_KEYBOARD_KEY;
	e->keyboard.key = key;
	e->keyboard.key_is_press = is_press;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_touch_down_event(struct ei_device *device, uint32_t touchid, double x, double y)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_TOUCH_DOWN;
	e->touch.touchid = touchid;
	e->touch.x = x;
	e->touch.y = y;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_touch_motion_event(struct ei_device *device, uint32_t touchid, double x, double y)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_TOUCH_MOTION;
	e->touch.touchid = touchid;
	e->touch.x = x;
	e->touch.y = y;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_touch_up_event(struct ei_device *device, uint32_t touchid)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_TOUCH_UP;
	e->touch.touchid = touchid;
	e->touch.is_cancel = false;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_touch_cancel_event(struct ei_device *device, uint32_t touchid)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_TOUCH_UP;
	e->touch.touchid = touchid;
	e->touch.is_cancel = true;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_text_keysym_event(struct ei_device *device, uint32_t keysym, bool is_press)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_TEXT_KEYSYM;
	e->text.keysym = keysym;
	e->text.is_press = is_press;

	queue_event(ei_device_get_context(device), e);
}

void
ei_queue_text_utf8_event(struct ei_device *device, const char *utf8)
{
	struct ei_event *e = ei_event_new_for_device(device);

	e->type = EI_EVENT_TEXT_UTF8;
	e->text.utf8 = xstrdup(utf8);

	queue_event(ei_device_get_context(device), e);
}

_public_ void
ei_disconnect(struct ei *ei)
{
	if (ei->state == EI_STATE_DISCONNECTED || ei->state == EI_STATE_DISCONNECTING)
		return;

	enum ei_state state = ei->state;

	/* We need the disconnecting state to be re-entrant
	   ei_device_remove() may call ei_disconnect() on a socket error */
	ei->state = EI_STATE_DISCONNECTING;

	struct ei_seat *seat;
	list_for_each_safe(seat, &ei->seats, link) {
		ei_seat_remove(seat);
	}

	if (state != EI_STATE_NEW && state != EI_STATE_BACKEND) {
		ei_connection_request_disconnect(ei->connection);
		ei_connection_remove_pending_callbacks(ei->connection);
	}
	ei_queue_disconnect_event(ei);
	ei->state = EI_STATE_DISCONNECTED;
	if (ei->source)
		source_remove(ei->source);
	ei->source = source_unref(ei->source);
}

#define DISCONNECT_IF_INVALID_ID(connection_, id_) do { \
	if (!brei_is_server_id(id_)) { \
		struct ei *ei_ = ei_connection_get_context(connection_); \
		log_bug(ei_, "Received invalid object id %#" PRIx64 ". Disconnecting", id_); \
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL, "Received invalid object id %#" PRIx64 ".", id_); \
	} \
} while(0)

static struct brei_result *
handle_msg_seat(struct ei_connection *connection, object_id_t seat_id, uint32_t version)
{
	DISCONNECT_IF_INVALID_ID(connection, seat_id);

	struct ei *ei = ei_connection_get_context(connection);

	DISCONNECT_IF_INVALID_VERSION(ei, ei_seat, seat_id, version);

	struct ei_seat *seat = ei_seat_new(ei, seat_id, version);

	/* We might get the seat event before our callback finished, so make sure
	 * we know we're connected
	 */
	ei_connected(ei);

	/* seats list owns the ref */
	list_append(&ei->seats, &seat->link);

	return NULL;
}

void
ei_queue_device_removed_event(struct ei_device *device)
{
	queue_device_removed_event(device);
}

void
ei_queue_device_added_event(struct ei_device *device)
{
	queue_device_added_event(device);
}

_public_ struct ei_event *
ei_get_event(struct ei *ei)
{
	if (list_empty(&ei->event_queue))
		return NULL;

	struct ei_event *e = list_first_entry(&ei->event_queue, e, link);
	list_remove(&e->link);

	return e;
}

_public_ struct ei_event *
ei_peek_event(struct ei *ei)
{
	if (list_empty(&ei->event_queue))
		return NULL;

	struct ei_event *e = list_first_entry(&ei->event_queue, e, link);
	return ei_event_ref(e);
}

void
ei_connected(struct ei *ei)
{
	if (ei->state == EI_STATE_CONNECTING) {
		ei->state = EI_STATE_CONNECTED;
		ei_queue_connect_event(ei);
	}
}

static struct brei_result *
handle_msg_disconnected(struct ei_connection *connection,
			uint32_t last_serial,
			uint32_t reason,
			const char *explanation)
{
	struct ei *ei = ei_connection_get_context(connection);

	if (reason == EI_CONNECTION_DISCONNECT_REASON_DISCONNECTED) {
		log_info(ei, "Disconnected by EIS");
		/* We got disconnected, disconnect our source because whatever
		 * we'd receive after this is garbage and the server won't
		 *  want to hear anything from us anyway. */
		source_remove(ei->source);
		ei_disconnect(ei);
		return NULL;
	} else {
		log_info(ei, "Disconnected after error: %s", explanation);
		return brei_result_new(reason, "%s", explanation);
	}
}

static struct brei_result *
handle_msg_invalid_object(struct ei_connection *connection,
			  uint32_t last_serial,
			  object_id_t object_id)
{
	struct ei *ei = ei_connection_get_context(connection);

	/* The protocol is async, so what can happen is:
	 *
	 * server sends A->destroyed()
	 *     client sends A->foo()
	 *     client receives A->destroyed()
	 * server receives A->foo()
	 * server sends invalid_object_id(A)
	 *     client receives invalid_object_id(A)
	 *
	 * This is expected and we shouldn't have a problem with that.
	 */

	struct ei_defunct_object *defunct;
	list_for_each_safe(defunct, &ei->defunct_objects, node) {
		if (defunct->object_id == object_id)
			return NULL;
	}

	log_bug(ei,
		"Invalid object %#" PRIx64 " after serial %u, I don't yet know how to handle that",
		object_id,
		last_serial);

	return NULL;
}

static struct brei_result *
handle_msg_ping(struct ei_connection *connection, object_id_t id, uint32_t version)
{
	DISCONNECT_IF_INVALID_ID(connection, id);

	struct ei *ei = ei_connection_get_context(connection);

	DISCONNECT_IF_INVALID_VERSION(ei, ei_pingpong, id, version);

	_unref_(ei_pingpong) *pingpong = ei_pingpong_new_for_id(ei, id, version);
	ei_queue_sync_event(ei_connection_get_context(connection), pingpong);

	return NULL;
}

static const struct ei_connection_interface interface = {
	.disconnected = handle_msg_disconnected,
	.seat = handle_msg_seat,
	.invalid_object = handle_msg_invalid_object,
	.ping = handle_msg_ping,
};

const struct ei_connection_interface *
ei_get_interface(struct ei *ei)
{
	return &interface;
}

static int
lookup_object(object_id_t object_id, struct brei_object **object, void *userdata)
{
	struct ei *ei = userdata;

	struct brei_object *obj;
	list_for_each(obj, &ei->proto_objects, link) {
		if (obj->id == object_id) {
			*object = obj;
			return 0;
		}
	}

	log_debug(ei, "Failed to find object %#" PRIx64 "", object_id);

	return -ENOENT;
}

static void
connection_dispatch(struct source *source, void *userdata)
{
	static uint8_t cleanup;
	struct ei *ei = userdata;
	enum ei_state old_state = ei->state;

	/* Flush any pending writes, if we have them */
	int rc = ei_unsent_flush(ei);
	if (rc < 0 && rc != -EAGAIN) {
		log_warn(ei, "Error flushing unsent queue: %s", strerror(-rc));
		ei_disconnect(ei);
	} else {
		_unref_(brei_result) *result =
			brei_dispatch(ei->brei, source_get_fd(source), lookup_object, ei);
		if (result) {
			log_warn(ei, "Connection error: %s", brei_result_get_explanation(result));
			brei_drain_fd(source_get_fd(source));
			ei_disconnect(ei);
		} else if (++cleanup % 20 == 0) {
			uint64_t now = ei_now(ei);
			struct ei_defunct_object *defunct;

			list_for_each_safe(defunct, &ei->defunct_objects, node) {
				/* Drop defunct objects after 5s */
				if (now - defunct->time < s2us(5))
					break;
				ei_defunct_object_free(defunct);
			}
		}
	}

	static const char *states[] = {
		"NEW", "BACKEND", "CONNECTING", "CONNECTED", "DISCONNECTED", "DISCONNECTING",
	};

	if (old_state != ei->state)
		log_debug(ei,
			  "Connection dispatch: %s -> %s",
			  states[old_state],
			  states[ei->state]);
}

static void
ei_queue_unsent(struct ei *ei, struct source *source, struct iobuf *buf)
{
	if (list_empty(&ei->unsent_queue)) {
		source_enable_write(source, true);
	}

	struct ei_unsent *unsent = xalloc(sizeof *unsent);
	unsent->buf = buf;
	list_append(&ei->unsent_queue, &unsent->node);
}

static void
ei_unsent_free(struct ei_unsent *unsent)
{
	list_remove(&unsent->node);
	iobuf_free(unsent->buf);
	free(unsent);
}

int
ei_unsent_flush(struct ei *ei)
{
	if (list_empty(&ei->unsent_queue))
		return 0;

	struct source *source = ei->source;
	int fd = source_get_fd(source);
	struct ei_unsent *unsent;
	list_for_each_safe(unsent, &ei->unsent_queue, node) {
		int rc = iobuf_send(unsent->buf, fd);
		if (rc < 0)
			return rc;
		ei_unsent_free(unsent);
	}
	source_enable_write(source, false);
	return 0;
}

int
ei_send_message(struct ei *ei,
		const struct brei_object *object,
		uint32_t opcode,
		const char *signature,
		size_t nargs,
		...)
{
	log_debug(ei,
		  "object %#" PRIx64 " sending: (%s@v%u:%s(%u)) signature '%s'",
		  object->id,
		  object->interface->name,
		  object->interface->version,
		  object->interface->requests[opcode].name,
		  opcode,
		  signature);

	va_list args;
	va_start(args, nargs);
	_unref_(brei_result) *result =
		brei_marshal_message(ei->brei, object->id, opcode, signature, nargs, args);
	va_end(args);

	if (brei_result_get_reason(result) != 0) {
		log_warn(ei, "failed to marshal message: %s", brei_result_get_explanation(result));
		return -EBADMSG;
	}

	_cleanup_iobuf_ struct iobuf *buf = brei_result_get_data(result);
	assert(buf);
	int fd = source_get_fd(ei->source);
	int rc = -EPIPE;
	if (fd != -1) {
		rc = ei_unsent_flush(ei);
		if (rc >= 0)
			rc = iobuf_send(buf, fd);
		if (rc == -EAGAIN) {
			ei_queue_unsent(ei, ei->source, steal(&buf));
			rc = 0;
		} else if (rc < 0) {
			log_warn(ei, "failed to send message: %s", strerror(-rc));
			source_remove(ei->source);
		}
	}
	return rc < 0 ? rc : 0;
}

int
ei_set_socket(struct ei *ei, int fd)
{
	struct source *source = source_new(fd, connection_dispatch, ei);
	int rc = sink_add_source(ei->sink, source);
	if (rc == 0) {
		ei->source = source_ref(source);
		ei->state = EI_STATE_BACKEND;

		/* The server SHOULD have already sent the handshake
		 * version, let's process that. If not ready, it'll happen
		 * in the next dispatch.
		 *
		 * FIXME: this will block if O_NONBLOCK is missing
		 */
		ei_dispatch(ei);
	}

	source_unref(source);

	return rc < 0 ? rc : 0;
}

_public_ void
ei_configure_name(struct ei *ei, const char *name)
{
	if (ei->state != EI_STATE_NEW) {
		log_bug_client(ei, "Client is already connected");
		return;
	}

	if (strlen(name) > 1024) {
		log_bug_client(ei, "Client name too long");
		return;
	}

	free(ei->name);
	ei->name = xstrdup(name);
}

_public_ void
ei_clock_set_now_func(struct ei *ei, ei_clock_now_func func)
{
	ei->clock_now = func;
}

_public_ uint64_t
ei_now(struct ei *ei)
{
	uint64_t ts = 0;

	if (ei->clock_now)
		ts = ei->clock_now(ei);
	else {
		int rc = now(&ts);
		if (rc < 0) {
			/* We should probably disconnect here but the chances of this
			 * happening are so slim it's not worth worrying about. Plus,
			 * if this fails we're likely to be inside eis_device_frame()
			 * so we should flush a frame event before disconnecting and... */
			log_error(ei, "clock_gettime failed: %s", strerror(-rc));
		}
	}

	return ts;
}

#ifdef _enable_tests_
#include "util-munit.h"

MUNIT_TEST(test_init_unref)
{
	struct ei *ei = ei_new(NULL);

	munit_assert_int(ei->state, ==, EI_STATE_NEW);
	munit_assert(list_empty(&ei->event_queue));
	munit_assert(list_empty(&ei->seats));

	munit_assert_not_null(ei->sink);

	struct ei *refd = ei_ref(ei);
	munit_assert_ptr_equal(ei, refd);
	munit_assert_int(ei->object.refcount, ==, 2);

	struct ei *unrefd = ei_unref(ei);
	munit_assert_null(unrefd);

	unrefd = ei_unref(ei);
	munit_assert_null(unrefd);

	return MUNIT_OK;
}

MUNIT_TEST(test_configure_name)
{
	struct ei *ei = ei_new(NULL);

	ei_configure_name(ei, "foo");
	munit_assert_string_equal(ei->name, "foo");
	ei_configure_name(ei, "bar");
	munit_assert_string_equal(ei->name, "bar");

	/* ignore names that are too long */
	char buf[1200] = { 0 };
	memset(buf, 'a', sizeof(buf) - 1);
	ei_configure_name(ei, buf);
	munit_assert_string_equal(ei->name, "bar");

	/* ignore names in all other states */
	for (enum ei_state state = EI_STATE_NEW + 1; state <= EI_STATE_DISCONNECTED; state++) {
		ei->state = state;
		ei_configure_name(ei, "expect ignored");
		munit_assert_string_equal(ei->name, "bar");
	}

	ei_unref(ei);

	return MUNIT_OK;
}
#endif
