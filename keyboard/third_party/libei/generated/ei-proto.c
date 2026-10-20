/**
 * GENERATED FILE, DO NOT EDIT
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include "brei-shared.h"
#include "brei-proto.h"

#include "ei-proto.h"

/**
 * Forward declarations. This file is intended to be compile-able without including
 * any of the actual implementation files.
 */
struct ei;

/**
 * The function that converts the given arguments into a wire format and sends it.
 * This function must be provided by the implementation, it is called by all
 * message sending functions (requests for libei, events for libeis).
 */
extern int ei_send_message(
  struct ei *ei, const struct brei_object *obj, uint32_t opcode,
  const char *signature, size_t nargs, ...
);

/***************************************************************************
 * Interface: ei_handshake                                                 *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_handshake_interface *ei_handshake_get_interface(struct ei_handshake * ei_handshake);

/* returns the message sending context from the given struct */
struct ei * ei_handshake_get_context(struct ei_handshake * ei_handshake);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_handshake_get_proto_object(struct ei_handshake * ei_handshake);

/* request opcodes */
#define EI_HANDSHAKE_REQUEST_HANDSHAKE_VERSION        0
#define EI_HANDSHAKE_REQUEST_FINISH                   1
#define EI_HANDSHAKE_REQUEST_CONTEXT_TYPE             2
#define EI_HANDSHAKE_REQUEST_NAME                     3
#define EI_HANDSHAKE_REQUEST_INTERFACE_VERSION        4

/* event opcodes */
#define EI_HANDSHAKE_EVENT_HANDSHAKE_VERSION          0
#define EI_HANDSHAKE_EVENT_INTERFACE_VERSION          1
#define EI_HANDSHAKE_EVENT_CONNECTION                 2

/* Message sender functions */
int
ei_handshake_request_handshake_version(struct ei_handshake * ei_handshake, uint32_t version)
{
	if (!ei_handshake)
		return -ENOENT;

	const struct brei_object *obj = ei_handshake_get_proto_object(ei_handshake);
	struct ei * ctx = ei_handshake_get_context(ei_handshake);

	if (obj->version < EI_HANDSHAKE_REQUEST_HANDSHAKE_VERSION_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_HANDSHAKE_REQUEST_HANDSHAKE_VERSION, "u",
		1, version);
}

int
ei_handshake_request_finish(struct ei_handshake * ei_handshake)
{
	if (!ei_handshake)
		return -ENOENT;

	const struct brei_object *obj = ei_handshake_get_proto_object(ei_handshake);
	struct ei * ctx = ei_handshake_get_context(ei_handshake);

	if (obj->version < EI_HANDSHAKE_REQUEST_FINISH_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_HANDSHAKE_REQUEST_FINISH, "",
		0);
}

int
ei_handshake_request_context_type(struct ei_handshake * ei_handshake, uint32_t context_type)
{
	if (!ei_handshake)
		return -ENOENT;

	const struct brei_object *obj = ei_handshake_get_proto_object(ei_handshake);
	struct ei * ctx = ei_handshake_get_context(ei_handshake);

	if (obj->version < EI_HANDSHAKE_REQUEST_CONTEXT_TYPE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_HANDSHAKE_REQUEST_CONTEXT_TYPE, "u",
		1, context_type);
}

int
ei_handshake_request_name(struct ei_handshake * ei_handshake, const char * name)
{
	if (!ei_handshake)
		return -ENOENT;

	const struct brei_object *obj = ei_handshake_get_proto_object(ei_handshake);
	struct ei * ctx = ei_handshake_get_context(ei_handshake);

	if (obj->version < EI_HANDSHAKE_REQUEST_NAME_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_HANDSHAKE_REQUEST_NAME, "s",
		1, name);
}

int
ei_handshake_request_interface_version(struct ei_handshake * ei_handshake, const char * name, uint32_t version)
{
	if (!ei_handshake)
		return -ENOENT;

	const struct brei_object *obj = ei_handshake_get_proto_object(ei_handshake);
	struct ei * ctx = ei_handshake_get_context(ei_handshake);

	if (obj->version < EI_HANDSHAKE_REQUEST_INTERFACE_VERSION_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_HANDSHAKE_REQUEST_INTERFACE_VERSION, "su",
		2, name, version);
}


/**
 * The dispatcher for the ei_handshake interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_handshake_dispatcher(
	struct ei_handshake * ei_handshake,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_handshake_interface *interface = ei_handshake_get_interface(ei_handshake);
	const struct brei_object *obj = ei_handshake_get_proto_object(ei_handshake);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_HANDSHAKE_EVENT_HANDSHAKE_VERSION:
		if (obj->version < EI_HANDSHAKE_EVENT_HANDSHAKE_VERSION_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->handshake_version != NULL);
		return interface->handshake_version(ei_handshake, (args + 0)->u);
	case EI_HANDSHAKE_EVENT_INTERFACE_VERSION:
		if (obj->version < EI_HANDSHAKE_EVENT_INTERFACE_VERSION_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->interface_version != NULL);
		return interface->interface_version(ei_handshake, (args + 0)->s, (args + 1)->u);
	case EI_HANDSHAKE_EVENT_CONNECTION:
		if (obj->version < EI_HANDSHAKE_EVENT_CONNECTION_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->connection != NULL);
		return interface->connection(ei_handshake, (args + 0)->u, (args + 1)->n, (args + 2)->u);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_handshake_requests[] = {
	{ "handshake_version", "u"  },
	{ "finish", ""  },
	{ "context_type", "u"  },
	{ "name", "s"  },
	{ "interface_version", "su"  },
};

static const struct brei_message ei_handshake_events[] = {
	{ "handshake_version", "u" },
	{ "interface_version", "su" },
	{ "connection", "unu" },
};

static const struct brei_message ei_handshake_incoming[] = {
	{ "handshake_version", "u" },
	{ "interface_version", "su" },
	{ "connection", "unu" },
};

const struct brei_interface ei_handshake_proto_interface = {
    .name = "ei_handshake", .version = 1,
    .nrequests = 5, .requests = ei_handshake_requests,
    .nevents = 3, .events = ei_handshake_events,
    .nincoming = 3, .incoming = ei_handshake_incoming,
    .dispatcher = (brei_event_dispatcher)ei_handshake_dispatcher,
};

/***************************************************************************
 * Interface: ei_connection                                                *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_connection_interface *ei_connection_get_interface(struct ei_connection * ei_connection);

/* returns the message sending context from the given struct */
struct ei * ei_connection_get_context(struct ei_connection * ei_connection);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_connection_get_proto_object(struct ei_connection * ei_connection);

/* request opcodes */
#define EI_CONNECTION_REQUEST_SYNC                    0
#define EI_CONNECTION_REQUEST_DISCONNECT              1

/* event opcodes */
#define EI_CONNECTION_EVENT_DISCONNECTED              0
#define EI_CONNECTION_EVENT_SEAT                      1
#define EI_CONNECTION_EVENT_INVALID_OBJECT            2
#define EI_CONNECTION_EVENT_PING                      3

