
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

struct ei;
struct ei_handshake;

typedef void (*ei_handshake_func)(struct ei_handshake *handshake,
				  void *handshake_data,
				  uint32_t proto_data);

/* This is a protocol-only object, not exposed in the API */
struct ei_handshake {
	struct object object;
	struct brei_object proto_object;
	void *user_data; /* Note: user-data is attached to the object */
};

OBJECT_DECLARE_GETTER(ei_handshake, context, struct ei *);
OBJECT_DECLARE_GETTER(ei_handshake, proto_object, const struct brei_object *);
OBJECT_DECLARE_GETTER(ei_handshake, id, object_id_t);
OBJECT_DECLARE_GETTER(ei_handshake, version, uint32_t);
OBJECT_DECLARE_GETTER(ei_handshake, interface, const struct ei_handshake_interface *);
OBJECT_DECLARE_GETTER(ei_handshake, user_data, void *);
OBJECT_DECLARE_SETTER(ei_handshake, user_data, void *);
OBJECT_DECLARE_REF(ei_handshake);
OBJECT_DECLARE_UNREF(ei_handshake);

struct ei_handshake *
ei_handshake_new(struct ei *ei, uint32_t version);
