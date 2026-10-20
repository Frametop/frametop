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
#include <math.h>

#include "util-bits.h"
#include "util-io.h"
#include "util-macros.h"
#include "util-mem.h"
#include "util-strings.h"
#include "util-time.h"

#include "ei-proto.h"
#include "libei-private.h"

DEFINE_UNREF_CLEANUP_FUNC(ei_region);

static const char *
ei_device_state_to_string(enum ei_device_state state)
{
	switch (state) {
	CASE_RETURN_STRING(EI_DEVICE_STATE_NEW);
	CASE_RETURN_STRING(EI_DEVICE_STATE_PAUSED);
	CASE_RETURN_STRING(EI_DEVICE_STATE_RESUMED);
	CASE_RETURN_STRING(EI_DEVICE_STATE_EMULATING);
	CASE_RETURN_STRING(EI_DEVICE_STATE_REMOVED_FROM_CLIENT);
	CASE_RETURN_STRING(EI_DEVICE_STATE_REMOVED_FROM_SERVER);
	CASE_RETURN_STRING(EI_DEVICE_STATE_DEAD);
	}
	assert(!"Unhandled device state");
}

static void
ei_device_set_state(struct ei_device *device, enum ei_device_state state)
{
	enum ei_device_state old_state = device->state;
	device->state = state;
	log_debug(ei_device_get_context(device),
		  "device %#" PRIx64 ": %s → %s",
		  ei_device_get_id(device),
		  ei_device_state_to_string(old_state),
		  ei_device_state_to_string(state));
}

static void
ei_device_destroy(struct ei_device *device)
{
	struct ei_seat *seat = ei_device_get_seat(device);
	struct ei_region *region;
	struct ei_event *event;

	assert(device->state == EI_DEVICE_STATE_DEAD);

	list_for_each_safe(region, &device->regions, link)
		ei_region_unref(region);

	list_for_each_safe(event, &device->pending_event_queue, link) {
		list_remove(&event->link);
		ei_event_unref(event);
	}

	list_remove(&device->link);
	ei_keymap_unref(device->keymap);
	ei_pointer_unref(device->pointer);
	ei_pointer_absolute_unref(device->pointer_absolute);
	ei_scroll_unref(device->scroll);
	ei_button_unref(device->button);
	ei_touchscreen_unref(device->touchscreen);
	ei_keyboard_unref(device->keyboard);
	ei_text_unref(device->text);
	ei_seat_unref(seat);
	free(device->name);
	free(device->pending_region_mapping_id);
}

_public_ OBJECT_IMPLEMENT_REF(ei_device);
_public_ OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_device);

static OBJECT_IMPLEMENT_CREATE(ei_device);
static OBJECT_IMPLEMENT_PARENT(ei_device, ei_seat);
_public_ OBJECT_IMPLEMENT_GETTER(ei_device, type, enum ei_device_type);
_public_
OBJECT_IMPLEMENT_GETTER(ei_device, name, const char *);
_public_
OBJECT_IMPLEMENT_GETTER(ei_device, user_data, void *);
_public_
OBJECT_IMPLEMENT_SETTER(ei_device, user_data, void *);
_public_
OBJECT_IMPLEMENT_GETTER(ei_device, width, uint32_t);
_public_
OBJECT_IMPLEMENT_GETTER(ei_device, height, uint32_t);
OBJECT_IMPLEMENT_GETTER_AS_REF(ei_device, proto_object, const struct brei_object *);

object_id_t
ei_device_get_id(struct ei_device *device)
{
	return device->proto_object.id;
}

_public_ struct ei_seat *
ei_device_get_seat(struct ei_device *device)
{
	return ei_device_parent(device);
}

_public_ struct ei *
ei_device_get_context(struct ei_device *device)
{
	assert(device);
	return ei_seat_get_context(ei_device_get_seat(device));
}

static inline bool
ei_device_in_region(struct ei_device *device, double x, double y)
{
	struct ei_region *r;

	if (list_empty(&device->regions))
		return true;

	list_for_each(r, &device->regions, link) {
		if (ei_region_contains(r, x, y))
			return true;
	}

	return false;
}

static struct brei_result *
handle_msg_destroy(struct ei_device *device, uint32_t serial)
{
	struct ei *ei = ei_device_get_context(device);
	ei_update_serial(ei, serial);
	log_debug(ei, "Removed device %#" PRIx64 "", ei_device_get_id(device));
	ei_device_removed_by_server(device);
	return NULL;
}

static struct brei_result *
handle_msg_name(struct ei_device *device, const char *name)
{
	ei_device_set_name(device, name);
	return NULL;
}

static struct brei_result *
handle_msg_device_type(struct ei_device *device, enum ei_device_type type)
{
	if (device->type != 0)
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "EIS sent the device type twice");
	ei_device_set_type(device, type);
	return NULL;
}

static struct brei_result *
handle_msg_dimensions(struct ei_device *device, uint32_t width, uint32_t height)
{
	if (ei_device_get_width(device) != 0 || ei_device_get_height(device) != 0)
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "EIS sent the device type twice");
	ei_device_set_size(device, width, height);

	return NULL;
}

static struct brei_result *
handle_msg_region(struct ei_device *device,
		  uint32_t x,
		  uint32_t y,
		  uint32_t w,
		  uint32_t h,
		  float scale)
{
	_unref_(ei_region) *r = ei_region_new();
	ei_region_set_offset(r, x, y);
	ei_region_set_size(r, w, h);
	ei_region_set_physical_scale(r, scale);

	_cleanup_free_ char *mapping_id = steal(&device->pending_region_mapping_id);
	if (mapping_id)
		ei_region_set_mapping_id(r, mapping_id);

	ei_device_add_region(device, r);

	return NULL;
}

static struct brei_result *
handle_msg_region_mapping_id(struct ei_device *device, const char *mapping_id)
{
	if (device->pending_region_mapping_id)
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "EIS sent the region mapping_id twice");
	device->pending_region_mapping_id = xstrdup(mapping_id);
	return NULL;
}