/* Message sender functions */
int
ei_connection_request_sync(struct ei_connection * ei_connection, new_id_t callback, uint32_t version)
{
	if (!ei_connection)
		return -ENOENT;

	const struct brei_object *obj = ei_connection_get_proto_object(ei_connection);
	struct ei * ctx = ei_connection_get_context(ei_connection);

	if (obj->version < EI_CONNECTION_REQUEST_SYNC_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_CONNECTION_REQUEST_SYNC, "nu",
		2, callback, version);
}

int
ei_connection_request_disconnect(struct ei_connection * ei_connection)
{
	if (!ei_connection)
		return -ENOENT;

	const struct brei_object *obj = ei_connection_get_proto_object(ei_connection);
	struct ei * ctx = ei_connection_get_context(ei_connection);

	if (obj->version < EI_CONNECTION_REQUEST_DISCONNECT_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_CONNECTION_REQUEST_DISCONNECT, "",
		0);
}


/**
 * The dispatcher for the ei_connection interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_connection_dispatcher(
	struct ei_connection * ei_connection,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_connection_interface *interface = ei_connection_get_interface(ei_connection);
	const struct brei_object *obj = ei_connection_get_proto_object(ei_connection);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_CONNECTION_EVENT_DISCONNECTED:
		if (obj->version < EI_CONNECTION_EVENT_DISCONNECTED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->disconnected != NULL);
		return interface->disconnected(ei_connection, (args + 0)->u, (args + 1)->u, (args + 2)->s);
	case EI_CONNECTION_EVENT_SEAT:
		if (obj->version < EI_CONNECTION_EVENT_SEAT_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->seat != NULL);
		return interface->seat(ei_connection, (args + 0)->n, (args + 1)->u);
	case EI_CONNECTION_EVENT_INVALID_OBJECT:
		if (obj->version < EI_CONNECTION_EVENT_INVALID_OBJECT_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->invalid_object != NULL);
		return interface->invalid_object(ei_connection, (args + 0)->u, (args + 1)->t);
	case EI_CONNECTION_EVENT_PING:
		if (obj->version < EI_CONNECTION_EVENT_PING_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->ping != NULL);
		return interface->ping(ei_connection, (args + 0)->n, (args + 1)->u);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_connection_requests[] = {
	{ "sync", "nu"  },
	{ "disconnect", ""  },
};

static const struct brei_message ei_connection_events[] = {
	{ "disconnected", "uus" },
	{ "seat", "nu" },
	{ "invalid_object", "ut" },
	{ "ping", "nu" },
};

static const struct brei_message ei_connection_incoming[] = {
	{ "disconnected", "uus" },
	{ "seat", "nu" },
	{ "invalid_object", "ut" },
	{ "ping", "nu" },
};

const struct brei_interface ei_connection_proto_interface = {
    .name = "ei_connection", .version = 1,
    .nrequests = 2, .requests = ei_connection_requests,
    .nevents = 4, .events = ei_connection_events,
    .nincoming = 4, .incoming = ei_connection_incoming,
    .dispatcher = (brei_event_dispatcher)ei_connection_dispatcher,
};

/***************************************************************************
 * Interface: ei_callback                                                  *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_callback_interface *ei_callback_get_interface(struct ei_callback * ei_callback);

/* returns the message sending context from the given struct */
struct ei * ei_callback_get_context(struct ei_callback * ei_callback);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_callback_get_proto_object(struct ei_callback * ei_callback);

/* request opcodes */

/* event opcodes */
#define EI_CALLBACK_EVENT_DONE                        0

/* Message sender functions */

/**
 * The dispatcher for the ei_callback interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_callback_dispatcher(
	struct ei_callback * ei_callback,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_callback_interface *interface = ei_callback_get_interface(ei_callback);
	const struct brei_object *obj = ei_callback_get_proto_object(ei_callback);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_CALLBACK_EVENT_DONE:
		if (obj->version < EI_CALLBACK_EVENT_DONE_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->done != NULL);
		return interface->done(ei_callback, (args + 0)->t);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_callback_requests[] = {
};

static const struct brei_message ei_callback_events[] = {
	{ "done", "t" },
};

static const struct brei_message ei_callback_incoming[] = {
	{ "done", "t" },
};

const struct brei_interface ei_callback_proto_interface = {
    .name = "ei_callback", .version = 1,
    .nrequests = 0, .requests = ei_callback_requests,
    .nevents = 1, .events = ei_callback_events,
    .nincoming = 1, .incoming = ei_callback_incoming,
    .dispatcher = (brei_event_dispatcher)ei_callback_dispatcher,
};

/***************************************************************************
 * Interface: ei_pingpong                                                  *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_pingpong_interface *ei_pingpong_get_interface(struct ei_pingpong * ei_pingpong);

/* returns the message sending context from the given struct */
struct ei * ei_pingpong_get_context(struct ei_pingpong * ei_pingpong);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_pingpong_get_proto_object(struct ei_pingpong * ei_pingpong);

/* request opcodes */
#define EI_PINGPONG_REQUEST_DONE                      0

/* event opcodes */

/* Message sender functions */
int
ei_pingpong_request_done(struct ei_pingpong * ei_pingpong, uint64_t callback_data)
{
	if (!ei_pingpong)
		return -ENOENT;

	const struct brei_object *obj = ei_pingpong_get_proto_object(ei_pingpong);
	struct ei * ctx = ei_pingpong_get_context(ei_pingpong);

	if (obj->version < EI_PINGPONG_REQUEST_DONE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_PINGPONG_REQUEST_DONE, "t",
		1, callback_data);
}


/**
 * The dispatcher for the ei_pingpong interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_pingpong_dispatcher(
	struct ei_pingpong * ei_pingpong,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_pingpong_requests[] = {
	{ "done", "t"  },
};

static const struct brei_message ei_pingpong_events[] = {
};

static const struct brei_message ei_pingpong_incoming[] = {
};

const struct brei_interface ei_pingpong_proto_interface = {
    .name = "ei_pingpong", .version = 1,
    .nrequests = 1, .requests = ei_pingpong_requests,
    .nevents = 0, .events = ei_pingpong_events,
    .nincoming = 0, .incoming = ei_pingpong_incoming,
    .dispatcher = (brei_event_dispatcher)ei_pingpong_dispatcher,
};

/***************************************************************************
 * Interface: ei_seat                                                      *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_seat_interface *ei_seat_get_interface(struct ei_seat * ei_seat);

/* returns the message sending context from the given struct */
struct ei * ei_seat_get_context(struct ei_seat * ei_seat);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_seat_get_proto_object(struct ei_seat * ei_seat);

/* request opcodes */
#define EI_SEAT_REQUEST_RELEASE                       0
#define EI_SEAT_REQUEST_BIND                          1
#define EI_SEAT_REQUEST_REQUEST_DEVICE                2

/* event opcodes */
#define EI_SEAT_EVENT_DESTROYED                       0
#define EI_SEAT_EVENT_NAME                            1
#define EI_SEAT_EVENT_CAPABILITY                      2
#define EI_SEAT_EVENT_DONE                            3
#define EI_SEAT_EVENT_DEVICE                          4

