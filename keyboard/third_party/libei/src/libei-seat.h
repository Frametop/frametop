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
#include "ei-proto.h"

struct ei;

enum ei_seat_state {
	EI_SEAT_STATE_NEW,
	EI_SEAT_STATE_DONE,
	EI_SEAT_STATE_REMOVED,
};

struct ei_seat {
	struct object object;
	void *user_data;

	struct brei_object proto_object;

	struct list link;
	enum ei_seat_state state;
	struct list devices;
	struct list devices_removed; /* removed from seat but client still has a ref */

	struct {
		/* Maps the interface as index to the capability bitmask on the protocol */
		uint64_t map[EI_INTERFACE_COUNT];
		/* A bitmask of the bitmask the client bound last */
		uint64_t bound;
	} capabilities;

	char *name;
};

OBJECT_DECLARE_GETTER(ei_seat, id, object_id_t);
OBJECT_DECLARE_GETTER(ei_seat, proto_object, const struct brei_object *);
OBJECT_DECLARE_GETTER(ei_seat, interface, const struct ei_seat_interface *);

struct ei_seat *
ei_seat_new(struct ei *ei, object_id_t id, uint32_t version);

void
ei_seat_remove(struct ei_seat *seat);

void
ei_seat_drop(struct ei_seat *seat);
