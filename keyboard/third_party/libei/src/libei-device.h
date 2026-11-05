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
#include "util-object.h"

#include "brei-shared.h"
#include "libei-keyboard.h"
#include "libei-pointer.h"
#include "libei-text.h"
#include "libei-touchscreen.h"
#include "libei.h"

enum ei_device_state {
	/* Before the DeviceAddedDone was received */
	EI_DEVICE_STATE_NEW,
	EI_DEVICE_STATE_PAUSED,
	EI_DEVICE_STATE_RESUMED,
	EI_DEVICE_STATE_EMULATING,
	/**
	 * Client removed the device, we no longer accept events from the
	 * client
	 */
	EI_DEVICE_STATE_REMOVED_FROM_CLIENT,
	/**
	 * Server removed the device, we need to remove it ourselves now.
	 */
	EI_DEVICE_STATE_REMOVED_FROM_SERVER,
	/**
	 * Device has been removed by both sides.
	 */
	EI_DEVICE_STATE_DEAD,
};

struct ei_device {
	struct object object;
	void *user_data;
	struct brei_object proto_object;

	struct ei_pointer *pointer;
	struct ei_pointer_absolute *pointer_absolute;
	struct ei_scroll *scroll;
	struct ei_button *button;
	struct ei_keyboard *keyboard;
	struct ei_touchscreen *touchscreen;
	struct ei_text *text;

	struct list link;
	enum ei_device_state state;
	uint32_t capabilities;
	char *name;
	enum ei_device_type type;

	struct list pending_event_queue; /* incoming events waiting for a frame */

	bool send_frame_event;

	uint32_t width, height;

	struct list regions;

	struct {
		bool x_is_stopped, y_is_stopped;
		bool x_is_cancelled, y_is_cancelled;
	} scroll_state;

	struct ei_keymap *keymap;

	char *pending_region_mapping_id;
};

struct ei_keymap {
	struct object object;
	struct ei_device *device;
	void *user_data;
	enum ei_keymap_type type;
	int fd;
	size_t size;
	bool assigned;
};

struct ei_touch {
	struct object object;
	struct ei_device *device;
	void *user_data;
	uint32_t tracking_id;
	enum {
		TOUCH_IS_NEW,
		TOUCH_IS_DOWN,
		TOUCH_IS_UP,
	} state;

	double x, y;
};

OBJECT_DECLARE_GETTER(ei_device, id, object_id_t);
OBJECT_DECLARE_GETTER(ei_device, proto_object, const struct brei_object *);
OBJECT_DECLARE_GETTER(ei_device, interface, const struct ei_device_interface *);
OBJECT_DECLARE_GETTER(ei_device, pointer_interface, const struct ei_pointer_interface *);
OBJECT_DECLARE_GETTER(ei_device,
		      pointer_absolute_interface,
		      const struct ei_pointer_absolute_interface *);
OBJECT_DECLARE_GETTER(ei_device, scroll_interface, const struct ei_scroll_interface *);
OBJECT_DECLARE_GETTER(ei_device, button_interface, const struct ei_button_interface *);
OBJECT_DECLARE_GETTER(ei_device, keyboard_interface, const struct ei_keyboard_interface *);
OBJECT_DECLARE_GETTER(ei_device, touchscreen_interface, const struct ei_touchscreen_interface *);
OBJECT_DECLARE_GETTER(ei_device, text_interface, const struct ei_text_interface *);
OBJECT_DECLARE_SETTER(ei_device, type, enum ei_device_type);
OBJECT_DECLARE_SETTER(ei_device, name, const char *);
OBJECT_DECLARE_SETTER(ei_device, seat, const char *);

struct ei_device *
ei_device_new(struct ei_seat *seat, object_id_t deviceid, uint32_t version);

void
ei_device_add_region(struct ei_device *device, struct ei_region *r);

void
ei_device_done(struct ei_device *device);

void
ei_device_removed_by_server(struct ei_device *device);

void
ei_device_event_start_emulating(struct ei_device *device, uint32_t sequence);

void
ei_device_event_stop_emulating(struct ei_device *device);

void
ei_device_added(struct ei_device *device);

void
ei_device_paused(struct ei_device *device);

void
ei_device_resumed(struct ei_device *device);

void
ei_device_set_size(struct ei_device *device, uint32_t width, uint32_t height);

void
ei_device_set_keymap(struct ei_device *device,
		     enum ei_keymap_type type,
		     int keymap_fd,
		     size_t size);