/* Message sender functions */
int
ei_seat_request_release(struct ei_seat * ei_seat)
{
	if (!ei_seat)
		return -ENOENT;

	const struct brei_object *obj = ei_seat_get_proto_object(ei_seat);
	struct ei * ctx = ei_seat_get_context(ei_seat);

	if (obj->version < EI_SEAT_REQUEST_RELEASE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_SEAT_REQUEST_RELEASE, "",
		0);
}

int
ei_seat_request_bind(struct ei_seat * ei_seat, uint64_t capabilities)
{
	if (!ei_seat)
		return -ENOENT;

	const struct brei_object *obj = ei_seat_get_proto_object(ei_seat);
	struct ei * ctx = ei_seat_get_context(ei_seat);

	if (obj->version < EI_SEAT_REQUEST_BIND_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_SEAT_REQUEST_BIND, "t",
		1, capabilities);
}

int
ei_seat_request_request_device(struct ei_seat * ei_seat, uint64_t capabilities)
{
	if (!ei_seat)
		return -ENOENT;

	const struct brei_object *obj = ei_seat_get_proto_object(ei_seat);
	struct ei * ctx = ei_seat_get_context(ei_seat);

	if (obj->version < EI_SEAT_REQUEST_REQUEST_DEVICE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_SEAT_REQUEST_REQUEST_DEVICE, "t",
		1, capabilities);
}


/**
 * The dispatcher for the ei_seat interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_seat_dispatcher(
	struct ei_seat * ei_seat,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_seat_interface *interface = ei_seat_get_interface(ei_seat);
	const struct brei_object *obj = ei_seat_get_proto_object(ei_seat);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_SEAT_EVENT_DESTROYED:
		if (obj->version < EI_SEAT_EVENT_DESTROYED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->destroyed != NULL);
		return interface->destroyed(ei_seat, (args + 0)->u);
	case EI_SEAT_EVENT_NAME:
		if (obj->version < EI_SEAT_EVENT_NAME_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->name != NULL);
		return interface->name(ei_seat, (args + 0)->s);
	case EI_SEAT_EVENT_CAPABILITY:
		if (obj->version < EI_SEAT_EVENT_CAPABILITY_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->capability != NULL);
		return interface->capability(ei_seat, (args + 0)->t, (args + 1)->s);
	case EI_SEAT_EVENT_DONE:
		if (obj->version < EI_SEAT_EVENT_DONE_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->done != NULL);
		return interface->done(ei_seat);
	case EI_SEAT_EVENT_DEVICE:
		if (obj->version < EI_SEAT_EVENT_DEVICE_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->device != NULL);
		return interface->device(ei_seat, (args + 0)->n, (args + 1)->u);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_seat_requests[] = {
	{ "release", ""  },
	{ "bind", "t"  },
	{ "request_device", "t"  },
};

static const struct brei_message ei_seat_events[] = {
	{ "destroyed", "u" },
	{ "name", "s" },
	{ "capability", "ts" },
	{ "done", "" },
	{ "device", "nu" },
};

static const struct brei_message ei_seat_incoming[] = {
	{ "destroyed", "u" },
	{ "name", "s" },
	{ "capability", "ts" },
	{ "done", "" },
	{ "device", "nu" },
};

const struct brei_interface ei_seat_proto_interface = {
    .name = "ei_seat", .version = 2,
    .nrequests = 3, .requests = ei_seat_requests,
    .nevents = 5, .events = ei_seat_events,
    .nincoming = 5, .incoming = ei_seat_incoming,
    .dispatcher = (brei_event_dispatcher)ei_seat_dispatcher,
};

/***************************************************************************
 * Interface: ei_device                                                    *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_device_interface *ei_device_get_interface(struct ei_device * ei_device);

/* returns the message sending context from the given struct */
struct ei * ei_device_get_context(struct ei_device * ei_device);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_device_get_proto_object(struct ei_device * ei_device);

/* request opcodes */
#define EI_DEVICE_REQUEST_RELEASE                     0
#define EI_DEVICE_REQUEST_START_EMULATING             1
#define EI_DEVICE_REQUEST_STOP_EMULATING              2
#define EI_DEVICE_REQUEST_FRAME                       3
#define EI_DEVICE_REQUEST_READY                       4

/* event opcodes */
#define EI_DEVICE_EVENT_DESTROYED                     0
#define EI_DEVICE_EVENT_NAME                          1
#define EI_DEVICE_EVENT_DEVICE_TYPE                   2
#define EI_DEVICE_EVENT_DIMENSIONS                    3
#define EI_DEVICE_EVENT_REGION                        4
#define EI_DEVICE_EVENT_INTERFACE                     5
#define EI_DEVICE_EVENT_DONE                          6
#define EI_DEVICE_EVENT_RESUMED                       7
#define EI_DEVICE_EVENT_PAUSED                        8
#define EI_DEVICE_EVENT_START_EMULATING               9
#define EI_DEVICE_EVENT_STOP_EMULATING                10
#define EI_DEVICE_EVENT_FRAME                         11
#define EI_DEVICE_EVENT_REGION_MAPPING_ID             12

/* Message sender functions */
int
ei_device_request_release(struct ei_device * ei_device)
{
	if (!ei_device)
		return -ENOENT;

	const struct brei_object *obj = ei_device_get_proto_object(ei_device);
	struct ei * ctx = ei_device_get_context(ei_device);

	if (obj->version < EI_DEVICE_REQUEST_RELEASE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_DEVICE_REQUEST_RELEASE, "",
		0);
}

int
ei_device_request_start_emulating(struct ei_device * ei_device, uint32_t last_serial, uint32_t sequence)
{
	if (!ei_device)
		return -ENOENT;

	const struct brei_object *obj = ei_device_get_proto_object(ei_device);
	struct ei * ctx = ei_device_get_context(ei_device);

	if (obj->version < EI_DEVICE_REQUEST_START_EMULATING_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_DEVICE_REQUEST_START_EMULATING, "uu",
		2, last_serial, sequence);
}

int
ei_device_request_stop_emulating(struct ei_device * ei_device, uint32_t last_serial)
{
	if (!ei_device)
		return -ENOENT;

	const struct brei_object *obj = ei_device_get_proto_object(ei_device);
	struct ei * ctx = ei_device_get_context(ei_device);

	if (obj->version < EI_DEVICE_REQUEST_STOP_EMULATING_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_DEVICE_REQUEST_STOP_EMULATING, "u",
		1, last_serial);
}

int
ei_device_request_frame(struct ei_device * ei_device, uint32_t last_serial, uint64_t timestamp)
{
	if (!ei_device)
		return -ENOENT;

	const struct brei_object *obj = ei_device_get_proto_object(ei_device);
	struct ei * ctx = ei_device_get_context(ei_device);

	if (obj->version < EI_DEVICE_REQUEST_FRAME_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_DEVICE_REQUEST_FRAME, "ut",
		2, last_serial, timestamp);
}

