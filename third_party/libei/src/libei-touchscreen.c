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

#include "config.h"

#include <errno.h>
#include <stdbool.h>

#include "util-bits.h"
#include "util-io.h"
#include "util-macros.h"
#include "util-mem.h"
#include "util-strings.h"
#include "util-version.h"

#include "ei-proto.h"
#include "libei-private.h"

static void
ei_touchscreen_destroy(struct ei_touchscreen *touchscreen)
{
	struct ei *ei = ei_touchscreen_get_context(touchscreen);
	ei_unregister_object(ei, &touchscreen->proto_object);
}

OBJECT_IMPLEMENT_REF(ei_touchscreen);
OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_touchscreen);

static OBJECT_IMPLEMENT_CREATE(ei_touchscreen);
static OBJECT_IMPLEMENT_PARENT(ei_touchscreen, ei_device);
OBJECT_IMPLEMENT_GETTER_AS_REF(ei_touchscreen, proto_object, const struct brei_object *);

struct ei_device *
ei_touchscreen_get_device(struct ei_touchscreen *touchscreen)
{
	return ei_touchscreen_parent(touchscreen);
}

struct ei *
ei_touchscreen_get_context(struct ei_touchscreen *touchscreen)
{
	return ei_device_get_context(ei_touchscreen_get_device(touchscreen));
}

const struct ei_touchscreen_interface *
ei_touchscreen_get_interface(struct ei_touchscreen *touchscreen)
{
	struct ei_device *device = ei_touchscreen_get_device(touchscreen);
	return ei_device_get_touchscreen_interface(device);
}

struct ei_touchscreen *
ei_touchscreen_new(struct ei_device *device, object_id_t id, uint32_t version)
{
	struct ei_touchscreen *touchscreen = ei_touchscreen_create(&device->object);
	struct ei *ei = ei_device_get_context(device);

	touchscreen->proto_object.id = id;
	touchscreen->proto_object.implementation = touchscreen;
	touchscreen->proto_object.interface = &ei_touchscreen_proto_interface;
	touchscreen->proto_object.version = version;
	ei_register_object(ei, &touchscreen->proto_object);

	return touchscreen; /* ref owned by caller */
}
