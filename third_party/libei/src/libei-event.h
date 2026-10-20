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

#include "libei.h"

struct ei;

struct ei_xkb_modifiers {
	uint32_t depressed;
	uint32_t latched;
	uint32_t locked;
	uint32_t group;
};

struct ei_event {
	struct object object; /* Parent is struct ei */
	enum ei_event_type type;
	struct list link;
	struct ei_seat *seat; /* NULL if device is non-NULL */
	struct ei_device *device;

	uint64_t timestamp;

	union {
		struct ei_xkb_modifiers modifiers;
		struct {
			double dx, dy;       /* relative motion */
			double absx, absy;   /* absolute motion */
			double sx, sy;       /* scroll */
			int32_t sdx, sdy;    /* discrete scroll */
			bool stop_x, stop_y; /* scroll stop */
			uint32_t button;
			bool button_is_press;
		} pointer;
		struct {
			uint32_t key;
			bool key_is_press;
		} keyboard;
		struct {
			uint32_t touchid;
			double x, y;
			bool is_cancel;
		} touch;
		struct {
			uint32_t keysym;
			bool is_press;
			char *utf8;
		} text;
		struct {
			uint32_t sequence;
		} start_emulating;
		struct {
			struct ei_ping *ping;
		} pong;
		struct {
			struct ei_pingpong *pingpong;
		} sync;
	};
};

struct ei_event *
ei_event_new(struct ei *ei);

struct ei_event *
ei_event_new_for_device(struct ei_device *device);

struct ei *
ei_event_get_context(struct ei_event *event);
