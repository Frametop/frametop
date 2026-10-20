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

/**
 * This is an abstraction layer for ref-counted objects with an optional
 * parent. It cuts down on boilerplate by providing a bunch of macros
 * that generate the various required functions.
 * Limitation: the object must be the first item in the parent struct.
 */

#pragma once

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

struct object;

/**
 * Function called when the last reference is unref'd. Clean up **internal**
 * state of the object, the object itself is freed by the generated
 * functions.
 */
typedef void (*object_destroy_func_t)(struct object *object);

/* internal implementation, do not use directly */
struct object {
	struct object *parent; /* may be NULL */
	uint32_t refcount;
	object_destroy_func_t destroy;
};

/* internal implementation, do not call directly */
static inline void
object_init(struct object *object, struct object *parent, object_destroy_func_t destroy)
{
	object->refcount = 1;
	object->destroy = destroy;
	object->parent = parent;
}

/* internal implementation, do not call directly */
static inline void
object_destroy(struct object *object)
{
	if (object->destroy)
		object->destroy(object);
	free(object);
}

/* internal implementation, do not call directly */
static inline void *
object_ref(struct object *object)
{
	assert(object->refcount >= 1);
	++object->refcount;
	return object;
}

/* internal implementation, do not call directly */
static inline void *
object_unref(struct object *object)
{
	assert(object->refcount >= 1);
	if (--object->refcount == 0)
		object_destroy(object);
	return NULL;
}

/**
 * For a type of "foo", declare
 *    struct foo *foo_ref(struct foo *f);
 */
#define OBJECT_DECLARE_REF(type_) \
struct type_ * type_##_ref(struct type_ *obj)

/**
 * For a type of "foo", declare
 *    struct foo *foo_unref(struct foo *f);
 */
#define OBJECT_DECLARE_UNREF(type_) \
struct type_ * type_##_unref(struct type_ *obj)

/**
 * For a type of "foo", generate
 *    struct foo *foo_ref(struct foo *f);
 */
#define OBJECT_IMPLEMENT_REF(type_) \
struct type_ * type_##_ref(struct type_ *obj) { \
	object_ref(&obj->object); \
	return obj; \
}

/**
 * For a type of "foo", generate
 *    struct foo *foo_unref(struct foo *f);
 * The function always returns NULL, when the last reference is removed the
 * object is freed.
 */
#define OBJECT_IMPLEMENT_UNREF(type_) \
struct type_ * type_##_unref(struct type_ *obj) { \
	if (!obj) return NULL; \
	return object_unref(&obj->object); \
} \
struct __useless_struct_to_allow_trailing_semicolon__

/**
 * Same as OBJECT_IMPLEMENT_UNREF() but also
 * also defines a unrefp function, use with
 *    __attribute__((cleanup(foo_unrefp))
 */
#define OBJECT_IMPLEMENT_UNREF_CLEANUP(type_) \
OBJECT_IMPLEMENT_UNREF(type_); \
__attribute__((used)) static inline void type_##_unrefp(struct type_ **p_) { \
	if (*p_) type_##_unref(*p_); \
} \
struct __useless_struct_to_allow_trailing_semicolon__

/**
 * For a type for "foo", generate
 *    void foo_init_object(struct foo *f)
 * which sets up the *object* part of the foo struct. Use this where
 * allocation through foo_create isn't suitable.
 */
#define OBJECT_IMPLEMENT_INIT(type_) \
void type_##_init_object(struct type_ *t, struct object *parent) { \
	assert((intptr_t)t == (intptr_t)&t->object && "field 'object' must be first one in the struct"); \
	object_init(&t->object, parent, (object_destroy_func_t)type_##_destroy); \
} \
struct __useless_struct_to_allow_trailing_semicolon__

/**
 * For a type for "foo", generate
 *    struct foo *foo_create(struct object *parent)
 * which returns an callocated and initialized foo struct.
 */
#define OBJECT_IMPLEMENT_CREATE(type_) \
struct type_ * type_##_create(struct object *parent) { \
	struct type_ *t = calloc(1, sizeof *t); \
	assert((intptr_t)t == (intptr_t)&t->object && "field 'object' must be first one in the struct"); \
	assert(t != NULL); \
	object_init(&t->object, parent, (object_destroy_func_t)type_##_destroy); \
	return t; \
} \
struct __useless_struct_to_allow_trailing_semicolon__

/**
 * For a type "foo" with parent type "bar", generate
 *    struct bar* foo_parent(struct foo *foo)
 */
#define OBJECT_IMPLEMENT_PARENT(type_, parent_type_) \
struct parent_type_ * type_##_parent(struct type_ *o) { \
	return container_of(o->object.parent, struct parent_type_, object); \
} \
struct __useless_struct_to_allow_trailing_semicolon__

/**
 * Declares a simple `bartype foo_get_bar(foo)` function.
 */
#define OBJECT_DECLARE_GETTER(type_, field_, rtype_) \
rtype_ type_##_get_##field_(struct type_ *obj)

/**
 * Declares a simple `void foo_set_bar(foo, bar)` function.
 */
#define OBJECT_DECLARE_SETTER(type_, field_, rtype_) \
void type_##_set_##field_(struct type_ *obj, rtype_ val_)

/**
 * Generate a simple getter function for the given type, field and return
 * type.
 */
#define OBJECT_IMPLEMENT_GETTER(type_, field_, rtype_) \
rtype_ type_##_get_##field_(struct type_ *obj) { \
	return obj->field_; \
} \
struct __useless_struct_to_allow_trailing_semicolon__

/**
 * Generate a simple getter function for the given type, field and return
 * type as a pointer to the field.
 */
#define OBJECT_IMPLEMENT_GETTER_AS_REF(type_, field_, rtype_) \
rtype_ type_##_get_##field_(struct type_ *obj) { \
	return &obj->field_; \
} \
struct __useless_struct_to_allow_trailing_semicolon__

/**
 * Generate a simple setter function for the given type, field and return
 * type.
 */
#define OBJECT_IMPLEMENT_SETTER(type_, field_, rtype_) \
void type_##_set_##field_(struct type_ *obj, rtype_ val_) { \
	obj->field_ = (val_); \
} \
struct __useless_struct_to_allow_trailing_semicolon__
