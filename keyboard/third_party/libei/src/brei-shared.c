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

#include "config.h"

#include <inttypes.h>
#include <stdbool.h>

#include "util-io.h"
#include "util-mem.h"
#include "util-strings.h"

#include "brei-proto.h"
#include "brei-shared.h"

struct brei_context {
	struct object object;
	void *parent_context;

	brei_logfunc_t log_func;
	void *log_context;
};

struct brei_header {
	object_id_t sender_id;
	uint32_t msglen; /* length of message in bytes including this header */
	uint32_t opcode;
} _packed_;
static_assert(sizeof(struct brei_header) == 16, "Unexpected size for brei_header struct");

/**
 * For a given string length (including null byte) return
 * the number of bytes needed on the protocol, including the
 * 4-byte length field.
 */
static inline uint32_t
brei_string_proto_length(uint32_t slen)
{
	uint32_t length = 4 + slen;
	uint32_t protolen = (length + 3) / 4 * 4;
	assert(protolen % 4 == 0);
	return protolen;
}

static void
brei_context_destroy(struct brei_context *ctx)
{
}

static OBJECT_IMPLEMENT_CREATE(brei_context);
OBJECT_IMPLEMENT_REF(brei_context);
OBJECT_IMPLEMENT_UNREF_CLEANUP(brei_context);
OBJECT_IMPLEMENT_SETTER(brei_context, log_func, brei_logfunc_t);
OBJECT_IMPLEMENT_SETTER(brei_context, log_context, void *);

static void
brei_result_destroy(struct brei_result *res)
{
	free(res->explanation);
}

static OBJECT_IMPLEMENT_CREATE(brei_result);
OBJECT_IMPLEMENT_GETTER(brei_result, data, void *);
OBJECT_IMPLEMENT_SETTER(brei_result, data, void *);
OBJECT_IMPLEMENT_GETTER(brei_result, reason, int);
OBJECT_IMPLEMENT_GETTER(brei_result, explanation, const char *);
OBJECT_IMPLEMENT_REF(brei_result);
OBJECT_IMPLEMENT_UNREF_CLEANUP(brei_result);

struct brei_result *
brei_result_new(int reason, const char *format, ...)
{
	struct brei_result *result = brei_result_create(NULL);
	result->reason = reason;
	if (format) {
		va_list args;
		va_start(args, format);
		result->explanation = xvaprintf(format, args);
		va_end(args);
	} else {
		assert(reason == 0); /* Any error needs an explanation */
	}
	return result;
}

struct brei_result *
brei_result_new_success(void *data)
{
	struct brei_result *result = brei_result_new(0, NULL);

	result->data = data;

	return result;
}

struct brei_result *
brei_result_new_from_neg_errno(int err)
{
	if (err >= 0)
		return NULL;

	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR, "%s", strerror(-err));
}

struct brei_context *
brei_context_new(void *parent_context)
{
	struct brei_context *brei = brei_context_create(NULL);

	brei->parent_context = parent_context;

	return brei;
}