static struct brei_result *
handle_msg_done(struct ei_device *device)
{
	struct ei *ei = ei_device_get_context(device);
	uint32_t width = ei_device_get_width(device);
	uint32_t height = ei_device_get_height(device);

	switch (ei_device_get_type(device)) {
	case EI_DEVICE_TYPE_PHYSICAL:
		if (width == 0 || height == 0)
			return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					       "missing width/height for physical device");
		break;
	case EI_DEVICE_TYPE_VIRTUAL:
		if (width != 0 || height != 0)
			return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					       "no width/height allowed for virtual device");
		break;
	default:
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Unsupported device type %ud",
				       ei_device_get_type(device));
	}

	if (device->pointer)
		mask_add(device->capabilities, EI_DEVICE_CAP_POINTER);
	if (device->pointer_absolute)
		mask_add(device->capabilities, EI_DEVICE_CAP_POINTER_ABSOLUTE);
	if (device->button)
		mask_add(device->capabilities, EI_DEVICE_CAP_BUTTON);
	if (device->scroll)
		mask_add(device->capabilities, EI_DEVICE_CAP_SCROLL);
	if (device->keyboard)
		mask_add(device->capabilities, EI_DEVICE_CAP_KEYBOARD);
	if (device->touchscreen)
		mask_add(device->capabilities, EI_DEVICE_CAP_TOUCH);
	if (device->text)
		mask_add(device->capabilities, EI_DEVICE_CAP_TEXT);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_POINTER) &&
	    !ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE) &&
	    !ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD) &&
	    !ei_device_has_capability(device, EI_DEVICE_CAP_TOUCH) &&
	    !ei_device_has_capability(device, EI_DEVICE_CAP_BUTTON) &&
	    !ei_device_has_capability(device, EI_DEVICE_CAP_SCROLL) &&
	    !ei_device_has_capability(device, EI_DEVICE_CAP_TEXT)) {
		log_debug(ei,
			  "Rejecting device %#" PRIx64 " '%s' with no known capabilities",
			  ei_device_get_id(device),
			  ei_device_get_name(device));
		ei_device_close(device);
		/* FIXME: this is untested */
		return NULL;
	}

	ei_device_added(device);
	ei_queue_device_added_event(device);
	ei_device_done(device);
	log_debug(ei,
		  "Added device %#" PRIx64 " '%s' caps: %s%s%s%s%s%s%s seat: %s",
		  ei_device_get_id(device),
		  ei_device_get_name(device),
		  ei_device_has_capability(device, EI_DEVICE_CAP_POINTER) ? "p" : "",
		  ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE) ? "a" : "",
		  ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD) ? "k" : "",
		  ei_device_has_capability(device, EI_DEVICE_CAP_TOUCH) ? "t" : "",
		  ei_device_has_capability(device, EI_DEVICE_CAP_BUTTON) ? "b" : "",
		  ei_device_has_capability(device, EI_DEVICE_CAP_SCROLL) ? "s" : "",
		  ei_device_has_capability(device, EI_DEVICE_CAP_TEXT) ? "x" : "",
		  ei_seat_get_name(ei_device_get_seat(device)));
	return NULL;
}

static struct brei_result *
handle_msg_resumed(struct ei_device *device, uint32_t serial)
{
	ei_update_serial(ei_device_get_context(device), serial);
	ei_device_resumed(device);
	ei_queue_device_resumed_event(device);

	return NULL;
}

static struct brei_result *
handle_msg_paused(struct ei_device *device, uint32_t serial)
{
	ei_update_serial(ei_device_get_context(device), serial);
	ei_device_paused(device);
	ei_queue_device_paused_event(device);

	return NULL;
}

#define DISCONNECT_IF_INVALID_ID(device_, id_) do { \
	if (!brei_is_server_id(id_)) { \
		struct ei *ei_ = ei_device_get_context(device_); \
		log_bug(ei_, "Received invalid object id %#" PRIx64 ". Disconnecting", id_); \
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL, "Received invalid object id %#" PRIx64 ".", id_); \
	} \
} while(0)

#define DISCONNECT_IF_SENDER_CONTEXT(device_) do {\
	struct ei *ei_ = ei_device_get_context(device_); \
	if (ei_is_sender(ei_)) { \
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_MODE, \
				       "Invalid event from receiver EIS context. Disconnecting"); \
	} \
} while(0)

static struct brei_result *
handle_msg_start_emulating(struct ei_device *device, uint32_t serial, uint32_t sequence)
{
	struct brei_result *result = NULL;

	DISCONNECT_IF_SENDER_CONTEXT(device);

	ei_update_serial(ei_device_get_context(device), serial);

	switch (device->state) {
	case EI_DEVICE_STATE_DEAD:
	case EI_DEVICE_STATE_NEW:
	case EI_DEVICE_STATE_PAUSED:
	case EI_DEVICE_STATE_EMULATING:
	case EI_DEVICE_STATE_REMOVED_FROM_SERVER:
		result = brei_result_new(
			EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
			"Invalid device state %u for a start_emulating event (sequence %u)",
			device->state,
			sequence);
		break;
	case EI_DEVICE_STATE_RESUMED:
		ei_queue_device_start_emulating_event(device, sequence);
		device->state = EI_DEVICE_STATE_EMULATING;
		break;
	case EI_DEVICE_STATE_REMOVED_FROM_CLIENT:
		/* race condition, that's fine */
		break;
	}

	return result;
}

static struct brei_result *
handle_msg_stop_emulating(struct ei_device *device, uint32_t serial)
{
	struct brei_result *result = NULL;

	DISCONNECT_IF_SENDER_CONTEXT(device);

	ei_update_serial(ei_device_get_context(device), serial);

	switch (device->state) {
	case EI_DEVICE_STATE_DEAD:
	case EI_DEVICE_STATE_NEW:
	case EI_DEVICE_STATE_PAUSED:
	case EI_DEVICE_STATE_RESUMED:
	case EI_DEVICE_STATE_REMOVED_FROM_SERVER:
		result = brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					 "Invalid device state %u for a stop_emulating event",
					 device->state);
		break;
	case EI_DEVICE_STATE_EMULATING:
		ei_queue_device_stop_emulating_event(device);
		device->state = EI_DEVICE_STATE_RESUMED;
		break;
	case EI_DEVICE_STATE_REMOVED_FROM_CLIENT:
		/* race condition, that's fine */
		break;
	}

	return result;
}