int
ei_device_request_ready(struct ei_device * ei_device)
{
	if (!ei_device)
		return -ENOENT;

	const struct brei_object *obj = ei_device_get_proto_object(ei_device);
	struct ei * ctx = ei_device_get_context(ei_device);

	if (obj->version < EI_DEVICE_REQUEST_READY_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_DEVICE_REQUEST_READY, "",
		0);
}


/**
 * The dispatcher for the ei_device interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_device_dispatcher(
	struct ei_device * ei_device,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_device_interface *interface = ei_device_get_interface(ei_device);
	const struct brei_object *obj = ei_device_get_proto_object(ei_device);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_DEVICE_EVENT_DESTROYED:
		if (obj->version < EI_DEVICE_EVENT_DESTROYED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->destroyed != NULL);
		return interface->destroyed(ei_device, (args + 0)->u);
	case EI_DEVICE_EVENT_NAME:
		if (obj->version < EI_DEVICE_EVENT_NAME_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->name != NULL);
		return interface->name(ei_device, (args + 0)->s);
	case EI_DEVICE_EVENT_DEVICE_TYPE:
		if (obj->version < EI_DEVICE_EVENT_DEVICE_TYPE_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->device_type != NULL);
		return interface->device_type(ei_device, (args + 0)->u);
	case EI_DEVICE_EVENT_DIMENSIONS:
		if (obj->version < EI_DEVICE_EVENT_DIMENSIONS_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->dimensions != NULL);
		return interface->dimensions(ei_device, (args + 0)->u, (args + 1)->u);
	case EI_DEVICE_EVENT_REGION:
		if (obj->version < EI_DEVICE_EVENT_REGION_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->region != NULL);
		return interface->region(ei_device, (args + 0)->u, (args + 1)->u, (args + 2)->u, (args + 3)->u, (args + 4)->f);
	case EI_DEVICE_EVENT_INTERFACE:
		if (obj->version < EI_DEVICE_EVENT_INTERFACE_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->interface != NULL);
		return interface->interface(ei_device, (args + 0)->n, (args + 1)->s, (args + 2)->u);
	case EI_DEVICE_EVENT_DONE:
		if (obj->version < EI_DEVICE_EVENT_DONE_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->done != NULL);
		return interface->done(ei_device);
	case EI_DEVICE_EVENT_RESUMED:
		if (obj->version < EI_DEVICE_EVENT_RESUMED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->resumed != NULL);
		return interface->resumed(ei_device, (args + 0)->u);
	case EI_DEVICE_EVENT_PAUSED:
		if (obj->version < EI_DEVICE_EVENT_PAUSED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->paused != NULL);
		return interface->paused(ei_device, (args + 0)->u);
	case EI_DEVICE_EVENT_START_EMULATING:
		if (obj->version < EI_DEVICE_EVENT_START_EMULATING_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->start_emulating != NULL);
		return interface->start_emulating(ei_device, (args + 0)->u, (args + 1)->u);
	case EI_DEVICE_EVENT_STOP_EMULATING:
		if (obj->version < EI_DEVICE_EVENT_STOP_EMULATING_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->stop_emulating != NULL);
		return interface->stop_emulating(ei_device, (args + 0)->u);
	case EI_DEVICE_EVENT_FRAME:
		if (obj->version < EI_DEVICE_EVENT_FRAME_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->frame != NULL);
		return interface->frame(ei_device, (args + 0)->u, (args + 1)->t);
	case EI_DEVICE_EVENT_REGION_MAPPING_ID:
		if (obj->version < EI_DEVICE_EVENT_REGION_MAPPING_ID_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->region_mapping_id != NULL);
		return interface->region_mapping_id(ei_device, (args + 0)->s);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_device_requests[] = {
	{ "release", ""  },
	{ "start_emulating", "uu"  },
	{ "stop_emulating", "u"  },
	{ "frame", "ut"  },
	{ "ready", ""  },
};

static const struct brei_message ei_device_events[] = {
	{ "destroyed", "u" },
	{ "name", "s" },
	{ "device_type", "u" },
	{ "dimensions", "uu" },
	{ "region", "uuuuf" },
	{ "interface", "nsu" },
	{ "done", "" },
	{ "resumed", "u" },
	{ "paused", "u" },
	{ "start_emulating", "uu" },
	{ "stop_emulating", "u" },
	{ "frame", "ut" },
	{ "region_mapping_id", "s" },
};

static const struct brei_message ei_device_incoming[] = {
	{ "destroyed", "u" },
	{ "name", "s" },
	{ "device_type", "u" },
	{ "dimensions", "uu" },
	{ "region", "uuuuf" },
	{ "interface", "nsu" },
	{ "done", "" },
	{ "resumed", "u" },
	{ "paused", "u" },
	{ "start_emulating", "uu" },
	{ "stop_emulating", "u" },
	{ "frame", "ut" },
	{ "region_mapping_id", "s" },
};

const struct brei_interface ei_device_proto_interface = {
    .name = "ei_device", .version = 3,
    .nrequests = 5, .requests = ei_device_requests,
    .nevents = 13, .events = ei_device_events,
    .nincoming = 13, .incoming = ei_device_incoming,
    .dispatcher = (brei_event_dispatcher)ei_device_dispatcher,
};

/***************************************************************************
 * Interface: ei_pointer                                                   *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_pointer_interface *ei_pointer_get_interface(struct ei_pointer * ei_pointer);

/* returns the message sending context from the given struct */
struct ei * ei_pointer_get_context(struct ei_pointer * ei_pointer);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_pointer_get_proto_object(struct ei_pointer * ei_pointer);

/* request opcodes */
#define EI_POINTER_REQUEST_RELEASE                    0
#define EI_POINTER_REQUEST_MOTION_RELATIVE            1

/* event opcodes */
#define EI_POINTER_EVENT_DESTROYED                    0
#define EI_POINTER_EVENT_MOTION_RELATIVE              1

/* Message sender functions */
int
ei_pointer_request_release(struct ei_pointer * ei_pointer)
{
	if (!ei_pointer)
		return -ENOENT;

	const struct brei_object *obj = ei_pointer_get_proto_object(ei_pointer);
	struct ei * ctx = ei_pointer_get_context(ei_pointer);

	if (obj->version < EI_POINTER_REQUEST_RELEASE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_POINTER_REQUEST_RELEASE, "",
		0);
}

int
ei_pointer_request_motion_relative(struct ei_pointer * ei_pointer, float x, float y)
{
	if (!ei_pointer)
		return -ENOENT;

	const struct brei_object *obj = ei_pointer_get_proto_object(ei_pointer);
	struct ei * ctx = ei_pointer_get_context(ei_pointer);

	if (obj->version < EI_POINTER_REQUEST_MOTION_RELATIVE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_POINTER_REQUEST_MOTION_RELATIVE, "ff",
		2, x, y);
}