#define log_debug(T_, ...) \
	brei_log_msg((T_), BREI_LOG_PRIORITY_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_info(T_, ...) \
	brei_log_msg((T_), BREI_LOG_PRIORITY_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_warn(T_, ...) \
	brei_log_msg((T_), BREI_LOG_PRIORITY_WARNING, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_error(T_, ...) \
	brei_log_msg((T_), BREI_LOG_PRIORITY_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_bug(T_, ...) \
	brei_log_msg((T_), BREI_LOG_PRIORITY_ERROR, __FILE__, __LINE__, __func__, "🪳  brei bug:  " __VA_ARGS__)
#define log_bug_client(T_, ...) \
	brei_log_msg((T_), BREI_LOG_PRIORITY_ERROR, __FILE__, __LINE__, __func__, "🪲  Bug: " __VA_ARGS__)

_printf_(6, 0) static void
brei_log_msg_va(struct brei_context *brei,
		enum brei_log_priority priority,
		const char *file,
		int lineno,
		const char *func,
		const char *format,
		va_list ap)
{
	if (brei->log_func && brei->log_context)
		brei->log_func(brei->log_context, priority, file, lineno, func, format, ap);
}

_printf_(6, 7) static void
brei_log_msg(struct brei_context *brei,
	     enum brei_log_priority priority,
	     const char *file,
	     int lineno,
	     const char *func,
	     const char *format,
	     ...)
{
	va_list args;
	va_start(args, format);
	brei_log_msg_va(brei, priority, file, lineno, func, format, args);
	va_end(args);
}

static struct brei_result *
brei_demarshal(struct brei_context *brei,
	       struct iobuf *buf,
	       const char *signature,
	       size_t *nargs_out,
	       union brei_arg **args_out,
	       char ***strings_out)
{
	size_t nargs = strlen(signature);
	if (nargs > 256) {
		return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
				       "Too many arguments in signature (%zu)",
				       nargs);
	}

	/* This over-allocates if we have more than one char per type but meh */
	_cleanup_free_ union brei_arg *args = xalloc(nargs * sizeof(*args));
	/* This over-allocates since not all args are strings but meh.
	   Needs to be NULL-terminated for strv_freep to work */
	_cleanup_(strv_freep) char **strings = xalloc((nargs + 1) * sizeof(*strings));

	const char *s = signature;
	union brei_arg *arg = args;
	uint32_t *p = (uint32_t *)iobuf_data(buf);
	uint32_t *end = (uint32_t *)iobuf_data_end(buf);
	size_t nstrings = 0;

	nargs = 0;
	while (*s) {
		uint32_t remaining = end - p;

		switch (*s) {
		case 'i':
		case 'u':
		case 'f':
			if (remaining < 1) {
				return brei_result_new(
					BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					"Message truncated, need 4 bytes but only %u remaining",
					remaining * 4);
			}
			arg->u = *p++;
			break;
		case 'x':
			if (remaining < 2) {
				return brei_result_new(
					BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					"Message truncated, need 8 bytes but only %u remaining",
					remaining * 4);
			}
			arg->x = *(int64_t *)p;
			p++;
			p++;
			break;
		case 'o':
		case 'n':
		case 't':
			if (remaining < 2) {
				return brei_result_new(
					BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					"Message truncated, need 8 bytes but only %u remaining",
					remaining * 4);
			}
			memcpy(&arg->x, p, sizeof(arg->x));
			p++;
			p++;
			break;
		case 'h':
			arg->h = iobuf_take_fd(buf);
			break;
		case 's': {
			if (remaining < 1) {
				return brei_result_new(
					BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					"Message truncated, need string length but 0 bytes remaining");
			}
			uint32_t slen = *p;
			uint32_t protolen = brei_string_proto_length(slen); /* in bytes */
			uint32_t len32 = protolen / 4; /* p and end are uint32_t* */

			if (remaining < len32) {
				return brei_result_new(
					BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					"Invalid string length %u, only %u bytes remaining",
					slen,
					remaining * 4);
			}

			if (slen == 0) {
				arg->s = NULL;
			} else {
				_cleanup_free_ char *str = xalloc(slen);
				memcpy(str, p + 1, slen);
				if (str[slen - 1] != '\0') {
					return brei_result_new(
						BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
						"Message string not zero-terminated");
				}
				strings[nstrings] = steal(&str);
				arg->s = strings[nstrings];
				nstrings++;
			}
			p += len32;
			break;
		}
		default:
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					       "Invalid signature '%c'",
					       *s);
		}
		arg++;
		s++;
		nargs++;
	}

	*args_out = steal(&args);
	*strings_out = steal(&strings);
	*nargs_out = nargs;

	return NULL;
}

static struct brei_result *
brei_marshal(struct brei_context *brei,
	     struct iobuf *buf,
	     const char *signature,
	     size_t nargs,
	     va_list args)
{
	const char *s = signature;
	int32_t i;
	uint32_t u;
	int64_t x;
	uint64_t t;
	float f;
	int fd;

	while (*s) {
		switch (*s) {
		case 'i':
			i = va_arg(args, int32_t);
			iobuf_append_u32(buf, i);
			break;
		case 'u':
			u = va_arg(args, uint32_t);
			iobuf_append_u32(buf, u);
			break;
		case 'x':
			x = va_arg(args, int64_t);
			iobuf_append_u64(buf, x);
			break;
		case 'o':
		case 'n':
		case 't':
			t = va_arg(args, uint64_t);
			iobuf_append_u64(buf, t);
			break;
		case 'f':
			f = va_arg(args, double);
			iobuf_append_f32(buf, f);
			break;
		case 'h':
			fd = va_arg(args, int);
			iobuf_append_fd(buf, fd);
			break;
		case 's': {
			static const char zeroes[4] = { 0 };
			const char *str = va_arg(args, const char *);

			/* FIXME: nullable strings */
			uint32_t slen = str ? strlen(str) + 1 : 0;
			iobuf_append_u32(buf, slen);
			if (slen > 0) {
				iobuf_append(buf, str, slen);
				if (slen % 4)
					iobuf_append(buf, zeroes, 4 - slen % 4);
			}
			break;
		}
		default:
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
					       "Invalid signature '%c'",
					       *s);
		}
		s++;
	}

	return NULL;
}

struct brei_result *
brei_marshal_message(struct brei_context *brei,
		     object_id_t id,
		     uint32_t opcode,
		     const char *signature,
		     size_t nargs,
		     va_list args)
{
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(128);

	_unref_(brei_result) *result = brei_marshal(brei, buf, signature, nargs, args);
	if (result)
		return steal(&result);

	size_t message_len = iobuf_len(buf) + sizeof(struct brei_header);
	uint32_t header[4] = { 0, 0, message_len, opcode };
	memcpy(header, &id, sizeof(id));
	iobuf_prepend(buf, header, sizeof(header));

	return brei_result_new_success(steal(&buf));
}

struct brei_result *
brei_dispatch(struct brei_context *brei,
	      int fd,
	      int (*lookup_object)(object_id_t object_id,
				   struct brei_object **object,
				   void *user_data),
	      void *user_data)
{
	_unref_(brei_result) *result = NULL;
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(64);
	int rc = iobuf_recv_from_fd(buf, fd);
	if (rc == -EAGAIN) {
		return NULL;
	} else if (rc == 0) {
		return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_TRANSPORT,
				       "socket disconnected");
	} else if (rc < 0) {
		return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_TRANSPORT,
				       "error: %s",
				       strerror(-rc));
	}

	while (true) {
		uint32_t *data = (uint32_t *)iobuf_data(buf);
		size_t len = iobuf_len(buf);

		const size_t headersize = sizeof(struct brei_header);

		if (len < headersize)
			break;

		const struct brei_header *header = (const struct brei_header *)data;
		uint32_t msglen = header->msglen;

		/* Max message size: 1MiB, plenty enough for the current protocol */
		static const uint32_t max_msglen = 1024 * 1024;
		if (msglen < headersize || msglen > max_msglen) {
			result = brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
						 "invalid message length %u",
						 msglen);
			goto error;
		}

		object_id_t object_id = header->sender_id;
		uint32_t opcode = header->opcode;
		;

		/* Find the object, it is stored in the ei/eis context  */
		struct brei_object *object = NULL;
		rc = lookup_object(header->sender_id, &object, user_data);
		if (rc == -ENOENT) {
			iobuf_pop(buf, msglen);
			continue;
		}
		if (rc < 0) {
			result = brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
						 "lookup_object failed with %d (%s)",
						 -rc,
						 strerror(-rc));
			goto error;
		}

		assert(object);

		/* We know the object's interface, now find the event we
		 * need to parse */
		const struct brei_interface *interface = object->interface;
		assert(interface);

		if (opcode >= interface->nincoming) {
			result = brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL,
						 "opcode %u exceeds interface %s method count %u",
						 header->opcode,
						 interface->name,
						 interface->nincoming);
			goto error;
		}

		iobuf_pop(buf, headersize);

		/* Demarshal the protocol into a set of arguments */
		_cleanup_free_ union brei_arg *args = NULL;
		_cleanup_(strv_freep) char **strings = NULL;
		const char *signature = interface->incoming[opcode].signature;
		size_t nargs = 0;
		result = brei_demarshal(brei, buf, signature, &nargs, &args, &strings);
		if (result)
			goto error;

		log_debug(brei,
			  "object %#" PRIx64 " dispatching %s.%s()",
			  object_id,
			  interface->name,
			  interface->incoming[opcode].name);

		/* Success! Let's pass this on to the
		 * context to process */
		result = interface->dispatcher(object->implementation, opcode, nargs, args);
		if (result)
			goto error;

		iobuf_pop(buf, msglen - headersize);
	}