static struct brei_result *
maybe_error_on_device_state(struct ei_device *device, const char *event_type)
{
	switch (device->state) {
	case EI_DEVICE_STATE_EMULATING:
		return NULL;
	case EI_DEVICE_STATE_REMOVED_FROM_CLIENT:
		/* we removed the device but server sent us an event - most
		 * likely a race condition, so let's ignore it */
		return NULL;
	case EI_DEVICE_STATE_NEW:
	case EI_DEVICE_STATE_PAUSED:
	case EI_DEVICE_STATE_RESUMED:
	case EI_DEVICE_STATE_REMOVED_FROM_SERVER:
	case EI_DEVICE_STATE_DEAD:
		break;
	}

	return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
			       "Invalid device state %u for a %s event",
			       device->state,
			       event_type);
}

static struct brei_result *
handle_msg_frame(struct ei_device *device, uint32_t serial, uint64_t time)
{
	DISCONNECT_IF_SENDER_CONTEXT(device);

	ei_update_serial(ei_device_get_context(device), serial);

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_frame_event(device, time);
		return NULL;
	}

	return maybe_error_on_device_state(device, "frame");
}

static struct brei_result *
handle_msg_interface(struct ei_device *device, object_id_t id, const char *name, uint32_t version)
{
	DISCONNECT_IF_INVALID_ID(device, id);

	struct ei *ei = ei_device_get_context(device);

	if (streq(name, EI_POINTER_INTERFACE_NAME)) {
		DISCONNECT_IF_INVALID_VERSION(ei, ei_pointer, id, version);
		if (device->pointer)
			return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					       "Duplicate ei_pointer interface object on device");
		device->pointer = ei_pointer_new(device, id, version);
	} else if (streq(name, EI_POINTER_ABSOLUTE_INTERFACE_NAME)) {
		DISCONNECT_IF_INVALID_VERSION(ei, ei_pointer_absolute, id, version);
		if (device->pointer_absolute)
			return brei_result_new(
				EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				"Duplicate ei_pointer_absolute interface object on device");
		device->pointer_absolute = ei_pointer_absolute_new(device, id, version);
	} else if (streq(name, EI_SCROLL_INTERFACE_NAME)) {
		DISCONNECT_IF_INVALID_VERSION(ei, ei_scroll, id, version);
		if (device->scroll)
			return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					       "Duplicate ei_scroll interface object on device");
		device->scroll = ei_scroll_new(device, id, version);
	} else if (streq(name, EI_BUTTON_INTERFACE_NAME)) {
		DISCONNECT_IF_INVALID_VERSION(ei, ei_button, id, version);
		if (device->button)
			return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					       "Duplicate ei_button interface object on device");
		device->button = ei_button_new(device, id, version);
	} else if (streq(name, EI_KEYBOARD_INTERFACE_NAME)) {
		DISCONNECT_IF_INVALID_VERSION(ei, ei_keyboard, id, version);
		if (device->keyboard)
			return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					       "Duplicate ei_keyboard interface object on device");

		device->keyboard = ei_keyboard_new(device, id, version);
	} else if (streq(name, EI_TOUCHSCREEN_INTERFACE_NAME)) {
		DISCONNECT_IF_INVALID_VERSION(ei, ei_touchscreen, id, version);
		if (device->touchscreen)
			return brei_result_new(
				EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				"Duplicate ei_touchscreen interface object on device");

		device->touchscreen = ei_touchscreen_new(device, id, version);
	} else if (streq(name, EI_TEXT_INTERFACE_NAME)) {
		DISCONNECT_IF_INVALID_VERSION(ei, ei_text, id, version);
		if (device->text)
			return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					       "Duplicate ei_text interface object on device");
		device->text = ei_text_new(device, id, version);
	} else {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Unsupported interface '%s' on device",
				       name);
	}
	return NULL;
}

static const struct ei_device_interface interface = {
	.destroyed = handle_msg_destroy,
	.name = handle_msg_name,
	.device_type = handle_msg_device_type,
	.dimensions = handle_msg_dimensions,
	.region = handle_msg_region,
	.done = handle_msg_done,
	.resumed = handle_msg_resumed,
	.paused = handle_msg_paused,
	.start_emulating = handle_msg_start_emulating,
	.stop_emulating = handle_msg_stop_emulating,
	.frame = handle_msg_frame,
	.interface = handle_msg_interface,
	/* v2 */
	.region_mapping_id = handle_msg_region_mapping_id,
};

const struct ei_device_interface *
ei_device_get_interface(struct ei_device *device)
{
	return &interface;
}

static struct brei_result *
handle_msg_pointer_rel(struct ei_pointer *pointer, float x, float y)
{
	struct ei_device *device = ei_pointer_get_device(pointer);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_POINTER)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Pointer rel event for non-pointer device");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_pointer_rel_event(device, x, y);
		return NULL;
	}

	return maybe_error_on_device_state(device, "pointer rel");
}

static struct brei_result *
handle_msg_pointer_abs(struct ei_pointer_absolute *pointer, float x, float y)
{
	struct ei_device *device = ei_pointer_absolute_get_device(pointer);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Pointer abs event for non-pointer device");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		if (!ei_device_in_region(device, x, y))
			return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_VALUE,
					       "abs position outside regions");

		ei_queue_pointer_abs_event(device, x, y);
		return NULL;
	}

	return maybe_error_on_device_state(device, "pointer abs");
}

static struct brei_result *
handle_msg_button(struct ei_button *button, uint32_t btn, uint32_t state)
{
	struct ei_device *device = ei_button_get_device(button);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_BUTTON)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Button event for non-button device");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_pointer_button_event(device, btn, !!state);
		return NULL;
	}

	return maybe_error_on_device_state(device, "pointer button");
}

static struct brei_result *
handle_msg_scroll(struct ei_scroll *scroll, float x, float y)
{
	struct ei_device *device = ei_scroll_get_device(scroll);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_SCROLL)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Scroll event for non-scroll device");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_pointer_scroll_event(device, x, y);
		return NULL;
	}

	return maybe_error_on_device_state(device, "pointer scroll");
}

static struct brei_result *
handle_msg_scroll_discrete(struct ei_scroll *scroll, int32_t x, int32_t y)
{
	struct ei_device *device = ei_scroll_get_device(scroll);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_SCROLL)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Scroll discrete event for non-scroll device");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_pointer_scroll_discrete_event(device, x, y);
		return NULL;
	}

	return maybe_error_on_device_state(device, "pointer scroll discrete");
}

static struct brei_result *
handle_msg_scroll_stop(struct ei_scroll *scroll, uint32_t x, uint32_t y, uint32_t is_cancel)
{
	struct ei_device *device = ei_scroll_get_device(scroll);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_SCROLL)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Scroll stop event for non-scroll device");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		if (is_cancel)
			ei_queue_pointer_scroll_cancel_event(device, !!x, !!y);
		else
			ei_queue_pointer_scroll_stop_event(device, !!x, !!y);
		return NULL;
	}

	return maybe_error_on_device_state(device, "pointer scroll discrete");
}

