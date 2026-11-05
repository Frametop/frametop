/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Red Hat, Inc.
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
ei_text_destroy(struct ei_text *text)
{
	struct ei *ei = ei_text_get_context(text);
	ei_unregister_object(ei, &text->proto_object);
}

OBJECT_IMPLEMENT_REF(ei_text);
OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_text);

static OBJECT_IMPLEMENT_CREATE(ei_text);
static OBJECT_IMPLEMENT_PARENT(ei_text, ei_device);
OBJECT_IMPLEMENT_GETTER_AS_REF(ei_text, proto_object, const struct brei_object *);

struct ei_device *
ei_text_get_device(struct ei_text *text)
{
	return ei_text_parent(text);
}

struct ei *
ei_text_get_context(struct ei_text *text)
{
	return ei_device_get_context(ei_text_get_device(text));
}

const struct ei_text_interface *
ei_text_get_interface(struct ei_text *text)
{
	struct ei_device *device = ei_text_get_device(text);
	return ei_device_get_text_interface(device);
}

struct ei_text *
ei_text_new(struct ei_device *device, object_id_t id, uint32_t version)
{
	struct ei_text *text = ei_text_create(&device->object);
	struct ei *ei = ei_device_get_context(device);

	text->proto_object.id = id;
	text->proto_object.implementation = text;
	text->proto_object.interface = &ei_text_proto_interface;
	text->proto_object.version = version;
	ei_register_object(ei, &text->proto_object);

	return text; /* ref owned by caller */
}
