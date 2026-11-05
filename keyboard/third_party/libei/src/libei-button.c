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
ei_button_destroy(struct ei_button *button)
{
	struct ei *ei = ei_button_get_context(button);
	ei_unregister_object(ei, &button->proto_object);
}

OBJECT_IMPLEMENT_REF(ei_button);
OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_button);

static OBJECT_IMPLEMENT_CREATE(ei_button);
static OBJECT_IMPLEMENT_PARENT(ei_button, ei_device);
OBJECT_IMPLEMENT_GETTER_AS_REF(ei_button, proto_object, const struct brei_object *);

struct ei_device *
ei_button_get_device(struct ei_button *button)
{
	return ei_button_parent(button);
}

struct ei *
ei_button_get_context(struct ei_button *button)
{
	return ei_device_get_context(ei_button_get_device(button));
}

const struct ei_button_interface *
ei_button_get_interface(struct ei_button *button)
{
	struct ei_device *device = ei_button_get_device(button);
	return ei_device_get_button_interface(device);
}

struct ei_button *
ei_button_new(struct ei_device *device, object_id_t id, uint32_t version)
{
	struct ei_button *button = ei_button_create(&device->object);
	struct ei *ei = ei_device_get_context(device);

	button->proto_object.id = id;
	button->proto_object.implementation = button;
	button->proto_object.interface = &ei_button_proto_interface;
	button->proto_object.version = version;
	ei_register_object(ei, &button->proto_object);

	return button; /* ref owned by caller */
}