static struct brei_result *
handle_msg_pointer_destroy(struct ei_pointer *pointer, uint32_t serial)
{
	struct ei *ei = ei_pointer_get_context(pointer);
	ei_update_serial(ei, serial);

	struct ei_device *device = ei_pointer_get_device(pointer);
	ei_pointer_unref(steal(&device->pointer));

	return NULL;
}

static struct brei_result *
handle_msg_pointer_absolute_destroy(struct ei_pointer_absolute *pointer_absolute, uint32_t serial)
{
	struct ei *ei = ei_pointer_absolute_get_context(pointer_absolute);
	ei_update_serial(ei, serial);

	struct ei_device *device = ei_pointer_absolute_get_device(pointer_absolute);
	ei_pointer_absolute_unref(steal(&device->pointer_absolute));

	return NULL;
}

static struct brei_result *
handle_msg_scroll_destroy(struct ei_scroll *scroll, uint32_t serial)
{
	struct ei *ei = ei_scroll_get_context(scroll);
	ei_update_serial(ei, serial);

	struct ei_device *device = ei_scroll_get_device(scroll);
	ei_scroll_unref(steal(&device->scroll));

	return NULL;
}

static struct brei_result *
handle_msg_button_destroy(struct ei_button *button, uint32_t serial)
{
	struct ei *ei = ei_button_get_context(button);
	ei_update_serial(ei, serial);

	struct ei_device *device = ei_button_get_device(button);
	ei_button_unref(steal(&device->button));

	return NULL;
}

static const struct ei_pointer_interface pointer_interface = {
	.destroyed = handle_msg_pointer_destroy,
	.motion_relative = handle_msg_pointer_rel,
};

static const struct ei_pointer_absolute_interface pointer_absolute_interface = {
	.destroyed = handle_msg_pointer_absolute_destroy,
	.motion_absolute = handle_msg_pointer_abs,
};

static const struct ei_button_interface button_interface = {
	.destroyed = handle_msg_button_destroy,
	.button = handle_msg_button,
};

static const struct ei_scroll_interface scroll_interface = {
	.destroyed = handle_msg_scroll_destroy,
	.scroll = handle_msg_scroll,
	.scroll_stop = handle_msg_scroll_stop,
	.scroll_discrete = handle_msg_scroll_discrete,
};

const struct ei_pointer_interface *
ei_device_get_pointer_interface(struct ei_device *device)
{
	return &pointer_interface;
}

const struct ei_pointer_absolute_interface *
ei_device_get_pointer_absolute_interface(struct ei_device *device)
{
	return &pointer_absolute_interface;
}

const struct ei_scroll_interface *
ei_device_get_scroll_interface(struct ei_device *device)
{
	return &scroll_interface;
}

const struct ei_button_interface *
ei_device_get_button_interface(struct ei_device *device)
{
	return &button_interface;
}

static struct brei_result *
handle_msg_keymap(struct ei_keyboard *keyboard,
		  uint32_t keymap_type,
		  uint32_t keymap_sz,
		  int keymap_fd)
{
	struct ei_device *device = ei_keyboard_get_device(keyboard);

	ei_device_set_keymap(device, keymap_type, keymap_fd, keymap_sz);
	xclose(keymap_fd);

	return NULL;
}

static struct brei_result *
handle_msg_keyboard_key(struct ei_keyboard *keyboard, uint32_t key, uint32_t state)
{
	struct ei_device *device = ei_keyboard_get_device(keyboard);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Key event for non-keyboard device");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_keyboard_key_event(device, key, !!state);
		return NULL;
	}

	return maybe_error_on_device_state(device, "key");
}

static struct brei_result *
handle_msg_keyboard_modifiers(struct ei_keyboard *keyboard,
			      uint32_t serial,
			      uint32_t depressed,
			      uint32_t locked,
			      uint32_t latched,
			      uint32_t group)
{
	struct ei *ei = ei_keyboard_get_context(keyboard);
	ei_update_serial(ei, serial);

	struct ei_device *device = ei_keyboard_get_device(keyboard);
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Modifier event for non-keyboard device");
	}

	struct ei_xkb_modifiers mods = {
		.depressed = depressed,
		.latched = latched,
		.locked = locked,
		.group = group,
	};

	ei_queue_keyboard_modifiers_event(device, &mods);

	return NULL;
}

static struct brei_result *
handle_msg_keyboard_destroy(struct ei_keyboard *keyboard, uint32_t serial)
{
	struct ei *ei = ei_keyboard_get_context(keyboard);
	ei_update_serial(ei, serial);

	struct ei_device *device = ei_keyboard_get_device(keyboard);
	ei_keyboard_unref(steal(&device->keyboard));

	return NULL;
}

static const struct ei_keyboard_interface keyboard_interface = {
	.destroyed = handle_msg_keyboard_destroy,
	.keymap = handle_msg_keymap,
	.key = handle_msg_keyboard_key,
	.modifiers = handle_msg_keyboard_modifiers,
};

const struct ei_keyboard_interface *
ei_device_get_keyboard_interface(struct ei_device *device)
{
	return &keyboard_interface;
}

static struct brei_result *
handle_msg_touch_down(struct ei_touchscreen *touchscreen, uint32_t touchid, float x, float y)
{
	struct ei_device *device = ei_touchscreen_get_device(touchscreen);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_TOUCH)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Touch down event for non-touch device");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_touch_down_event(device, touchid, x, y);
		return NULL;
	}

	return maybe_error_on_device_state(device, "touch down");
}

static struct brei_result *
handle_msg_touch_motion(struct ei_touchscreen *touchscreen, uint32_t touchid, float x, float y)
{
	struct ei_device *device = ei_touchscreen_get_device(touchscreen);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_TOUCH)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Touch motion event for non-touch device");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_touch_motion_event(device, touchid, x, y);
		return NULL;
	}

	return maybe_error_on_device_state(device, "touch motion");
}

static struct brei_result *
handle_msg_touch_up(struct ei_touchscreen *touchscreen, uint32_t touchid)
{
	struct ei_device *device = ei_touchscreen_get_device(touchscreen);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_TOUCH)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Touch up event for non-touch device");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_touch_up_event(device, touchid);
		return NULL;
	}
	return maybe_error_on_device_state(device, "touch up");
}

