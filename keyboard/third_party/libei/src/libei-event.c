/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Red Hat, Inc.
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

#include "libei-private.h"

OBJECT_IMPLEMENT_REF(ei_event);
_public_ OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_event);
_public_ OBJECT_IMPLEMENT_GETTER(ei_event, type, enum ei_event_type);
_public_
OBJECT_IMPLEMENT_GETTER(ei_event, device, struct ei_device *);
_public_
OBJECT_IMPLEMENT_GETTER(ei_event, seat, struct ei_seat *);

_public_ const char *
ei_event_type_to_string(enum ei_event_type type)
{
	switch (type) {
	CASE_RETURN_STRING(EI_EVENT_CONNECT);
	CASE_RETURN_STRING(EI_EVENT_DISCONNECT);
	CASE_RETURN_STRING(EI_EVENT_SEAT_ADDED);
	CASE_RETURN_STRING(EI_EVENT_SEAT_REMOVED);
	CASE_RETURN_STRING(EI_EVENT_DEVICE_ADDED);
	CASE_RETURN_STRING(EI_EVENT_DEVICE_REMOVED);
	CASE_RETURN_STRING(EI_EVENT_DEVICE_PAUSED);
	CASE_RETURN_STRING(EI_EVENT_DEVICE_RESUMED);
	CASE_RETURN_STRING(EI_EVENT_KEYBOARD_MODIFIERS);
	CASE_RETURN_STRING(EI_EVENT_PONG);
	CASE_RETURN_STRING(EI_EVENT_SYNC);
	CASE_RETURN_STRING(EI_EVENT_FRAME);
	CASE_RETURN_STRING(EI_EVENT_DEVICE_START_EMULATING);
	CASE_RETURN_STRING(EI_EVENT_DEVICE_STOP_EMULATING);
	CASE_RETURN_STRING(EI_EVENT_POINTER_MOTION);
	CASE_RETURN_STRING(EI_EVENT_POINTER_MOTION_ABSOLUTE);
	CASE_RETURN_STRING(EI_EVENT_BUTTON_BUTTON);
	CASE_RETURN_STRING(EI_EVENT_SCROLL_DELTA);
	CASE_RETURN_STRING(EI_EVENT_SCROLL_STOP);
	CASE_RETURN_STRING(EI_EVENT_SCROLL_CANCEL);
	CASE_RETURN_STRING(EI_EVENT_SCROLL_DISCRETE);
	CASE_RETURN_STRING(EI_EVENT_KEYBOARD_KEY);
	CASE_RETURN_STRING(EI_EVENT_TOUCH_DOWN);
	CASE_RETURN_STRING(EI_EVENT_TOUCH_UP);
	CASE_RETURN_STRING(EI_EVENT_TOUCH_MOTION);
	CASE_RETURN_STRING(EI_EVENT_TEXT_KEYSYM);
	CASE_RETURN_STRING(EI_EVENT_TEXT_UTF8);
	}

	return NULL;
}

static void
ei_event_destroy(struct ei_event *event)
{
	struct ei *ei = ei_event_get_context(event);

	switch (event->type) {
	case EI_EVENT_CONNECT:
	case EI_EVENT_DISCONNECT:
	case EI_EVENT_SEAT_ADDED:
	case EI_EVENT_SEAT_REMOVED:
	case EI_EVENT_DEVICE_REMOVED:
	case EI_EVENT_DEVICE_PAUSED:
	case EI_EVENT_DEVICE_RESUMED:
	case EI_EVENT_KEYBOARD_MODIFIERS:
	case EI_EVENT_FRAME:
	case EI_EVENT_DEVICE_START_EMULATING:
	case EI_EVENT_DEVICE_STOP_EMULATING:
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
		break;
	case EI_EVENT_TEXT_UTF8:
		free(steal(&event->text.utf8));
		break;
	case EI_EVENT_DEVICE_ADDED:
		if (ei->interface_versions.ei_device >= EI_DEVICE_REQUEST_READY_SINCE_VERSION)
			ei_device_request_ready(event->device);
		break;
	case EI_EVENT_PONG:
		ei_ping_unref(event->pong.ping);
		break;
	case EI_EVENT_SYNC:
		ei_sync_event_send_done(event);
		ei_pingpong_unref(event->sync.pingpong);
		break;
	}
	ei_device_unref(event->device);
	ei_seat_unref(event->seat);
}