/**
 * The dispatcher for the ei_pointer interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_pointer_dispatcher(
	struct ei_pointer * ei_pointer,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_pointer_interface *interface = ei_pointer_get_interface(ei_pointer);
	const struct brei_object *obj = ei_pointer_get_proto_object(ei_pointer);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_POINTER_EVENT_DESTROYED:
		if (obj->version < EI_POINTER_EVENT_DESTROYED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->destroyed != NULL);
		return interface->destroyed(ei_pointer, (args + 0)->u);
	case EI_POINTER_EVENT_MOTION_RELATIVE:
		if (obj->version < EI_POINTER_EVENT_MOTION_RELATIVE_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->motion_relative != NULL);
		return interface->motion_relative(ei_pointer, (args + 0)->f, (args + 1)->f);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_pointer_requests[] = {
	{ "release", ""  },
	{ "motion_relative", "ff"  },
};

static const struct brei_message ei_pointer_events[] = {
	{ "destroyed", "u" },
	{ "motion_relative", "ff" },
};

static const struct brei_message ei_pointer_incoming[] = {
	{ "destroyed", "u" },
	{ "motion_relative", "ff" },
};

const struct brei_interface ei_pointer_proto_interface = {
    .name = "ei_pointer", .version = 1,
    .nrequests = 2, .requests = ei_pointer_requests,
    .nevents = 2, .events = ei_pointer_events,
    .nincoming = 2, .incoming = ei_pointer_incoming,
    .dispatcher = (brei_event_dispatcher)ei_pointer_dispatcher,
};

/***************************************************************************
 * Interface: ei_pointer_absolute                                          *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_pointer_absolute_interface *ei_pointer_absolute_get_interface(struct ei_pointer_absolute * ei_pointer_absolute);

/* returns the message sending context from the given struct */
struct ei * ei_pointer_absolute_get_context(struct ei_pointer_absolute * ei_pointer_absolute);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_pointer_absolute_get_proto_object(struct ei_pointer_absolute * ei_pointer_absolute);

/* request opcodes */
#define EI_POINTER_ABSOLUTE_REQUEST_RELEASE           0
#define EI_POINTER_ABSOLUTE_REQUEST_MOTION_ABSOLUTE   1

/* event opcodes */
#define EI_POINTER_ABSOLUTE_EVENT_DESTROYED           0
#define EI_POINTER_ABSOLUTE_EVENT_MOTION_ABSOLUTE     1

/* Message sender functions */
int
ei_pointer_absolute_request_release(struct ei_pointer_absolute * ei_pointer_absolute)
{
	if (!ei_pointer_absolute)
		return -ENOENT;

	const struct brei_object *obj = ei_pointer_absolute_get_proto_object(ei_pointer_absolute);
	struct ei * ctx = ei_pointer_absolute_get_context(ei_pointer_absolute);

	if (obj->version < EI_POINTER_ABSOLUTE_REQUEST_RELEASE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_POINTER_ABSOLUTE_REQUEST_RELEASE, "",
		0);
}

int
ei_pointer_absolute_request_motion_absolute(struct ei_pointer_absolute * ei_pointer_absolute, float x, float y)
{
	if (!ei_pointer_absolute)
		return -ENOENT;

	const struct brei_object *obj = ei_pointer_absolute_get_proto_object(ei_pointer_absolute);
	struct ei * ctx = ei_pointer_absolute_get_context(ei_pointer_absolute);

	if (obj->version < EI_POINTER_ABSOLUTE_REQUEST_MOTION_ABSOLUTE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_POINTER_ABSOLUTE_REQUEST_MOTION_ABSOLUTE, "ff",
		2, x, y);
}


/**
 * The dispatcher for the ei_pointer_absolute interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_pointer_absolute_dispatcher(
	struct ei_pointer_absolute * ei_pointer_absolute,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_pointer_absolute_interface *interface = ei_pointer_absolute_get_interface(ei_pointer_absolute);
	const struct brei_object *obj = ei_pointer_absolute_get_proto_object(ei_pointer_absolute);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_POINTER_ABSOLUTE_EVENT_DESTROYED:
		if (obj->version < EI_POINTER_ABSOLUTE_EVENT_DESTROYED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->destroyed != NULL);
		return interface->destroyed(ei_pointer_absolute, (args + 0)->u);
	case EI_POINTER_ABSOLUTE_EVENT_MOTION_ABSOLUTE:
		if (obj->version < EI_POINTER_ABSOLUTE_EVENT_MOTION_ABSOLUTE_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->motion_absolute != NULL);
		return interface->motion_absolute(ei_pointer_absolute, (args + 0)->f, (args + 1)->f);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_pointer_absolute_requests[] = {
	{ "release", ""  },
	{ "motion_absolute", "ff"  },
};

static const struct brei_message ei_pointer_absolute_events[] = {
	{ "destroyed", "u" },
	{ "motion_absolute", "ff" },
};

static const struct brei_message ei_pointer_absolute_incoming[] = {
	{ "destroyed", "u" },
	{ "motion_absolute", "ff" },
};

const struct brei_interface ei_pointer_absolute_proto_interface = {
    .name = "ei_pointer_absolute", .version = 1,
    .nrequests = 2, .requests = ei_pointer_absolute_requests,
    .nevents = 2, .events = ei_pointer_absolute_events,
    .nincoming = 2, .incoming = ei_pointer_absolute_incoming,
    .dispatcher = (brei_event_dispatcher)ei_pointer_absolute_dispatcher,
};

/***************************************************************************
 * Interface: ei_scroll                                                    *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_scroll_interface *ei_scroll_get_interface(struct ei_scroll * ei_scroll);

/* returns the message sending context from the given struct */
struct ei * ei_scroll_get_context(struct ei_scroll * ei_scroll);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_scroll_get_proto_object(struct ei_scroll * ei_scroll);

/* request opcodes */
#define EI_SCROLL_REQUEST_RELEASE                     0
#define EI_SCROLL_REQUEST_SCROLL                      1
#define EI_SCROLL_REQUEST_SCROLL_DISCRETE             2
#define EI_SCROLL_REQUEST_SCROLL_STOP                 3

/* event opcodes */
#define EI_SCROLL_EVENT_DESTROYED                     0
#define EI_SCROLL_EVENT_SCROLL                        1
#define EI_SCROLL_EVENT_SCROLL_DISCRETE               2
#define EI_SCROLL_EVENT_SCROLL_STOP                   3

/* Message sender functions */
int
ei_scroll_request_release(struct ei_scroll * ei_scroll)
{
	if (!ei_scroll)
		return -ENOENT;

	const struct brei_object *obj = ei_scroll_get_proto_object(ei_scroll);
	struct ei * ctx = ei_scroll_get_context(ei_scroll);

	if (obj->version < EI_SCROLL_REQUEST_RELEASE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_SCROLL_REQUEST_RELEASE, "",
		0);
}

int
ei_scroll_request_scroll(struct ei_scroll * ei_scroll, float x, float y)
{
	if (!ei_scroll)
		return -ENOENT;

	const struct brei_object *obj = ei_scroll_get_proto_object(ei_scroll);
	struct ei * ctx = ei_scroll_get_context(ei_scroll);

	if (obj->version < EI_SCROLL_REQUEST_SCROLL_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_SCROLL_REQUEST_SCROLL, "ff",
		2, x, y);
}