static struct brei_result *
handle_msg_touch_cancel(struct ei_touchscreen *touchscreen, uint32_t touchid)
{
	struct ei_device *device = ei_touchscreen_get_device(touchscreen);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_TOUCH)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Touch cancel event for non-touch device");
	}

	struct ei *ei = ei_device_get_context(device);
	if (ei->interface_versions.ei_touchscreen < EI_TOUCHSCREEN_EVENT_CANCEL_SINCE_VERSION) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Touch cancel event for touchscreen version v1");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_touch_cancel_event(device, touchid);
		return NULL;
	}
	return maybe_error_on_device_state(device, "touch cancel");
}

static struct brei_result *
handle_msg_touchscreen_destroy(struct ei_touchscreen *touchscreen, uint32_t serial)
{
	struct ei *ei = ei_touchscreen_get_context(touchscreen);
	ei_update_serial(ei, serial);

	struct ei_device *device = ei_touchscreen_get_device(touchscreen);
	ei_touchscreen_unref(steal(&device->touchscreen));

	return NULL;
}

static const struct ei_touchscreen_interface touchscreen_interface = {
	.destroyed = handle_msg_touchscreen_destroy,
	.down = handle_msg_touch_down,
	.motion = handle_msg_touch_motion,
	.up = handle_msg_touch_up,
	.cancel = handle_msg_touch_cancel,
};

const struct ei_touchscreen_interface *
ei_device_get_touchscreen_interface(struct ei_device *device)
{
	return &touchscreen_interface;
}

static struct brei_result *
handle_msg_text_destroy(struct ei_text *text, uint32_t serial)
{
	struct ei *ei = ei_text_get_context(text);
	ei_update_serial(ei, serial);

	struct ei_device *device = ei_text_get_device(text);
	ei_text_unref(steal(&device->text));

	return NULL;
}

static struct brei_result *
handle_msg_text_keysym(struct ei_text *text, uint32_t keysym, uint32_t state)
{
	struct ei_device *device = ei_text_get_device(text);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_TEXT)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "keysym event for non-text device");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_text_keysym_event(device, keysym, !!state);
		return NULL;
	}

	return maybe_error_on_device_state(device, "text keysym");
}

static struct brei_result *
handle_msg_text_utf8(struct ei_text *text, const char *utf8)
{
	struct ei_device *device = ei_text_get_device(text);

	DISCONNECT_IF_SENDER_CONTEXT(device);

	if (!ei_device_has_capability(device, EI_DEVICE_CAP_TEXT)) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "utf8 event for non-text device");
	}

	if (!utf8 || utf8[0] == '\0') {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "utf8 text is NULL or the empty string");
	}

	if (strlen(utf8) > 254) {
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "utf8 string exceeds max length of 255 bytes");
	}

	if (device->state == EI_DEVICE_STATE_EMULATING) {
		ei_queue_text_utf8_event(device, utf8);
		return NULL;
	}

	return maybe_error_on_device_state(device, "text utf8");
}

static const struct ei_text_interface text_interface = {
	.destroyed = handle_msg_text_destroy,
	.keysym = handle_msg_text_keysym,
	.utf8 = handle_msg_text_utf8,
};

const struct ei_text_interface *
ei_device_get_text_interface(struct ei_device *device)
{
	return &text_interface;
}

struct ei_device *
ei_device_new(struct ei_seat *seat, object_id_t deviceid, uint32_t version)
{
	struct ei_device *device = ei_device_create(&seat->object);
	struct ei *ei = ei_seat_get_context(seat);

	device->proto_object.id = deviceid, device->proto_object.implementation = device;
	device->proto_object.interface = &ei_device_proto_interface;
	device->proto_object.version = version;
	list_init(&device->proto_object.link);
	ei_register_object(ei, &device->proto_object);

	device->capabilities = 0;
	device->state = EI_DEVICE_STATE_NEW;
	device->name = xaprintf("unnamed device %#" PRIx64 "", deviceid);
	list_init(&device->regions);
	list_init(&device->pending_event_queue);

	/* We have a ref to the seat to make sure our seat doesn't get
	 * destroyed while a ref to the device is still alive.
	 * And the seat has a ref to the device in the seat->devices list.
	 * dropped when the device is removed.
	 */
	ei_seat_ref(seat);

	return device;
}

void
ei_device_done(struct ei_device *device)
{
	ei_device_paused(device);
}

void
ei_device_add_region(struct ei_device *device, struct ei_region *region)
{
	if (device->state != EI_DEVICE_STATE_NEW)
		return;

	ei_region_ref(region);
	list_append(&device->regions, &region->link);
}

_public_ OBJECT_IMPLEMENT_REF(ei_keymap);
_public_ OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_keymap);
_public_ OBJECT_IMPLEMENT_GETTER(ei_keymap, type, enum ei_keymap_type);
_public_
OBJECT_IMPLEMENT_GETTER(ei_keymap, fd, int);
_public_
OBJECT_IMPLEMENT_GETTER(ei_keymap, size, size_t);
_public_
OBJECT_IMPLEMENT_GETTER(ei_keymap, device, struct ei_device *);
_public_
OBJECT_IMPLEMENT_GETTER(ei_keymap, user_data, void *);
_public_
OBJECT_IMPLEMENT_SETTER(ei_keymap, user_data, void *);

static void
ei_keymap_destroy(struct ei_keymap *keymap)
{
	xclose(keymap->fd);
}

static OBJECT_IMPLEMENT_CREATE(ei_keymap);

_public_ struct ei_keymap *
ei_device_keyboard_get_keymap(struct ei_device *device)
{
	return device->keymap;
}

static struct ei_keymap *
ei_keymap_new(struct ei *ei, enum ei_keymap_type type, int fd, size_t size)
{
	_unref_(ei_keymap) *keymap = ei_keymap_create(NULL);

	switch (type) {
	case EI_KEYMAP_TYPE_XKB:
		break;
	default:
		log_bug(ei, "Unsupported keymap type: %u", type);
		return NULL;
	}

	if (fd < 0 || size == 0) {
		log_bug(ei, "Invalid keymap fd %d with size %zu", fd, size);
		return NULL;
	}

	int newfd = xdup(fd);
	if (newfd < 0) {
		log_bug(ei, "Failed to dup keymap fd: %s", strerror(errno));
		return NULL;
	}

	keymap->fd = newfd;
	keymap->type = type;
	keymap->size = size;

	return ei_keymap_ref(keymap);
}