error:
	if (result) {
		log_error(brei, "%s", brei_result_get_explanation(result));
		return steal(&result);
	}

	return NULL;
}

void
brei_drain_fd(int fd)
{
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(1024);
	int rc;

	while ((rc = iobuf_recv_from_fd(buf, fd)) > 0)
		;
}

#ifdef _enable_tests_
#include "util-munit.h"

MUNIT_TEST(test_brei_string_proto_length)
{
	munit_assert_int(brei_string_proto_length(0), ==, 4);
	munit_assert_int(brei_string_proto_length(1), ==, 8);
	munit_assert_int(brei_string_proto_length(4), ==, 8);
	munit_assert_int(brei_string_proto_length(5), ==, 12);
	munit_assert_int(brei_string_proto_length(8), ==, 12);
	munit_assert_int(brei_string_proto_length(12), ==, 16);
	munit_assert_int(brei_string_proto_length(13), ==, 20);

	return MUNIT_OK;
}

static struct brei_result *
brei_marshal_va(struct brei_context *brei,
		struct iobuf *buf,
		const char *signature,
		size_t nargs,
		...)
{
	va_list args;

	va_start(args, nargs);
	struct brei_result *result = brei_marshal(brei, buf, signature, nargs, args);
	va_end(args);
	return result;
}