int
ei_scroll_request_scroll_discrete(struct ei_scroll * ei_scroll, int32_t x, int32_t y)
{
	if (!ei_scroll)
		return -ENOENT;

	const struct brei_object *obj = ei_scroll_get_proto_object(ei_scroll);
	struct ei * ctx = ei_scroll_get_context(ei_scroll);

	if (obj->version < EI_SCROLL_REQUEST_SCROLL_DISCRETE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_SCROLL_REQUEST_SCROLL_DISCRETE, "ii",
		2, x, y);
}

int
ei_scroll_request_scroll_stop(struct ei_scroll * ei_scroll, uint32_t x, uint32_t y, uint32_t is_cancel)
{
	if (!ei_scroll)
		return -ENOENT;

	const struct brei_object *obj = ei_scroll_get_proto_object(ei_scroll);
	struct ei * ctx = ei_scroll_get_context(ei_scroll);

	if (obj->version < EI_SCROLL_REQUEST_SCROLL_STOP_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_SCROLL_REQUEST_SCROLL_STOP, "uuu",
		3, x, y, is_cancel);
}


/**
 * The dispatcher for the ei_scroll interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_scroll_dispatcher(
	struct ei_scroll * ei_scroll,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_scroll_interface *interface = ei_scroll_get_interface(ei_scroll);
	const struct brei_object *obj = ei_scroll_get_proto_object(ei_scroll);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_SCROLL_EVENT_DESTROYED:
		if (obj->version < EI_SCROLL_EVENT_DESTROYED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->destroyed != NULL);
		return interface->destroyed(ei_scroll, (args + 0)->u);
	case EI_SCROLL_EVENT_SCROLL:
		if (obj->version < EI_SCROLL_EVENT_SCROLL_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->scroll != NULL);
		return interface->scroll(ei_scroll, (args + 0)->f, (args + 1)->f);
	case EI_SCROLL_EVENT_SCROLL_DISCRETE:
		if (obj->version < EI_SCROLL_EVENT_SCROLL_DISCRETE_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->scroll_discrete != NULL);
		return interface->scroll_discrete(ei_scroll, (args + 0)->i, (args + 1)->i);
	case EI_SCROLL_EVENT_SCROLL_STOP:
		if (obj->version < EI_SCROLL_EVENT_SCROLL_STOP_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->scroll_stop != NULL);
		return interface->scroll_stop(ei_scroll, (args + 0)->u, (args + 1)->u, (args + 2)->u);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_scroll_requests[] = {
	{ "release", ""  },
	{ "scroll", "ff"  },
	{ "scroll_discrete", "ii"  },
	{ "scroll_stop", "uuu"  },
};

static const struct brei_message ei_scroll_events[] = {
	{ "destroyed", "u" },
	{ "scroll", "ff" },
	{ "scroll_discrete", "ii" },
	{ "scroll_stop", "uuu" },
};

static const struct brei_message ei_scroll_incoming[] = {
	{ "destroyed", "u" },
	{ "scroll", "ff" },
	{ "scroll_discrete", "ii" },
	{ "scroll_stop", "uuu" },
};

const struct brei_interface ei_scroll_proto_interface = {
    .name = "ei_scroll", .version = 1,
    .nrequests = 4, .requests = ei_scroll_requests,
    .nevents = 4, .events = ei_scroll_events,
    .nincoming = 4, .incoming = ei_scroll_incoming,
    .dispatcher = (brei_event_dispatcher)ei_scroll_dispatcher,
};

/***************************************************************************
 * Interface: ei_button                                                    *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_button_interface *ei_button_get_interface(struct ei_button * ei_button);

/* returns the message sending context from the given struct */
struct ei * ei_button_get_context(struct ei_button * ei_button);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_button_get_proto_object(struct ei_button * ei_button);

/* request opcodes */
#define EI_BUTTON_REQUEST_RELEASE                     0
#define EI_BUTTON_REQUEST_BUTTON                      1

/* event opcodes */
#define EI_BUTTON_EVENT_DESTROYED                     0
#define EI_BUTTON_EVENT_BUTTON                        1

/* Message sender functions */
int
ei_button_request_release(struct ei_button * ei_button)
{
	if (!ei_button)
		return -ENOENT;

	const struct brei_object *obj = ei_button_get_proto_object(ei_button);
	struct ei * ctx = ei_button_get_context(ei_button);

	if (obj->version < EI_BUTTON_REQUEST_RELEASE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_BUTTON_REQUEST_RELEASE, "",
		0);
}

int
ei_button_request_button(struct ei_button * ei_button, uint32_t button, uint32_t state)
{
	if (!ei_button)
		return -ENOENT;

	const struct brei_object *obj = ei_button_get_proto_object(ei_button);
	struct ei * ctx = ei_button_get_context(ei_button);

	if (obj->version < EI_BUTTON_REQUEST_BUTTON_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_BUTTON_REQUEST_BUTTON, "uu",
		2, button, state);
}


/**
 * The dispatcher for the ei_button interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_button_dispatcher(
	struct ei_button * ei_button,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_button_interface *interface = ei_button_get_interface(ei_button);
	const struct brei_object *obj = ei_button_get_proto_object(ei_button);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_BUTTON_EVENT_DESTROYED:
		if (obj->version < EI_BUTTON_EVENT_DESTROYED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->destroyed != NULL);
		return interface->destroyed(ei_button, (args + 0)->u);
	case EI_BUTTON_EVENT_BUTTON:
		if (obj->version < EI_BUTTON_EVENT_BUTTON_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->button != NULL);
		return interface->button(ei_button, (args + 0)->u, (args + 1)->u);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_button_requests[] = {
	{ "release", ""  },
	{ "button", "uu"  },
};

static const struct brei_message ei_button_events[] = {
	{ "destroyed", "u" },
	{ "button", "uu" },
};

static const struct brei_message ei_button_incoming[] = {
	{ "destroyed", "u" },
	{ "button", "uu" },
};

const struct brei_interface ei_button_proto_interface = {
    .name = "ei_button", .version = 1,
    .nrequests = 2, .requests = ei_button_requests,
    .nevents = 2, .events = ei_button_events,
    .nincoming = 2, .incoming = ei_button_incoming,
    .dispatcher = (brei_event_dispatcher)ei_button_dispatcher,
};

/***************************************************************************
 * Interface: ei_keyboard                                                  *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_keyboard_interface *ei_keyboard_get_interface(struct ei_keyboard * ei_keyboard);

/* returns the message sending context from the given struct */
struct ei * ei_keyboard_get_context(struct ei_keyboard * ei_keyboard);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_keyboard_get_proto_object(struct ei_keyboard * ei_keyboard);

/* request opcodes */
#define EI_KEYBOARD_REQUEST_RELEASE                   0
#define EI_KEYBOARD_REQUEST_KEY                       1

/* event opcodes */
#define EI_KEYBOARD_EVENT_DESTROYED                   0
#define EI_KEYBOARD_EVENT_KEYMAP                      1
#define EI_KEYBOARD_EVENT_KEY                         2
#define EI_KEYBOARD_EVENT_MODIFIERS                   3