void
ei_device_set_keymap(struct ei_device *device, enum ei_keymap_type type, int keymap_fd, size_t size)
{
	device->keymap = ei_keymap_unref(device->keymap);

	if (!type)
		return;

	struct ei *ei = ei_device_get_context(device);
	_unref_(ei_keymap) *keymap = ei_keymap_new(ei, type, keymap_fd, size);
	if (!keymap)
		return; /* FIXME: ei_device_remove() here */

	keymap->device = device;
	device->keymap = ei_keymap_ref(keymap);
}

static int
ei_device_send_release(struct ei_device *device)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	if (device->pointer)
		ei_pointer_request_release(device->pointer);
	if (device->keyboard)
		ei_keyboard_request_release(device->keyboard);
	if (device->touchscreen)
		ei_touchscreen_request_release(device->touchscreen);
	if (device->scroll)
		ei_scroll_request_release(device->scroll);
	if (device->button)
		ei_button_request_release(device->button);

	int rc = ei_device_request_release(device);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

_public_ void
ei_device_close(struct ei_device *device)
{
	switch (device->state) {
	case EI_DEVICE_STATE_NEW:
	case EI_DEVICE_STATE_DEAD:
	case EI_DEVICE_STATE_REMOVED_FROM_CLIENT:
	case EI_DEVICE_STATE_REMOVED_FROM_SERVER:
		break;
	case EI_DEVICE_STATE_EMULATING:
		if (ei_is_sender(ei_device_get_context(device)))
			ei_device_request_stop_emulating(
				device,
				ei_get_serial(ei_device_get_context(device)));
		_fallthrough_;
	case EI_DEVICE_STATE_PAUSED:
	case EI_DEVICE_STATE_RESUMED:
		ei_device_set_state(device, EI_DEVICE_STATE_REMOVED_FROM_CLIENT);
		ei_device_send_release(device);
		break;
	}
}

void
ei_device_removed_by_server(struct ei_device *device)
{
	struct ei_seat *seat = ei_device_get_seat(device);
	struct ei *ei = ei_device_get_context(device);
	struct ei_event *event;

	switch (device->state) {
	case EI_DEVICE_STATE_NEW:
	case EI_DEVICE_STATE_DEAD:
	case EI_DEVICE_STATE_REMOVED_FROM_SERVER:
		break;
	case EI_DEVICE_STATE_REMOVED_FROM_CLIENT:
	case EI_DEVICE_STATE_PAUSED:
	case EI_DEVICE_STATE_RESUMED:
	case EI_DEVICE_STATE_EMULATING:
		list_for_each_safe(event, &device->pending_event_queue, link) {
			list_remove(&event->link);
			ei_event_unref(event);
		}

		/* in the case of ei_disconnect() we may fake the
		 * removal by the server, so we need to also remove the
		 * pointer/keyboard/touch interfaces
		 */
		ei_pointer_unref(steal(&device->pointer));
		ei_keyboard_unref(steal(&device->keyboard));
		ei_touchscreen_unref(steal(&device->touchscreen));
		ei_scroll_unref(steal(&device->scroll));
		ei_button_unref(steal(&device->button));

		ei_unregister_object(ei, &device->proto_object);
		ei_queue_device_removed_event(device);
		ei_device_set_state(device, EI_DEVICE_STATE_DEAD);
		/* Device is dead now. Move it from the devices list to
		 * removed and drop the seat's ref to the device.
		 * This should be the last ref to the device that libei has
		 * (not counting any queued events). Device is kept alive by
		 * any client refs but once those drop, the device can be
		 * destroyed.
		 */
		list_remove(&device->link);
		list_append(&seat->devices_removed, &device->link);
		ei_device_unref(device);
		break;
	}
}

void
ei_device_resumed(struct ei_device *device)
{
	ei_device_set_state(device, EI_DEVICE_STATE_RESUMED);
}

void
ei_device_paused(struct ei_device *device)
{
	ei_device_set_state(device, EI_DEVICE_STATE_PAUSED);
}

void
ei_device_added(struct ei_device *device)
{
}

void
ei_device_set_type(struct ei_device *device, enum ei_device_type type)
{
	switch (type) {
	case EI_DEVICE_TYPE_PHYSICAL:
	case EI_DEVICE_TYPE_VIRTUAL:
		device->type = type;
		break;
	default:
		log_bug(ei_device_get_context(device), "Invalid device type %u", type);
		break;
	}
}

void
ei_device_set_size(struct ei_device *device, uint32_t width, uint32_t height)
{
	if (device->type == EI_DEVICE_TYPE_PHYSICAL) {
		device->width = width;
		device->height = height;
	}
}

void
ei_device_set_name(struct ei_device *device, const char *name)
{
	free(device->name);
	device->name = xstrdup(name);
}

_public_ bool
ei_device_has_capability(struct ei_device *device, enum ei_device_capability cap)
{
	switch (cap) {
	case EI_DEVICE_CAP_POINTER:
	case EI_DEVICE_CAP_POINTER_ABSOLUTE:
	case EI_DEVICE_CAP_KEYBOARD:
	case EI_DEVICE_CAP_TOUCH:
	case EI_DEVICE_CAP_BUTTON:
	case EI_DEVICE_CAP_SCROLL:
	case EI_DEVICE_CAP_TEXT:
		return mask_all(device->capabilities, cap);
	}
	return false;
}

static void
ei_device_frame_now(struct ei_device *device)
{
	uint64_t now = ei_now(ei_device_get_context(device));

	ei_device_frame(device, now);
}

static void
_flush_frame(struct ei_device *device, const char *func)
{
	if (device->send_frame_event) {
		log_bug_client(ei_device_get_context(device),
			       "%s: missing call to ei_device_frame()",
			       func);
		ei_device_frame_now(device);
	}
}
#define ei_device_flush_frame(d_) _flush_frame(d_, __func__)

_public_ void
ei_device_start_emulating(struct ei_device *device, uint32_t sequence)
{
	struct ei *ei = ei_device_get_context(device);

	if (device->state != EI_DEVICE_STATE_RESUMED)
		return;

	assert(!device->send_frame_event);

	device->state = EI_DEVICE_STATE_EMULATING;
	int rc = ei_device_request_start_emulating(device, ei_get_serial(ei), sequence);
	if (rc)
		ei_disconnect(ei_device_get_context(device));
}