MUNIT_TEST(test_brei_marshal)
{
	_unref_(brei_context) *brei = brei_context_new(NULL);
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(64);
	const char *str = "eierspeise";

	{
		_unref_(brei_result) *result = brei_marshal_va(brei,
							       buf,
							       "noiusf",
							       5,
							       (object_id_t)0xab,
							       (object_id_t)0xcd,
							       (int32_t)-13,
							       (uint32_t)0xfffd,
							       str,
							       1.45);
		munit_assert_ptr_null(result);
	}

	_cleanup_free_ union brei_arg *args = NULL;
	_cleanup_(strv_freep) char **strings = NULL;
	size_t nargs = 0;
	_unref_(brei_result) *result = brei_demarshal(brei, buf, "noiusf", &nargs, &args, &strings);
	munit_assert_ptr_null(result);
	munit_assert_int(nargs, ==, 6);

	munit_assert_int(args[0].o, ==, 0xab);
	munit_assert_int(args[1].o, ==, 0xcd);
	munit_assert_int(args[2].i, ==, -13);
	munit_assert_int(args[3].u, ==, 0xfffd);
	munit_assert_string_equal(args[4].s, str);
	munit_assert_double_equal(args[5].f, 1.45, 3 /* precision */);

	/* make sure strings is filled in as expected and null-terminated */
	munit_assert_ptr_equal(args[4].s, strings[0]);
	munit_assert_ptr_null(strings[1]);

	return MUNIT_OK;
}

MUNIT_TEST(test_brei_marshal_bad_sig)
{
	_unref_(brei_context) *brei = brei_context_new(NULL);
	_cleanup_iobuf_ struct iobuf *buf = iobuf_new(64);
	const char *str = "eierspeise";

	{
		_unref_(brei_result) *result = brei_marshal_va(brei,
							       buf,
							       "noiusf",
							       5,
							       (object_id_t)0xab,
							       (object_id_t)0xcd,
							       (int32_t)-13,
							       (uint32_t)0xfffd,
							       str,
							       1.45);
		munit_assert_ptr_null(result);
	}

	_cleanup_free_ union brei_arg *args = NULL;
	_cleanup_(strv_freep) char **strings = NULL;
	size_t nargs = 789;
	_unref_(brei_result) *result =
		brei_demarshal(brei, buf, "nxoiusf", &nargs, &args, &strings);
	munit_assert_ptr_not_null(result);
	munit_assert_int(brei_result_get_reason(result),
			 ==,
			 BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL);
	munit_assert_int(nargs, ==, 789); /* nargs must not be touched on error */

	return MUNIT_OK;
}

static int
brei_send_message(struct brei_context *brei,
		  int fd,
		  object_id_t id,
		  uint32_t opcode,
		  const char *signature,
		  size_t nargs,
		  va_list args)
{
	_unref_(brei_result) *result =
		brei_marshal_message(brei, id, opcode, signature, nargs, args);

	if (brei_result_get_reason(result) != 0)
		return -EBADMSG;

	_cleanup_iobuf_ struct iobuf *buf = NULL;
	buf = brei_result_get_data(result);
	return iobuf_send(buf, fd);
}

static int
brei_send_message_va(int fd,
		     object_id_t id,
		     uint32_t opcode,
		     const char *signature,
		     size_t nargs,
		     ...)
{
	_unref_(brei_context) *brei = brei_context_new(NULL);
	va_list args;
	va_start(args, nargs);
	int rc = brei_send_message(brei, fd, id, opcode, signature, nargs, args);
	va_end(args);
	return rc;
}