static OBJECT_IMPLEMENT_CREATE(ei_event);
static OBJECT_IMPLEMENT_PARENT(ei_event, ei);

struct ei_event *
ei_event_new(struct ei *ei)
{
	return ei_event_create(&ei->object);
}

struct ei_event *
ei_event_new_for_device(struct ei_device *device)
{
	struct ei_event *event = ei_event_new(ei_device_get_context(device));

	event->seat = ei_seat_ref(ei_device_get_seat(device));
	event->device = ei_device_ref(device);

	return event;
}

struct ei *
ei_event_get_context(struct ei_event *event)
{
	return ei_event_parent(event);
}

static inline bool
check_event_type(struct ei_event *event, const char *function_name, ...)
{
	bool rc = false;
	va_list args;
	unsigned int type_permitted;
	enum ei_event_type type = ei_event_get_type(event);

	va_start(args, function_name);
	type_permitted = va_arg(args, unsigned int);

	while (type_permitted != (unsigned int)-1) {
		if (type_permitted == type) {
			rc = true;
			break;
		}
		type_permitted = va_arg(args, unsigned int);
	}

	va_end(args);

	if (!rc)
		log_bug_client(ei_event_get_context(event),
			       "Invalid event type (%s) %u passed to %s()",
			       ei_event_type_to_string(type),
			       type,
			       function_name);

	return rc;
}
#define require_event_type(event_, retval_, ...)	\
	if (!check_event_type(event_, __func__, __VA_ARGS__, -1)) \
		return retval_;

_public_ uint32_t
ei_event_emulating_get_sequence(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_DEVICE_START_EMULATING);

	return event->start_emulating.sequence;
}

_public_ uint32_t
ei_event_keyboard_get_xkb_mods_depressed(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_KEYBOARD_MODIFIERS);

	return event->modifiers.depressed;
}

_public_ uint32_t
ei_event_keyboard_get_xkb_mods_latched(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_KEYBOARD_MODIFIERS);

	return event->modifiers.latched;
}

_public_ uint32_t
ei_event_keyboard_get_xkb_mods_locked(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_KEYBOARD_MODIFIERS);

	return event->modifiers.locked;
}

_public_ uint32_t
ei_event_keyboard_get_xkb_group(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_KEYBOARD_MODIFIERS);

	return event->modifiers.group;
}

_public_ struct ei_ping *
ei_event_pong_get_ping(struct ei_event *event)
{
	require_event_type(event, NULL, EI_EVENT_PONG);

	return event->pong.ping;
}

_public_ double
ei_event_pointer_get_dx(struct ei_event *event)
{
	require_event_type(event, 0.0, EI_EVENT_POINTER_MOTION) return event->pointer.dx;
}

_public_ double
ei_event_pointer_get_dy(struct ei_event *event)
{
	require_event_type(event, 0.0, EI_EVENT_POINTER_MOTION);

	return event->pointer.dy;
}

_public_ double
ei_event_pointer_get_absolute_x(struct ei_event *event)
{
	require_event_type(event, 0.0, EI_EVENT_POINTER_MOTION_ABSOLUTE);

	return event->pointer.absx;
}

_public_ double
ei_event_pointer_get_absolute_y(struct ei_event *event)
{
	require_event_type(event, 0.0, EI_EVENT_POINTER_MOTION_ABSOLUTE);

	return event->pointer.absy;
}

_public_ uint32_t
ei_event_button_get_button(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_BUTTON_BUTTON);

	return event->pointer.button;
}