_public_ void
ei_device_stop_emulating(struct ei_device *device)
{
	struct ei *ei = ei_device_get_context(device);

	if (device->state != EI_DEVICE_STATE_EMULATING)
		return;

	ei_device_flush_frame(device);
	device->state = EI_DEVICE_STATE_RESUMED;
	int rc = ei_device_request_stop_emulating(device, ei_get_serial(ei));
	if (rc)
		ei_disconnect(ei_device_get_context(device));
}

_public_ struct ei_region *
ei_device_get_region(struct ei_device *device, size_t index)
{
	return list_nth_entry(struct ei_region, &device->regions, link, index);
}

_public_ struct ei_region *
ei_device_get_region_at(struct ei_device *device, double x, double y)
{
	struct ei_region *r;

	list_for_each(r, &device->regions, link) {
		if (ei_region_contains(r, x, y))
			return r;
	}
	return NULL;
}

static inline void
ei_device_resume_scrolling(struct ei_device *device, double x, double y)
{
	if (x) {
		device->scroll_state.x_is_stopped = false;
		device->scroll_state.x_is_cancelled = false;
	}
	if (y) {
		device->scroll_state.y_is_stopped = false;
		device->scroll_state.y_is_cancelled = false;
	}
}

static int
ei_send_pointer_rel(struct ei_device *device, double x, double y)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_pointer_request_motion_relative(device->pointer, x, y);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

static int
ei_send_pointer_abs(struct ei_device *device, double x, double y)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_pointer_absolute_request_motion_absolute(device->pointer_absolute, x, y);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

static int
ei_send_pointer_button(struct ei_device *device, uint32_t button, bool is_press)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_button_request_button(device->button, button, is_press);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

static int
ei_send_pointer_scroll(struct ei_device *device, double x, double y)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_scroll_request_scroll(device->scroll, x, y);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

static int
ei_send_pointer_scroll_stop(struct ei_device *device, double x, double y)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_scroll_request_scroll_stop(device->scroll, x, y, false);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

static int
ei_send_pointer_scroll_cancel(struct ei_device *device, double x, double y)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_scroll_request_scroll_stop(device->scroll, x, y, true);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

static int
ei_send_pointer_scroll_discrete(struct ei_device *device, int32_t x, int32_t y)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_scroll_request_scroll_discrete(device->scroll, x, y);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

_public_ void
ei_device_pointer_motion(struct ei_device *device, double x, double y)
{
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_POINTER)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not a pointer",
			       __func__);
		return;
	}

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	ei_send_pointer_rel(device, x, y);
}

_public_ void
ei_device_pointer_motion_absolute(struct ei_device *device, double x, double y)
{
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not an absolute pointer",
			       __func__);
		return;
	}

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	if (!ei_device_in_region(device, x, y))
		return;

	ei_send_pointer_abs(device, x, y);
}

_public_ void
ei_device_button_button(struct ei_device *device, uint32_t button, bool is_press)
{
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_BUTTON)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not a button device",
			       __func__);
		return;
	}

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	/* Ignore anything < BTN_MOUSE. Avoids the common error of sending
	 * numerical buttons instead of BTN_LEFT and friends. */
	if (button < 0x110) {
		log_bug_client(ei_device_get_context(device),
			       "%s: button code must be one of BTN_*",
			       __func__);
		return;
	}

	ei_send_pointer_button(device, button, is_press);
}

_public_ void
ei_device_scroll_delta(struct ei_device *device, double x, double y)
{
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_SCROLL)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not scroll device",
			       __func__);
		return;
	}

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	ei_device_resume_scrolling(device, x, y);

	ei_send_pointer_scroll(device, x, y);
}

_public_ void
ei_device_scroll_stop(struct ei_device *device, bool x, bool y)
{
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_SCROLL)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not scroll device",
			       __func__);
		return;
	}

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	/* Filter out duplicate scroll stop requests */
	if (x && !device->scroll_state.x_is_stopped)
		device->scroll_state.x_is_stopped = true;
	else
		x = false;

	if (y && !device->scroll_state.y_is_stopped)
		device->scroll_state.y_is_stopped = true;
	else
		y = false;

	if (x || y)
		ei_send_pointer_scroll_stop(device, x, y);
}

_public_ void
ei_device_scroll_cancel(struct ei_device *device, bool x, bool y)
{
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_SCROLL)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not scroll device",
			       __func__);
		return;
	}

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	/* Filter out duplicate scroll cancelled requests */
	if (x && !device->scroll_state.x_is_cancelled) {
		device->scroll_state.x_is_stopped = true;
		device->scroll_state.x_is_cancelled = true;
	} else {
		x = false;
	}

	if (y && !device->scroll_state.y_is_cancelled) {
		device->scroll_state.y_is_stopped = true;
		device->scroll_state.y_is_cancelled = true;
	} else {
		y = false;
	}

	if (x || y)
		ei_send_pointer_scroll_cancel(device, x, y);
}

_public_ void
ei_device_scroll_discrete(struct ei_device *device, int32_t x, int32_t y)
{
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_SCROLL)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not scroll device",
			       __func__);
		return;
	}

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	if (abs(x) == 1 || abs(y) == 1) {
		log_bug_client(ei_device_get_context(device),
			       "%s: suspicious discrete event value 1, did you mean 120?",
			       __func__);
	}

	ei_device_resume_scrolling(device, x, y);

	ei_send_pointer_scroll_discrete(device, x, y);
}

static int
ei_send_keyboard_key(struct ei_device *device, uint32_t key, bool is_press)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_keyboard_request_key(device->keyboard, key, is_press);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

_public_ void
ei_device_keyboard_key(struct ei_device *device, uint32_t key, bool is_press)
{
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not a keyboard",
			       __func__);
		return;
	}

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	ei_send_keyboard_key(device, key, is_press);
}

_public_ OBJECT_IMPLEMENT_REF(ei_touch);
_public_ OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_touch);
_public_
OBJECT_IMPLEMENT_GETTER(ei_touch, device, struct ei_device *);
_public_
OBJECT_IMPLEMENT_GETTER(ei_touch, user_data, void *);
_public_
OBJECT_IMPLEMENT_SETTER(ei_touch, user_data, void *);

