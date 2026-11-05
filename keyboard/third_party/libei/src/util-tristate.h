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
 * A type-safe tristate implementation. A tristate value has three options,
 * usually a logical 'on' and 'off' plus the 'unset' value.
 *
 * Usage:
 *
 *   DEFINE_TRISTATE(yes, no, unset);
 *   DEFINE_TRISTATE(on, off, neither);
 *
 *   tristate t = tristate_unset;
 *   if (something)
 *	t = tristate_yes;
 *   else if (something_else)
 *      t = tristate_no;
 *
 *  if (tristate_is_yes(t))
 *      printf("yep");
 *
 *  switch (tristate_value(t)) {
 *  case tristate_val_yes:
 *  case tristate_val_no:
 *  case tristate_val_unset:
 *  }
 *
 *  Basic type safety is provided - mixing tristates types causes an
 *  abort(). For example:
 *
 *  DEFINE_TRISTATE(yes, no, unset);
 *  DEFINE_TRISTATE(on, off, neither);
 *
 *  tristate t1 = tristate_unset;
 *  tristate t2 = tristate_off;
 *  tristate t3 = tristate_neither;
 *
 *  t2 and t3 have the same "type". t1 is a different "type".
 *
 *  tristate_is_neither(t1) // this will abort
 *  tristate_is_yes(t2) // this will abort
 */
#pragma once

#include <stdbool.h>

typedef struct {
	unsigned _val;
} tristate;

/* Implementation detail:
 * Tristate value is type_mask | val
 *   where val are the 2 LSB with
 *      11 ... logical true state
 *      10 ... logical false state
 *      00 ... unset state
 * All other bits are the type mask. This type mask is used to check that
 * two different tristate definitions cannot be intermixed.
 */
static const unsigned _TRISTATE_TYPE_MASK = ~0x3;

/* implementation detail, ignore */
__attribute__((used)) static inline void
_tristate_check_type(const tristate *t1, unsigned type)
{
	assert((t1->_val & _TRISTATE_TYPE_MASK) == type || !"Invalid tristate type comparison");
}

/**
 * For the three given arguments on, off and none, define:
 * - tristate_on, tristate_off and tristate_none as constant values to
 *   assign. For example: tristate t = tristate_on;
 * - tristate_is_on(), tristate_is_off(), tristate_is_none() as functions to check
 *   a tristate. This function will abort if different tristate types are
 *   mixed. For example:
 *      tristate t = tristate_on;
 *      if (tristate_is_none(t)) { .... }
 * - tristate_onoff_value() to retrieve the value from a tristate to be used
 *   in e.g. a switch statement. The values are tristate_val_on,
 *   tristate_val_off, trisate_val_none. For example:
 *   switch(tristate_onoff_value(t)) {
 *      case tristate_val_on: break;
 *      case tristate_val_off: break;
 *      case tristate_val_none: break;
 *    }
 */
#define DEFINE_TRISTATE(_on, _off, _none)						\
	static const unsigned _TRISTATE_TYPE_##_on##_off = (__LINE__ << 2);		\
	static const unsigned tristate_val_##_on   = _TRISTATE_TYPE_##_on##_off | 3;	\
	static const unsigned tristate_val_##_off  = _TRISTATE_TYPE_##_on##_off | 2;	\
	static const unsigned tristate_val_##_none = _TRISTATE_TYPE_##_on##_off | 0;	\
	static const tristate tristate_##_on =   { ._val = tristate_val_##_on };	\
	static const tristate tristate_##_off =  { ._val = tristate_val_##_off };	\
	static const tristate tristate_##_none = { ._val = tristate_val_##_none };	\
	static inline bool tristate_is_##_on(tristate t) {				\
		_tristate_check_type(&t, _TRISTATE_TYPE_##_on##_off);			\
		return t._val == tristate_##_on._val;					\
	}										\
	__attribute__((used)) static inline bool tristate_is_##_off(tristate t) {				\
		_tristate_check_type(&t, _TRISTATE_TYPE_##_on##_off);			\
		return t._val == tristate_##_off._val;					\
	}										\
	__attribute__((used)) static inline bool tristate_is_##_none(tristate t) {				\
		_tristate_check_type(&t, _TRISTATE_TYPE_##_on##_off);			\
		return t._val == tristate_##_none._val;					\
	}										\
	__attribute__((used)) static inline signed char tristate_##_on##_off##_value(tristate t) {		\
		_tristate_check_type(&t, _TRISTATE_TYPE_##_on##_off);			\
		return t._val;								\
	}										\
	__attribute__((used)) static inline const char *tristate_##_on##_off##_name(tristate t) {		\
		if (tristate_is_##_on(t)) return #_on;					\
		if (tristate_is_##_off(t)) return #_off;				\
		if (tristate_is_##_none(t)) return #_none;				\
		assert(!"Invalid tristate value");					\
	}										\
	struct __useless_struct_to_allow_trailing_semicolon__
