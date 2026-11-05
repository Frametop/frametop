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
ei_pingpong_destroy(struct ei_pingpong *pingpong)
{
	struct ei *ei = ei_pingpong_get_context(pingpong);
	ei_unregister_object(ei, &pingpong->proto_object);
}

OBJECT_IMPLEMENT_REF(ei_pingpong);
OBJECT_IMPLEMENT_UNREF_CLEANUP(ei_pingpong);
OBJECT_IMPLEMENT_GETTER(ei_pingpong, user_data, void *);
OBJECT_IMPLEMENT_SETTER(ei_pingpong, user_data, void *);
OBJECT_IMPLEMENT_GETTER_AS_REF(ei_pingpong, proto_object, const struct brei_object *);

static OBJECT_IMPLEMENT_CREATE(ei_pingpong);
static OBJECT_IMPLEMENT_PARENT(ei_pingpong, ei);

struct ei *
ei_pingpong_get_context(struct ei_pingpong *pingpong)
{
	assert(pingpong);
	return ei_pingpong_parent(pingpong);
}

object_id_t
ei_pingpong_get_id(struct ei_pingpong *pingpong)
{
	return pingpong->proto_object.id;
}

static const struct ei_pingpong_interface interface = {};

const struct ei_pingpong_interface *
ei_pingpong_get_interface(struct ei_pingpong *pingpong)
{
	return &interface;
}

struct ei_pingpong *
ei_pingpong_new_for_id(struct ei *ei, object_id_t id, uint32_t version)
{
	struct ei_pingpong *pingpong = ei_pingpong_create(&ei->object);

	pingpong->proto_object.id = id;
	pingpong->proto_object.implementation = pingpong;
	pingpong->proto_object.interface = &ei_pingpong_proto_interface;
	pingpong->proto_object.version = version; /* FIXME */
	ei_register_object(ei, &pingpong->proto_object);
	list_init(&pingpong->link);

	return pingpong; /* ref owned by caller */
}