static void
ei_touch_destroy(struct ei_touch *touch)
{
	if (touch->state == TOUCH_IS_DOWN)
		ei_touch_up(touch);
	/* Enforce a frame, otherwise we're just pending. If the client
	 * doesn't want this, it needs to ei_touch_up() */
	ei_device_frame_now(touch->device);
	ei_device_unref(touch->device);
}

static OBJECT_IMPLEMENT_CREATE(ei_touch);

static int
ei_send_touch_down(struct ei_device *device, uint32_t tid, double x, double y)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_touchscreen_request_down(device->touchscreen, tid, x, y);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

static int
ei_send_touch_motion(struct ei_device *device, uint32_t tid, double x, double y)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_touchscreen_request_motion(device->touchscreen, tid, x, y);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

static int
ei_send_touch_up(struct ei_device *device, uint32_t tid)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_touchscreen_request_up(device->touchscreen, tid);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

static int
ei_send_touch_cancel(struct ei_device *device, uint32_t tid)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_touchscreen_request_cancel(device->touchscreen, tid);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

_public_ struct ei_touch *
ei_device_touch_new(struct ei_device *device)
{
	static uint32_t tracking_id = 0;

	/* Not using the device as parent object because we need a ref
	 * to it */
	struct ei_touch *touch = ei_touch_create(NULL);

	touch->device = ei_device_ref(device);
	touch->state = TOUCH_IS_NEW;
	touch->tracking_id = ++tracking_id;

	return touch;
}

_public_ void
ei_touch_down(struct ei_touch *touch, double x, double y)
{
	struct ei_device *device = ei_touch_get_device(touch);

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	if (touch->state != TOUCH_IS_NEW) {
		log_bug_client(ei_device_get_context(device),
			       "%s: touch %u already down or up",
			       __func__,
			       touch->tracking_id);
		return;
	}

	if (!ei_device_in_region(device, x, y)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: touch %u has invalid x/y coordinates",
			       __func__,
			       touch->tracking_id);
		touch->state = TOUCH_IS_UP;
		return;
	}

	touch->state = TOUCH_IS_DOWN;

	ei_send_touch_down(device, touch->tracking_id, x, y);
}

_public_ void
ei_touch_motion(struct ei_touch *touch, double x, double y)
{
	struct ei_device *device = ei_touch_get_device(touch);

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	if (touch->state != TOUCH_IS_DOWN) {
		log_bug_client(ei_device_get_context(device),
			       "%s: touch %u is not currently down",
			       __func__,
			       touch->tracking_id);
		return;
	}

	if (!ei_device_in_region(device, x, y)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: touch %u has invalid x/y coordinates",
			       __func__,
			       touch->tracking_id);
		ei_touch_up(touch);
		return;
	}

	ei_send_touch_motion(touch->device, touch->tracking_id, x, y);
}

_public_ void
ei_touch_up(struct ei_touch *touch)
{
	struct ei_device *device = ei_touch_get_device(touch);
	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	if (touch->state != TOUCH_IS_DOWN) {
		log_bug_client(ei_device_get_context(device),
			       "%s: touch %u is not currently down",
			       __func__,
			       touch->tracking_id);
		return;
	}

	touch->state = TOUCH_IS_UP;
	ei_send_touch_up(touch->device, touch->tracking_id);
}

_public_ void
ei_touch_cancel(struct ei_touch *touch)
{
	struct ei_device *device = ei_touch_get_device(touch);
	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	if (touch->state != TOUCH_IS_DOWN) {
		log_bug_client(ei_device_get_context(device),
			       "%s: touch %u is not currently down",
			       __func__,
			       touch->tracking_id);
		return;
	}

	touch->state = TOUCH_IS_UP;

	struct ei *ei = ei_device_get_context(device);

	if (ei->interface_versions.ei_touchscreen >= EI_TOUCHSCREEN_REQUEST_CANCEL_SINCE_VERSION)
		ei_send_touch_cancel(touch->device, touch->tracking_id);
	else
		ei_send_touch_up(touch->device, touch->tracking_id);
}

static int
ei_send_text_keysym(struct ei_device *device, uint32_t keysym, bool is_press)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_text_request_keysym(device->text, keysym, is_press);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

static int
ei_send_text_utf8(struct ei_device *device, const char *utf8)
{
	struct ei *ei = ei_device_get_context(device);

	if (ei->state == EI_STATE_NEW || ei->state == EI_STATE_DISCONNECTED)
		return 0;

	device->send_frame_event = true;

	int rc = ei_text_request_utf8(device->text, utf8);
	if (rc)
		ei_disconnect(ei);
	return rc;
}

_public_ void
ei_device_text_keysym(struct ei_device *device, uint32_t keysym, bool is_press)
{
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_TEXT)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not a text device",
			       __func__);
		return;
	}

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	ei_send_text_keysym(device, keysym, is_press);
}

_public_ void
ei_device_text_utf8(struct ei_device *device, const char *utf8)
{
	ei_device_text_utf8_with_length(device, utf8, utf8 ? strlen(utf8) : 0);
}

_public_ void
ei_device_text_utf8_with_length(struct ei_device *device, const char *utf8, size_t length)
{
	if (!ei_device_has_capability(device, EI_DEVICE_CAP_TEXT)) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not a text device",
			       __func__);
		return;
	}

	if (device->state != EI_DEVICE_STATE_EMULATING) {
		log_bug_client(ei_device_get_context(device),
			       "%s: device is not emulating",
			       __func__);
		return;
	}

	if (!utf8) {
		log_bug_client(ei_device_get_context(device), "%s: utf8 is NULL", __func__);
		return;
	}

	if (length == 0) {
		log_bug_client(ei_device_get_context(device),
			       "%s: zero-length utf8 string",
			       __func__);
		return;
	}

	if (length > 254) {
		log_bug_client(ei_device_get_context(device),
			       "%s: utf8 string exceeds max length of 255 bytes",
			       __func__);
		return;
	}

	char buf[255] = { 0 };
	memcpy(buf, utf8, length);
	ei_send_text_utf8(device, buf);
}

_public_ void
ei_device_frame(struct ei_device *device, uint64_t time)
{
	struct ei *ei = ei_device_get_context(device);

	if (device->state != EI_DEVICE_STATE_EMULATING)
		return;

	if (!device->send_frame_event)
		return;

	device->send_frame_event = false;

	int rc = ei_device_request_frame(device, ei_get_serial(ei), time);
	if (rc)
		ei_disconnect(ei_device_get_context(device));
	return;
}
