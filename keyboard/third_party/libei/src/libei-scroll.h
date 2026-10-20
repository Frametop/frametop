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

#pragma once

#include "util-list.h"
#include "util-object.h"

#include "brei-shared.h"

struct ei;
struct ei_device;
struct ei_scroll;

/* This is a protocol-only object, not exposed in the API */
struct ei_scroll {
	struct object object;
	struct brei_object proto_object;
};

OBJECT_DECLARE_GETTER(ei_scroll, context, struct ei *);
OBJECT_DECLARE_GETTER(ei_scroll, device, struct ei_device *);
OBJECT_DECLARE_GETTER(ei_scroll, proto_object, const struct brei_object *);
OBJECT_DECLARE_GETTER(ei_scroll, interface, const struct ei_scroll_interface *);
OBJECT_DECLARE_REF(ei_scroll);
OBJECT_DECLARE_UNREF(ei_scroll);

struct ei_scroll *
ei_scroll_new(struct ei_device *device, object_id_t id, uint32_t version);