/* Message sender functions */
int
ei_keyboard_request_release(struct ei_keyboard * ei_keyboard)
{
	if (!ei_keyboard)
		return -ENOENT;

	const struct brei_object *obj = ei_keyboard_get_proto_object(ei_keyboard);
	struct ei * ctx = ei_keyboard_get_context(ei_keyboard);

	if (obj->version < EI_KEYBOARD_REQUEST_RELEASE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_KEYBOARD_REQUEST_RELEASE, "",
		0);
}

int
ei_keyboard_request_key(struct ei_keyboard * ei_keyboard, uint32_t key, uint32_t state)
{
	if (!ei_keyboard)
		return -ENOENT;

	const struct brei_object *obj = ei_keyboard_get_proto_object(ei_keyboard);
	struct ei * ctx = ei_keyboard_get_context(ei_keyboard);

	if (obj->version < EI_KEYBOARD_REQUEST_KEY_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_KEYBOARD_REQUEST_KEY, "uu",
		2, key, state);
}


/**
 * The dispatcher for the ei_keyboard interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_keyboard_dispatcher(
	struct ei_keyboard * ei_keyboard,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_keyboard_interface *interface = ei_keyboard_get_interface(ei_keyboard);
	const struct brei_object *obj = ei_keyboard_get_proto_object(ei_keyboard);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_KEYBOARD_EVENT_DESTROYED:
		if (obj->version < EI_KEYBOARD_EVENT_DESTROYED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->destroyed != NULL);
		return interface->destroyed(ei_keyboard, (args + 0)->u);
	case EI_KEYBOARD_EVENT_KEYMAP:
		if (obj->version < EI_KEYBOARD_EVENT_KEYMAP_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->keymap != NULL);
		return interface->keymap(ei_keyboard, (args + 0)->u, (args + 1)->u, (args + 2)->h);
	case EI_KEYBOARD_EVENT_KEY:
		if (obj->version < EI_KEYBOARD_EVENT_KEY_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->key != NULL);
		return interface->key(ei_keyboard, (args + 0)->u, (args + 1)->u);
	case EI_KEYBOARD_EVENT_MODIFIERS:
		if (obj->version < EI_KEYBOARD_EVENT_MODIFIERS_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->modifiers != NULL);
		return interface->modifiers(ei_keyboard, (args + 0)->u, (args + 1)->u, (args + 2)->u, (args + 3)->u, (args + 4)->u);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_keyboard_requests[] = {
	{ "release", ""  },
	{ "key", "uu"  },
};

static const struct brei_message ei_keyboard_events[] = {
	{ "destroyed", "u" },
	{ "keymap", "uuh" },
	{ "key", "uu" },
	{ "modifiers", "uuuuu" },
};

static const struct brei_message ei_keyboard_incoming[] = {
	{ "destroyed", "u" },
	{ "keymap", "uuh" },
	{ "key", "uu" },
	{ "modifiers", "uuuuu" },
};

const struct brei_interface ei_keyboard_proto_interface = {
    .name = "ei_keyboard", .version = 1,
    .nrequests = 2, .requests = ei_keyboard_requests,
    .nevents = 4, .events = ei_keyboard_events,
    .nincoming = 4, .incoming = ei_keyboard_incoming,
    .dispatcher = (brei_event_dispatcher)ei_keyboard_dispatcher,
};

/***************************************************************************
 * Interface: ei_touchscreen                                               *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_touchscreen_interface *ei_touchscreen_get_interface(struct ei_touchscreen * ei_touchscreen);

/* returns the message sending context from the given struct */
struct ei * ei_touchscreen_get_context(struct ei_touchscreen * ei_touchscreen);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_touchscreen_get_proto_object(struct ei_touchscreen * ei_touchscreen);

/* request opcodes */
#define EI_TOUCHSCREEN_REQUEST_RELEASE                0
#define EI_TOUCHSCREEN_REQUEST_DOWN                   1
#define EI_TOUCHSCREEN_REQUEST_MOTION                 2
#define EI_TOUCHSCREEN_REQUEST_UP                     3
#define EI_TOUCHSCREEN_REQUEST_CANCEL                 4

/* event opcodes */
#define EI_TOUCHSCREEN_EVENT_DESTROYED                0
#define EI_TOUCHSCREEN_EVENT_DOWN                     1
#define EI_TOUCHSCREEN_EVENT_MOTION                   2
#define EI_TOUCHSCREEN_EVENT_UP                       3
#define EI_TOUCHSCREEN_EVENT_CANCEL                   4

/* Message sender functions */
int
ei_touchscreen_request_release(struct ei_touchscreen * ei_touchscreen)
{
	if (!ei_touchscreen)
		return -ENOENT;

	const struct brei_object *obj = ei_touchscreen_get_proto_object(ei_touchscreen);
	struct ei * ctx = ei_touchscreen_get_context(ei_touchscreen);

	if (obj->version < EI_TOUCHSCREEN_REQUEST_RELEASE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_TOUCHSCREEN_REQUEST_RELEASE, "",
		0);
}

int
ei_touchscreen_request_down(struct ei_touchscreen * ei_touchscreen, uint32_t touchid, float x, float y)
{
	if (!ei_touchscreen)
		return -ENOENT;

	const struct brei_object *obj = ei_touchscreen_get_proto_object(ei_touchscreen);
	struct ei * ctx = ei_touchscreen_get_context(ei_touchscreen);

	if (obj->version < EI_TOUCHSCREEN_REQUEST_DOWN_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_TOUCHSCREEN_REQUEST_DOWN, "uff",
		3, touchid, x, y);
}

int
ei_touchscreen_request_motion(struct ei_touchscreen * ei_touchscreen, uint32_t touchid, float x, float y)
{
	if (!ei_touchscreen)
		return -ENOENT;

	const struct brei_object *obj = ei_touchscreen_get_proto_object(ei_touchscreen);
	struct ei * ctx = ei_touchscreen_get_context(ei_touchscreen);

	if (obj->version < EI_TOUCHSCREEN_REQUEST_MOTION_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_TOUCHSCREEN_REQUEST_MOTION, "uff",
		3, touchid, x, y);
}

int
ei_touchscreen_request_up(struct ei_touchscreen * ei_touchscreen, uint32_t touchid)
{
	if (!ei_touchscreen)
		return -ENOENT;

	const struct brei_object *obj = ei_touchscreen_get_proto_object(ei_touchscreen);
	struct ei * ctx = ei_touchscreen_get_context(ei_touchscreen);

	if (obj->version < EI_TOUCHSCREEN_REQUEST_UP_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_TOUCHSCREEN_REQUEST_UP, "u",
		1, touchid);
}

int
ei_touchscreen_request_cancel(struct ei_touchscreen * ei_touchscreen, uint32_t touchid)
{
	if (!ei_touchscreen)
		return -ENOENT;

	const struct brei_object *obj = ei_touchscreen_get_proto_object(ei_touchscreen);
	struct ei * ctx = ei_touchscreen_get_context(ei_touchscreen);

	if (obj->version < EI_TOUCHSCREEN_REQUEST_CANCEL_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_TOUCHSCREEN_REQUEST_CANCEL, "u",
		1, touchid);
}


