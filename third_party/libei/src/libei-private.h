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

#include <inttypes.h>
#include <stdarg.h>

#include "util-list.h"
#include "util-macros.h"
#include "util-object.h"
#include "util-sources.h"
#include "util-structs.h"

#include "brei-shared.h"
#include "libei-button.h"
#include "libei-callback.h"
#include "libei-connection.h"
#include "libei-device.h"
#include "libei-event.h"
#include "libei-handshake.h"
#include "libei-keyboard.h"
#include "libei-pingpong.h"
#include "libei-pointer-absolute.h"
#include "libei-pointer.h"
#include "libei-region.h"
#include "libei-scroll.h"
#include "libei-seat.h"
#include "libei-text.h"
#include "libei-touchscreen.h"
#include "libei.h"

struct ei_backend_interface {
	void (*destroy)(struct ei *ei, void *backend);
};

enum ei_state {
	EI_STATE_NEW,           /* No backend yet */
	EI_STATE_BACKEND,       /* We have a backend */
	EI_STATE_CONNECTING,    /* client requested connect */
	EI_STATE_CONNECTED,     /* server has sent connect */
	EI_STATE_DISCONNECTING, /* in the process of cleaning up */
	EI_STATE_DISCONNECTED,
};

struct ei_interface_versions {
	uint32_t ei_connection;
	uint32_t ei_handshake;
	uint32_t ei_callback;
	uint32_t ei_pingpong;
	uint32_t ei_seat;
	uint32_t ei_device;
	uint32_t ei_pointer;
	uint32_t ei_pointer_absolute;
	uint32_t ei_scroll;
	uint32_t ei_button;
	uint32_t ei_keyboard;
	uint32_t ei_touchscreen;
	uint32_t ei_text;
};

struct ei_unsent {
	struct list node;
	struct iobuf *buf;
};

struct ei_defunct_object {
	struct list node;
	object_id_t object_id;
	uint64_t time;
};

struct ei {
	struct object object;

	struct ei_connection *connection;
	struct ei_handshake *handshake;
	struct ei_interface_versions interface_versions;
	struct list proto_objects; /* brei_object list */
	struct list defunct_objects;
	object_id_t next_object_id;

	uint32_t serial;

	void *user_data;
	struct brei_context *brei;
	struct sink *sink;
	struct source *source;
	struct list unsent_queue;

	struct ei_backend_interface backend_interface;
	void *backend;
	enum ei_state state;
	struct list event_queue;
	struct list seats;
	char *name;

	struct {
		ei_log_handler handler;
		enum ei_log_priority priority;
	} log;

	ei_clock_now_func clock_now;

	bool is_sender;
};

const struct ei_connection_interface *
ei_get_interface(struct ei *ei);

int
ei_set_socket(struct ei *ei, int fd);

struct ei *
ei_get_context(struct ei *ei);

struct ei_connection *
ei_get_connection(struct ei *ei);

object_id_t
ei_get_new_id(struct ei *ei);

void
ei_update_serial(struct ei *ei, uint32_t serial);

uint32_t
ei_get_serial(struct ei *ei);

void
ei_register_object(struct ei *ei, struct brei_object *object);

void
ei_unregister_object(struct ei *ei, struct brei_object *object);

int
ei_send_message(struct ei *ei,
		const struct brei_object *object,
		uint32_t opcode,
		const char *signature,
		size_t nargs,
		...);

int
ei_unsent_flush(struct ei *ei);

void
ei_connected(struct ei *ei);

void
ei_queue_connect_event(struct ei *ei);

void
ei_queue_disconnect_event(struct ei *ei);

void
ei_queue_pong_event(struct ei *ei, struct ei_ping *ping);

void
ei_queue_sync_event(struct ei *ei, struct ei_pingpong *ping);

void
ei_queue_device_removed_event(struct ei_device *device);

void
ei_queue_device_added_event(struct ei_device *device);