_public_ bool
ei_event_button_get_is_press(struct ei_event *event)
{
	require_event_type(event, false, EI_EVENT_BUTTON_BUTTON);

	return event->pointer.button_is_press;
}

_public_ double
ei_event_scroll_get_dx(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_SCROLL_DELTA);
	return event->pointer.sx;
}

_public_ double
ei_event_scroll_get_dy(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_SCROLL_DELTA);
	return event->pointer.sy;
}

_public_ int32_t
ei_event_scroll_get_discrete_dx(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_SCROLL_DISCRETE);
	return event->pointer.sdx;
}

_public_ int32_t
ei_event_scroll_get_discrete_dy(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_SCROLL_DISCRETE);
	return event->pointer.sdy;
}

_public_ bool
ei_event_scroll_get_stop_x(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_SCROLL_STOP, EI_EVENT_SCROLL_CANCEL);
	return event->pointer.stop_x;
}

_public_ bool
ei_event_scroll_get_stop_y(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_SCROLL_STOP, EI_EVENT_SCROLL_CANCEL);
	return event->pointer.stop_y;
}

_public_ uint32_t
ei_event_keyboard_get_key(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_KEYBOARD_KEY);

	return event->keyboard.key;
}

_public_ bool
ei_event_keyboard_get_key_is_press(struct ei_event *event)
{
	require_event_type(event, false, EI_EVENT_KEYBOARD_KEY);

	return event->keyboard.key_is_press;
}

_public_ uint32_t
ei_event_touch_get_id(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_TOUCH_DOWN, EI_EVENT_TOUCH_UP, EI_EVENT_TOUCH_MOTION);

	return event->touch.touchid;
}

_public_ double
ei_event_touch_get_x(struct ei_event *event)
{
	require_event_type(event, 0.0, EI_EVENT_TOUCH_DOWN, EI_EVENT_TOUCH_MOTION);

	return event->touch.x;
}

_public_ double
ei_event_touch_get_y(struct ei_event *event)
{
	require_event_type(event, 0.0, EI_EVENT_TOUCH_DOWN, EI_EVENT_TOUCH_MOTION);

	return event->touch.y;
}

_public_ bool
ei_event_touch_get_is_cancel(struct ei_event *event)
{
	require_event_type(event, false, EI_EVENT_TOUCH_UP);

	return event->touch.is_cancel;
}

_public_ uint32_t
ei_event_text_get_keysym(struct ei_event *event)
{
	require_event_type(event, 0, EI_EVENT_TEXT_KEYSYM);

	return event->text.keysym;
}

_public_ bool
ei_event_text_get_keysym_is_press(struct ei_event *event)
{
	require_event_type(event, false, EI_EVENT_TEXT_KEYSYM);

	return event->text.is_press;
}

_public_ const char *
ei_event_text_get_utf8(struct ei_event *event)
{
	require_event_type(event, NULL, EI_EVENT_TEXT_UTF8);

	return event->text.utf8;
}

_public_ uint64_t
ei_event_get_time(struct ei_event *event)
{
	require_event_type(event,
			   0,
			   EI_EVENT_POINTER_MOTION,
			   EI_EVENT_POINTER_MOTION_ABSOLUTE,
			   EI_EVENT_BUTTON_BUTTON,
			   EI_EVENT_SCROLL_DELTA,
			   EI_EVENT_SCROLL_STOP,
			   EI_EVENT_SCROLL_CANCEL,
			   EI_EVENT_SCROLL_DISCRETE,
			   EI_EVENT_KEYBOARD_KEY,
			   EI_EVENT_TOUCH_DOWN,
			   EI_EVENT_TOUCH_UP,
			   EI_EVENT_TOUCH_MOTION,
			   EI_EVENT_TEXT_KEYSYM,
			   EI_EVENT_TEXT_UTF8,
			   EI_EVENT_FRAME);

	return event->timestamp;
}