/**
 * Return the number of int32s required to store count bytes.
 */
static inline uint32_t
bytes_to_int32(uint32_t count)
{
	return (uint32_t)(((uint64_t)count + 3) / 4);
}

static inline void
buffer_debug(const void *buffer, size_t sz)
{
	const size_t stride = 8;
	_cleanup_(strv_freep) char **strv = strv_from_mem(buffer, sz, stride);

	munit_logf(MUNIT_LOG_DEBUG, "Logging buffer size %zu", sz);
	for (size_t offset = 0; offset < sz; offset += stride) {
		const char *s = strv[offset / stride];
		munit_assert_ptr_not_null(s);
		munit_logf(MUNIT_LOG_DEBUG, "%02zu: %s", offset, s);
	}
}

MUNIT_TEST(test_brei_send_message)
{
	int sv[2];
	int rc = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv);
	munit_assert_int(rc, ==, 0);

	_cleanup_close_ int sock_read = sv[0];
	_cleanup_close_ int sock_write = sv[1];

	const int header_size = 16;

	{
		const int msglen = header_size + 8; /* 2 * 4 bytes */
		object_id_t id = 1;
		uint32_t opcode = 2;
		const char *signature = "uu";
		int rc = brei_send_message_va(sock_write, id, opcode, signature, 2, 0xff, 0xdddd);
		munit_assert_int(rc, ==, msglen);

		uint32_t buf[64];
		int len = read(sock_read, buf, sizeof(buf));
		munit_assert_int(len, ==, msglen);

		buffer_debug(buf, len);

		const struct brei_header *header = (const struct brei_header *)buf;
		munit_assert_int(header->sender_id, ==, id);
		munit_assert_int(header->msglen, ==, msglen);
		munit_assert_int(header->opcode, ==, opcode);
		munit_assert_int(buf[4], ==, 0xff);
		munit_assert_int(buf[5], ==, 0xdddd);
	}
	{
		const int msglen = header_size + 8; /* 2 * 4 bytes */
		object_id_t id = 1;
		uint32_t opcode = 2;
		const char *signature = "fi";
		int rc = brei_send_message_va(sock_write, id, opcode, signature, 2, 1.234, -12);
		munit_assert_int(rc, ==, msglen);

		uint32_t buf[64];
		int len = read(sock_read, buf, sizeof(buf));
		union {
			uint32_t bytes;
			float f;
		} ufloat;
		munit_assert_int(len, ==, msglen);

		buffer_debug(buf, len);

		const struct brei_header *header = (const struct brei_header *)buf;
		munit_assert_int(header->sender_id, ==, id);
		munit_assert_int(header->msglen, ==, msglen);
		munit_assert_int(header->opcode, ==, opcode);

		ufloat.bytes = buf[4];
		munit_assert_double_equal(ufloat.f, 1.234, 4 /* precision */);
		munit_assert_int(buf[5], ==, -12);
	}

	{
		const char string[12] = "hello wor"; /* tests padding too */
		uint32_t string_len = bytes_to_int32(strlen0(string)) * 4;
		munit_assert_int(string_len, ==, sizeof(string));

		const int msglen =
			header_size + 24 + string_len; /* 4 bytes + 2 * 8 bytes + string length */
		object_id_t id = 2;
		uint32_t opcode = 3;
		const char *signature = "ison";
		int rc = brei_send_message_va(sock_write,
					      id,
					      opcode,
					      signature,
					      4,
					      (int32_t)-42,
					      string,
					      (object_id_t)0xab,
					      (object_id_t)0xcdef);
		munit_assert_int(rc, ==, msglen);

		uint32_t buf[64];
		int len = read(sock_read, buf, sizeof(buf));
		munit_assert_int(len, ==, msglen);

		buffer_debug(buf, len);

		const struct brei_header *header = (const struct brei_header *)buf;
		munit_assert_uint64(header->sender_id, ==, id);
		munit_assert_int(header->msglen, ==, msglen);
		munit_assert_int(header->opcode, ==, opcode);

		munit_assert_int(buf[4], ==, -42);

		uint32_t slen = buf[5];
		munit_assert_int(slen, ==, strlen0(string));
		char protostring[sizeof(string)] = { 0 };
		assert(brei_string_proto_length(slen) - 4 == sizeof(protostring));
		memcpy(protostring, &buf[6], brei_string_proto_length(slen) - 4);
		munit_assert_string_equal(protostring, string);
		munit_assert_int(memcmp(protostring, string, brei_string_proto_length(slen) - 4),
				 ==,
				 0);

		object_id_t a, b;
		memcpy(&a, &buf[6 + string_len / 4], sizeof(a));
		memcpy(&b, &buf[8 + string_len / 4], sizeof(b));
		munit_assert_uint64(a, ==, 0xab);
		munit_assert_uint64(b, ==, 0xcdef);
	}

	{
		const char string1[12] = "hello wor"; /* tests padding too */
		const char string2[8] = "barba";      /* tests padding too */

		const int msglen =
			header_size + 8 + sizeof(string1) +
			sizeof(string2); /* 8 header + 2 * 4 bytes slen + string lengths */
		object_id_t id = 2;
		uint32_t opcode = 3;
		const char *signature = "ss";
		int rc = brei_send_message_va(sock_write,
					      id,
					      opcode,
					      signature,
					      2,
					      string1,
					      string2);
		munit_assert_int(rc, ==, msglen);

		uint32_t buf[64];
		int len = read(sock_read, buf, sizeof(buf));
		munit_assert_int(len, ==, msglen);

		buffer_debug(buf, len);

		const struct brei_header *header = (const struct brei_header *)buf;
		munit_assert_uint64(header->sender_id, ==, id);
		munit_assert_int(header->msglen, ==, msglen);
		munit_assert_int(header->opcode, ==, opcode);

		uint32_t s1len = buf[4];
		munit_assert_int(s1len, ==, strlen0(string1));
		char protostring1[sizeof(string1)] = { 0 };
		assert(brei_string_proto_length(s1len) - 4 == sizeof(protostring1));
		memcpy(protostring1, &buf[5], brei_string_proto_length(s1len) - 4);
		munit_assert_string_equal(protostring1, string1);
		munit_assert_int(memcmp(protostring1, string1, brei_string_proto_length(s1len) - 4),
				 ==,
				 0);

		uint32_t s2len = buf[8];
		munit_assert_int(s2len, ==, strlen0(string2));
		char protostring2[sizeof(string2)] = { 0 };
		assert(brei_string_proto_length(s2len) - 4 == sizeof(protostring2));
		memcpy(protostring2, &buf[9], brei_string_proto_length(s2len) - 4);
		munit_assert_string_equal(protostring2, string2);
		munit_assert_int(memcmp(protostring2, string2, brei_string_proto_length(s2len) - 4),
				 ==,
				 0);
	}

	{
		int fds[2];
		int rc = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds);
		munit_assert_int(rc, ==, 0);
		_cleanup_close_ int left = fds[0];
		_cleanup_close_ int right = fds[1];

		/* actual message data to be sent */
		char data[] = "some data\n";

		const int msglen = header_size + 8; /* 2 unsigned, fd is not in data */
		object_id_t id = 2;
		uint32_t opcode = 3;
		const char *signature = "uhu";
		rc = brei_send_message_va(sock_write, id, opcode, signature, 3, 0xab, right, 0xcd);
		munit_assert_int(rc, ==, msglen);

		/* We passed it down, can close it now */
		close(right);
		right = -1;

		/* receive the brei message */
		_cleanup_iobuf_ struct iobuf *recv = iobuf_new(64);
		int len = iobuf_recv_from_fd(recv, sock_read);

		uint32_t *buf = (uint32_t *)iobuf_data(recv);
		munit_assert_int(len, ==, msglen);
		const struct brei_header *header = (const struct brei_header *)buf;
		munit_assert_uint64(header->sender_id, ==, id);
		munit_assert_int(header->msglen, ==, msglen);
		munit_assert_int(header->opcode, ==, opcode);

		munit_assert_int(buf[4], ==, 0xab);
		/* fd is not in data */
		munit_assert_int(buf[5], ==, 0xcd);

		_cleanup_close_ int fd = iobuf_take_fd(recv);
		munit_assert_int(fd, !=, -1);
		munit_assert_int(iobuf_take_fd(recv), ==, -1); /* only one fd */

		/* send some data down the dup'd fd */
		rc = xsend(fd, data, sizeof(data));
		munit_assert_int(rc, ==, sizeof(data));

		char recvbuf[64] = { 0 };
		rc = xread(left, recvbuf, sizeof(recvbuf));
		munit_assert_int(rc, ==, sizeof(data));
		munit_assert_string_equal(recvbuf, data);
	}

	return MUNIT_OK;
}

#endif