/**
 * The dispatcher for the ei_touchscreen interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_touchscreen_dispatcher(
	struct ei_touchscreen * ei_touchscreen,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_touchscreen_interface *interface = ei_touchscreen_get_interface(ei_touchscreen);
	const struct brei_object *obj = ei_touchscreen_get_proto_object(ei_touchscreen);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_TOUCHSCREEN_EVENT_DESTROYED:
		if (obj->version < EI_TOUCHSCREEN_EVENT_DESTROYED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->destroyed != NULL);
		return interface->destroyed(ei_touchscreen, (args + 0)->u);
	case EI_TOUCHSCREEN_EVENT_DOWN:
		if (obj->version < EI_TOUCHSCREEN_EVENT_DOWN_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->down != NULL);
		return interface->down(ei_touchscreen, (args + 0)->u, (args + 1)->f, (args + 2)->f);
	case EI_TOUCHSCREEN_EVENT_MOTION:
		if (obj->version < EI_TOUCHSCREEN_EVENT_MOTION_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->motion != NULL);
		return interface->motion(ei_touchscreen, (args + 0)->u, (args + 1)->f, (args + 2)->f);
	case EI_TOUCHSCREEN_EVENT_UP:
		if (obj->version < EI_TOUCHSCREEN_EVENT_UP_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->up != NULL);
		return interface->up(ei_touchscreen, (args + 0)->u);
	case EI_TOUCHSCREEN_EVENT_CANCEL:
		if (obj->version < EI_TOUCHSCREEN_EVENT_CANCEL_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->cancel != NULL);
		return interface->cancel(ei_touchscreen, (args + 0)->u);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_touchscreen_requests[] = {
	{ "release", ""  },
	{ "down", "uff"  },
	{ "motion", "uff"  },
	{ "up", "u"  },
	{ "cancel", "u"  },
};

static const struct brei_message ei_touchscreen_events[] = {
	{ "destroyed", "u" },
	{ "down", "uff" },
	{ "motion", "uff" },
	{ "up", "u" },
	{ "cancel", "u" },
};

static const struct brei_message ei_touchscreen_incoming[] = {
	{ "destroyed", "u" },
	{ "down", "uff" },
	{ "motion", "uff" },
	{ "up", "u" },
	{ "cancel", "u" },
};

const struct brei_interface ei_touchscreen_proto_interface = {
    .name = "ei_touchscreen", .version = 2,
    .nrequests = 5, .requests = ei_touchscreen_requests,
    .nevents = 5, .events = ei_touchscreen_events,
    .nincoming = 5, .incoming = ei_touchscreen_incoming,
    .dispatcher = (brei_event_dispatcher)ei_touchscreen_dispatcher,
};

/***************************************************************************
 * Interface: ei_text                                                      *
 ***************************************************************************/

/* returns the protocol interface from the given struct, see the dispatcher */
const struct ei_text_interface *ei_text_get_interface(struct ei_text * ei_text);

/* returns the message sending context from the given struct */
struct ei * ei_text_get_context(struct ei_text * ei_text);

/* Returns the protocol object id from the given struct */
const struct brei_object *
ei_text_get_proto_object(struct ei_text * ei_text);

/* request opcodes */
#define EI_TEXT_REQUEST_RELEASE                       0
#define EI_TEXT_REQUEST_KEYSYM                        1
#define EI_TEXT_REQUEST_UTF8                          2

/* event opcodes */
#define EI_TEXT_EVENT_DESTROYED                       0
#define EI_TEXT_EVENT_KEYSYM                          1
#define EI_TEXT_EVENT_UTF8                            2

/* Message sender functions */
int
ei_text_request_release(struct ei_text * ei_text)
{
	if (!ei_text)
		return -ENOENT;

	const struct brei_object *obj = ei_text_get_proto_object(ei_text);
	struct ei * ctx = ei_text_get_context(ei_text);

	if (obj->version < EI_TEXT_REQUEST_RELEASE_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_TEXT_REQUEST_RELEASE, "",
		0);
}

int
ei_text_request_keysym(struct ei_text * ei_text, uint32_t keysym, uint32_t state)
{
	if (!ei_text)
		return -ENOENT;

	const struct brei_object *obj = ei_text_get_proto_object(ei_text);
	struct ei * ctx = ei_text_get_context(ei_text);

	if (obj->version < EI_TEXT_REQUEST_KEYSYM_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_TEXT_REQUEST_KEYSYM, "uu",
		2, keysym, state);
}

int
ei_text_request_utf8(struct ei_text * ei_text, const char * text)
{
	if (!ei_text)
		return -ENOENT;

	const struct brei_object *obj = ei_text_get_proto_object(ei_text);
	struct ei * ctx = ei_text_get_context(ei_text);

	if (obj->version < EI_TEXT_REQUEST_UTF8_SINCE_VERSION)
		return -ENOTSUP;

	return ei_send_message(
		ctx, obj, EI_TEXT_REQUEST_UTF8, "s",
		1, text);
}


/**
 * The dispatcher for the ei_text interface is the function called by
 * brei with the messages parsed from the wire.
 */
static struct brei_result *
ei_text_dispatcher(
	struct ei_text * ei_text,
	uint32_t opcode,
	size_t nargs,
	union brei_arg *args
) {
	const struct ei_text_interface *interface = ei_text_get_interface(ei_text);
	const struct brei_object *obj = ei_text_get_proto_object(ei_text);

	if (!interface)
		return NULL;

	switch (opcode) {
	case EI_TEXT_EVENT_DESTROYED:
		if (obj->version < EI_TEXT_EVENT_DESTROYED_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->destroyed != NULL);
		return interface->destroyed(ei_text, (args + 0)->u);
	case EI_TEXT_EVENT_KEYSYM:
		if (obj->version < EI_TEXT_EVENT_KEYSYM_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->keysym != NULL);
		return interface->keysym(ei_text, (args + 0)->u, (args + 1)->u);
	case EI_TEXT_EVENT_UTF8:
		if (obj->version < EI_TEXT_EVENT_UTF8_SINCE_VERSION)
			return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
					       "Opcode %u not supported in this interface version", opcode);
		assert(interface->utf8 != NULL);
		return interface->utf8(ei_text, (args + 0)->s);
	}
	return brei_result_new(BREI_CONNECTION_DISCONNECT_REASON_ERROR,
			       "Opcode %u not supported in this interface version", opcode);
}

static const struct brei_message ei_text_requests[] = {
	{ "release", ""  },
	{ "keysym", "uu"  },
	{ "utf8", "s"  },
};

static const struct brei_message ei_text_events[] = {
	{ "destroyed", "u" },
	{ "keysym", "uu" },
	{ "utf8", "s" },
};

static const struct brei_message ei_text_incoming[] = {
	{ "destroyed", "u" },
	{ "keysym", "uu" },
	{ "utf8", "s" },
};

const struct brei_interface ei_text_proto_interface = {
    .name = "ei_text", .version = 1,
    .nrequests = 3, .requests = ei_text_requests,
    .nevents = 3, .events = ei_text_events,
    .nincoming = 3, .incoming = ei_text_incoming,
    .dispatcher = (brei_event_dispatcher)ei_text_dispatcher,
};