void
ei_queue_device_resumed_event(struct ei_device *device);

void
ei_queue_device_paused_event(struct ei_device *device);

void
ei_queue_seat_added_event(struct ei_seat *seat);

void
ei_queue_seat_removed_event(struct ei_seat *seat);

void
ei_queue_device_start_emulating_event(struct ei_device *device, uint32_t sequence);

void
ei_queue_device_stop_emulating_event(struct ei_device *device);

void
ei_queue_frame_event(struct ei_device *device, uint64_t time);

void
ei_queue_pointer_rel_event(struct ei_device *device, double x, double y);

void
ei_queue_pointer_abs_event(struct ei_device *device, double x, double y);

void
ei_queue_pointer_button_event(struct ei_device *device, uint32_t button, bool is_press);

void
ei_queue_keyboard_key_event(struct ei_device *device, uint32_t key, bool is_press);

void
ei_queue_keyboard_modifiers_event(struct ei_device *device, const struct ei_xkb_modifiers *mods);

void
ei_queue_pointer_scroll_event(struct ei_device *device, double x, double y);

void
ei_queue_pointer_scroll_discrete_event(struct ei_device *device, int32_t x, int32_t y);

void
ei_queue_pointer_scroll_stop_event(struct ei_device *device, bool x, bool y);

void
ei_queue_pointer_scroll_cancel_event(struct ei_device *device, bool x, bool y);

void
ei_queue_touch_down_event(struct ei_device *device, uint32_t touchid, double x, double y);

void
ei_queue_touch_motion_event(struct ei_device *device, uint32_t touchid, double x, double y);

void
ei_queue_touch_up_event(struct ei_device *device, uint32_t touchid);

void
ei_queue_touch_cancel_event(struct ei_device *device, uint32_t touchid);

void
ei_queue_text_keysym_event(struct ei_device *device, uint32_t keysym, bool is_press);

void
ei_queue_text_utf8_event(struct ei_device *device, const char *utf8);

void
ei_sync_event_send_done(struct ei_event *e);

_printf_(6, 7) void
ei_log_msg(struct ei *ei,
	   enum ei_log_priority priority,
	   const char *file,
	   int lineno,
	   const char *func,
	   const char *format,
	   ...);

_printf_(6, 0) void
ei_log_msg_va(struct ei *ei,
	      enum ei_log_priority priority,
	      const char *file,
	      int lineno,
	      const char *func,
	      const char *format,
	      va_list args);

#define log_debug(T_, ...) \
	ei_log_msg((T_), EI_LOG_PRIORITY_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_info(T_, ...) \
	ei_log_msg((T_), EI_LOG_PRIORITY_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_warn(T_, ...) \
	ei_log_msg((T_), EI_LOG_PRIORITY_WARNING, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_error(T_, ...) \
	ei_log_msg((T_), EI_LOG_PRIORITY_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_bug(T_, ...) \
	ei_log_msg((T_), EI_LOG_PRIORITY_ERROR, __FILE__, __LINE__, __func__, "🪳  libei bug:  " __VA_ARGS__)
#define log_bug_client(T_, ...) \
	ei_log_msg((T_), EI_LOG_PRIORITY_ERROR, __FILE__, __LINE__, __func__, "🪲  Bug: " __VA_ARGS__)

#define DISCONNECT_IF_INVALID_VERSION(ei_, intf_, id_, version_) do { \
	struct ei *_ei = (ei_); \
	uint32_t _version = (version_); \
	uint64_t _id = (id_); \
	if (_ei->interface_versions.intf_ < _version) { \
		log_bug(ei_, "Received invalid version %u for object id %#" PRIx64 ". Disconnecting", _version, _id); \
		return brei_result_new(EI_CONNECTION_DISCONNECT_REASON_PROTOCOL, "Received invalid version %u for object id %#" PRIx64 ".", _version, _id); \
	} \
} while(0)
