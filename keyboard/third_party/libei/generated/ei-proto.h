/**
 * GENERATED FILE, DO NOT EDIT
 *
 * SPDX-License-Identifier: MIT
 */



#pragma once

#ifdef _cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <brei-proto.h>
/**
 * Forward declarations. This file is intended to be compile-able without including
 * any of the actual sources files.
 */

struct ei;

/* Interface declarations */
struct ei_handshake;
struct ei_connection;
struct ei_callback;
struct ei_pingpong;
struct ei_seat;
struct ei_device;
struct ei_pointer;
struct ei_pointer_absolute;
struct ei_scroll;
struct ei_button;
struct ei_keyboard;
struct ei_touchscreen;
struct ei_text;

extern const struct brei_interface ei_handshake_proto_interface;
extern const struct brei_interface ei_connection_proto_interface;
extern const struct brei_interface ei_callback_proto_interface;
extern const struct brei_interface ei_pingpong_proto_interface;
extern const struct brei_interface ei_seat_proto_interface;
extern const struct brei_interface ei_device_proto_interface;
extern const struct brei_interface ei_pointer_proto_interface;
extern const struct brei_interface ei_pointer_absolute_proto_interface;
extern const struct brei_interface ei_scroll_proto_interface;
extern const struct brei_interface ei_button_proto_interface;
extern const struct brei_interface ei_keyboard_proto_interface;
extern const struct brei_interface ei_touchscreen_proto_interface;
extern const struct brei_interface ei_text_proto_interface;

#define EI_HANDSHAKE_INTERFACE_NAME "ei_handshake"
#define EI_CONNECTION_INTERFACE_NAME "ei_connection"
#define EI_CALLBACK_INTERFACE_NAME "ei_callback"
#define EI_PINGPONG_INTERFACE_NAME "ei_pingpong"
#define EI_SEAT_INTERFACE_NAME "ei_seat"
#define EI_DEVICE_INTERFACE_NAME "ei_device"
#define EI_POINTER_INTERFACE_NAME "ei_pointer"
#define EI_POINTER_ABSOLUTE_INTERFACE_NAME "ei_pointer_absolute"
#define EI_SCROLL_INTERFACE_NAME "ei_scroll"
#define EI_BUTTON_INTERFACE_NAME "ei_button"
#define EI_KEYBOARD_INTERFACE_NAME "ei_keyboard"
#define EI_TOUCHSCREEN_INTERFACE_NAME "ei_touchscreen"
#define EI_TEXT_INTERFACE_NAME "ei_text"

__attribute__((unused))
static const char *EI_INTERFACE_NAMES[] = {
	EI_HANDSHAKE_INTERFACE_NAME,
	EI_CONNECTION_INTERFACE_NAME,
	EI_CALLBACK_INTERFACE_NAME,
	EI_PINGPONG_INTERFACE_NAME,
	EI_SEAT_INTERFACE_NAME,
	EI_DEVICE_INTERFACE_NAME,
	EI_POINTER_INTERFACE_NAME,
	EI_POINTER_ABSOLUTE_INTERFACE_NAME,
	EI_SCROLL_INTERFACE_NAME,
	EI_BUTTON_INTERFACE_NAME,
	EI_KEYBOARD_INTERFACE_NAME,
	EI_TOUCHSCREEN_INTERFACE_NAME,
	EI_TEXT_INTERFACE_NAME,
};

/* Interface indices as used in the protocol.xml file */
#define EI_HANDSHAKE_INTERFACE_INDEX 0
#define EI_CONNECTION_INTERFACE_INDEX 1
#define EI_CALLBACK_INTERFACE_INDEX 2
#define EI_PINGPONG_INTERFACE_INDEX 3
#define EI_SEAT_INTERFACE_INDEX 4
#define EI_DEVICE_INTERFACE_INDEX 5
#define EI_POINTER_INTERFACE_INDEX 6
#define EI_POINTER_ABSOLUTE_INTERFACE_INDEX 7
#define EI_SCROLL_INTERFACE_INDEX 8
#define EI_BUTTON_INTERFACE_INDEX 9
#define EI_KEYBOARD_INTERFACE_INDEX 10
#define EI_TOUCHSCREEN_INTERFACE_INDEX 11
#define EI_TEXT_INTERFACE_INDEX 12
#define EI_INTERFACE_COUNT 13


/**                                 ei_handshake                                           **/

#define EI_HANDSHAKE_REQUEST_HANDSHAKE_VERSION_SINCE_VERSION 1
#define EI_HANDSHAKE_REQUEST_FINISH_SINCE_VERSION 1
#define EI_HANDSHAKE_REQUEST_CONTEXT_TYPE_SINCE_VERSION 1
#define EI_HANDSHAKE_REQUEST_NAME_SINCE_VERSION 1
#define EI_HANDSHAKE_REQUEST_INTERFACE_VERSION_SINCE_VERSION 1

#define EI_HANDSHAKE_EVENT_HANDSHAKE_VERSION_SINCE_VERSION 1
#define EI_HANDSHAKE_EVENT_INTERFACE_VERSION_SINCE_VERSION 1
#define EI_HANDSHAKE_EVENT_CONNECTION_SINCE_VERSION 1

enum ei_handshake_context_type {
	EI_HANDSHAKE_CONTEXT_TYPE_RECEIVER = 1,
	EI_HANDSHAKE_CONTEXT_TYPE_SENDER = 2,
};

#define EI_HANDSHAKE_CONTEXT_TYPE_RECEIVER_SINCE_VERSION 1
#define EI_HANDSHAKE_CONTEXT_TYPE_SENDER_SINCE_VERSION 1

/* Message sender functions */
extern int
ei_handshake_request_handshake_version(struct ei_handshake * ei_handshake, uint32_t version);

extern int
ei_handshake_request_finish(struct ei_handshake * ei_handshake);

extern int
ei_handshake_request_context_type(struct ei_handshake * ei_handshake, uint32_t context_type);

extern int
ei_handshake_request_name(struct ei_handshake * ei_handshake, const char * name);

extern int
ei_handshake_request_interface_version(struct ei_handshake * ei_handshake, const char * name, uint32_t version);


/**
 * Interface to handle incoming messages for objects of type ei_handshake.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_handshake_interface {
   struct brei_result * (*handshake_version)(struct ei_handshake * ei_handshake, uint32_t version);
   struct brei_result * (*interface_version)(struct ei_handshake * ei_handshake, const char * name, uint32_t version);
   struct brei_result * (*connection)(struct ei_handshake * ei_handshake, uint32_t serial, new_id_t connection, uint32_t version);
};

/**                                 ei_connection                                           **/

#define EI_CONNECTION_REQUEST_SYNC_SINCE_VERSION 1
#define EI_CONNECTION_REQUEST_DISCONNECT_SINCE_VERSION 1

#define EI_CONNECTION_EVENT_DISCONNECTED_SINCE_VERSION 1
#define EI_CONNECTION_EVENT_SEAT_SINCE_VERSION 1
#define EI_CONNECTION_EVENT_INVALID_OBJECT_SINCE_VERSION 1
#define EI_CONNECTION_EVENT_PING_SINCE_VERSION 1

enum ei_connection_disconnect_reason {
	EI_CONNECTION_DISCONNECT_REASON_DISCONNECTED = 0,
	EI_CONNECTION_DISCONNECT_REASON_ERROR = 1,
	EI_CONNECTION_DISCONNECT_REASON_MODE = 2,
	EI_CONNECTION_DISCONNECT_REASON_PROTOCOL = 3,
	EI_CONNECTION_DISCONNECT_REASON_VALUE = 4,
	EI_CONNECTION_DISCONNECT_REASON_TRANSPORT = 5,
};

#define EI_CONNECTION_DISCONNECT_REASON_DISCONNECTED_SINCE_VERSION 1
#define EI_CONNECTION_DISCONNECT_REASON_ERROR_SINCE_VERSION 1
#define EI_CONNECTION_DISCONNECT_REASON_MODE_SINCE_VERSION 1
#define EI_CONNECTION_DISCONNECT_REASON_PROTOCOL_SINCE_VERSION 1
#define EI_CONNECTION_DISCONNECT_REASON_VALUE_SINCE_VERSION 1
#define EI_CONNECTION_DISCONNECT_REASON_TRANSPORT_SINCE_VERSION 1

/* Message sender functions */
extern int
ei_connection_request_sync(struct ei_connection * ei_connection, new_id_t callback, uint32_t version);

extern int
ei_connection_request_disconnect(struct ei_connection * ei_connection);


/**
 * Interface to handle incoming messages for objects of type ei_connection.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_connection_interface {
   struct brei_result * (*disconnected)(struct ei_connection * ei_connection, uint32_t last_serial, uint32_t reason, const char * explanation);
   struct brei_result * (*seat)(struct ei_connection * ei_connection, new_id_t seat, uint32_t version);
   struct brei_result * (*invalid_object)(struct ei_connection * ei_connection, uint32_t last_serial, uint64_t invalid_id);
   struct brei_result * (*ping)(struct ei_connection * ei_connection, new_id_t ping, uint32_t version);
};

/**                                 ei_callback                                           **/


#define EI_CALLBACK_EVENT_DONE_SINCE_VERSION 1


/* Message sender functions */

/**
 * Interface to handle incoming messages for objects of type ei_callback.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_callback_interface {
   struct brei_result * (*done)(struct ei_callback * ei_callback, uint64_t callback_data);
};

/**                                 ei_pingpong                                           **/

#define EI_PINGPONG_REQUEST_DONE_SINCE_VERSION 1



/* Message sender functions */
extern int
ei_pingpong_request_done(struct ei_pingpong * ei_pingpong, uint64_t callback_data);


/**
 * Interface to handle incoming messages for objects of type ei_pingpong.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_pingpong_interface {
};

/**                                 ei_seat                                           **/

#define EI_SEAT_REQUEST_RELEASE_SINCE_VERSION 1
#define EI_SEAT_REQUEST_BIND_SINCE_VERSION 1
#define EI_SEAT_REQUEST_REQUEST_DEVICE_SINCE_VERSION 2

#define EI_SEAT_EVENT_DESTROYED_SINCE_VERSION 1
#define EI_SEAT_EVENT_NAME_SINCE_VERSION 1
#define EI_SEAT_EVENT_CAPABILITY_SINCE_VERSION 1
#define EI_SEAT_EVENT_DONE_SINCE_VERSION 1
#define EI_SEAT_EVENT_DEVICE_SINCE_VERSION 1


/* Message sender functions */
extern int
ei_seat_request_release(struct ei_seat * ei_seat);

extern int
ei_seat_request_bind(struct ei_seat * ei_seat, uint64_t capabilities);

extern int
ei_seat_request_request_device(struct ei_seat * ei_seat, uint64_t capabilities);


/**
 * Interface to handle incoming messages for objects of type ei_seat.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_seat_interface {
   struct brei_result * (*destroyed)(struct ei_seat * ei_seat, uint32_t serial);
   struct brei_result * (*name)(struct ei_seat * ei_seat, const char * name);
   struct brei_result * (*capability)(struct ei_seat * ei_seat, uint64_t mask, const char * interface);
   struct brei_result * (*done)(struct ei_seat * ei_seat);
   struct brei_result * (*device)(struct ei_seat * ei_seat, new_id_t device, uint32_t version);
};

/**                                 ei_device                                           **/

#define EI_DEVICE_REQUEST_RELEASE_SINCE_VERSION 1
#define EI_DEVICE_REQUEST_START_EMULATING_SINCE_VERSION 1
#define EI_DEVICE_REQUEST_STOP_EMULATING_SINCE_VERSION 1
#define EI_DEVICE_REQUEST_FRAME_SINCE_VERSION 1
#define EI_DEVICE_REQUEST_READY_SINCE_VERSION 3

#define EI_DEVICE_EVENT_DESTROYED_SINCE_VERSION 1
#define EI_DEVICE_EVENT_NAME_SINCE_VERSION 1
#define EI_DEVICE_EVENT_DEVICE_TYPE_SINCE_VERSION 1
#define EI_DEVICE_EVENT_DIMENSIONS_SINCE_VERSION 1
#define EI_DEVICE_EVENT_REGION_SINCE_VERSION 1
#define EI_DEVICE_EVENT_INTERFACE_SINCE_VERSION 1
#define EI_DEVICE_EVENT_DONE_SINCE_VERSION 1
#define EI_DEVICE_EVENT_RESUMED_SINCE_VERSION 1
#define EI_DEVICE_EVENT_PAUSED_SINCE_VERSION 1
#define EI_DEVICE_EVENT_START_EMULATING_SINCE_VERSION 1
#define EI_DEVICE_EVENT_STOP_EMULATING_SINCE_VERSION 1
#define EI_DEVICE_EVENT_FRAME_SINCE_VERSION 1
#define EI_DEVICE_EVENT_REGION_MAPPING_ID_SINCE_VERSION 2

enum ei_device_device_type {
	EI_DEVICE_DEVICE_TYPE_VIRTUAL = 1,
	EI_DEVICE_DEVICE_TYPE_PHYSICAL = 2,
};

#define EI_DEVICE_DEVICE_TYPE_VIRTUAL_SINCE_VERSION 1
#define EI_DEVICE_DEVICE_TYPE_PHYSICAL_SINCE_VERSION 1

/* Message sender functions */
extern int
ei_device_request_release(struct ei_device * ei_device);

extern int
ei_device_request_start_emulating(struct ei_device * ei_device, uint32_t last_serial, uint32_t sequence);

extern int
ei_device_request_stop_emulating(struct ei_device * ei_device, uint32_t last_serial);

extern int
ei_device_request_frame(struct ei_device * ei_device, uint32_t last_serial, uint64_t timestamp);

extern int
ei_device_request_ready(struct ei_device * ei_device);


/**
 * Interface to handle incoming messages for objects of type ei_device.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_device_interface {
   struct brei_result * (*destroyed)(struct ei_device * ei_device, uint32_t serial);
   struct brei_result * (*name)(struct ei_device * ei_device, const char * name);
   struct brei_result * (*device_type)(struct ei_device * ei_device, uint32_t device_type);
   struct brei_result * (*dimensions)(struct ei_device * ei_device, uint32_t width, uint32_t height);
   struct brei_result * (*region)(struct ei_device * ei_device, uint32_t offset_x, uint32_t offset_y, uint32_t width, uint32_t hight, float scale);
   struct brei_result * (*interface)(struct ei_device * ei_device, new_id_t object, const char * interface_name, uint32_t version);
   struct brei_result * (*done)(struct ei_device * ei_device);
   struct brei_result * (*resumed)(struct ei_device * ei_device, uint32_t serial);
   struct brei_result * (*paused)(struct ei_device * ei_device, uint32_t serial);
   struct brei_result * (*start_emulating)(struct ei_device * ei_device, uint32_t serial, uint32_t sequence);
   struct brei_result * (*stop_emulating)(struct ei_device * ei_device, uint32_t serial);
   struct brei_result * (*frame)(struct ei_device * ei_device, uint32_t serial, uint64_t timestamp);
   struct brei_result * (*region_mapping_id)(struct ei_device * ei_device, const char * mapping_id);
};

/**                                 ei_pointer                                           **/

#define EI_POINTER_REQUEST_RELEASE_SINCE_VERSION 1
#define EI_POINTER_REQUEST_MOTION_RELATIVE_SINCE_VERSION 1

#define EI_POINTER_EVENT_DESTROYED_SINCE_VERSION 1
#define EI_POINTER_EVENT_MOTION_RELATIVE_SINCE_VERSION 1


/* Message sender functions */
extern int
ei_pointer_request_release(struct ei_pointer * ei_pointer);

extern int
ei_pointer_request_motion_relative(struct ei_pointer * ei_pointer, float x, float y);


/**
 * Interface to handle incoming messages for objects of type ei_pointer.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_pointer_interface {
   struct brei_result * (*destroyed)(struct ei_pointer * ei_pointer, uint32_t serial);
   struct brei_result * (*motion_relative)(struct ei_pointer * ei_pointer, float x, float y);
};

/**                                 ei_pointer_absolute                                           **/

#define EI_POINTER_ABSOLUTE_REQUEST_RELEASE_SINCE_VERSION 1
#define EI_POINTER_ABSOLUTE_REQUEST_MOTION_ABSOLUTE_SINCE_VERSION 1

#define EI_POINTER_ABSOLUTE_EVENT_DESTROYED_SINCE_VERSION 1
#define EI_POINTER_ABSOLUTE_EVENT_MOTION_ABSOLUTE_SINCE_VERSION 1


/* Message sender functions */
extern int
ei_pointer_absolute_request_release(struct ei_pointer_absolute * ei_pointer_absolute);

extern int
ei_pointer_absolute_request_motion_absolute(struct ei_pointer_absolute * ei_pointer_absolute, float x, float y);


/**
 * Interface to handle incoming messages for objects of type ei_pointer_absolute.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_pointer_absolute_interface {
   struct brei_result * (*destroyed)(struct ei_pointer_absolute * ei_pointer_absolute, uint32_t serial);
   struct brei_result * (*motion_absolute)(struct ei_pointer_absolute * ei_pointer_absolute, float x, float y);
};

/**                                 ei_scroll                                           **/

#define EI_SCROLL_REQUEST_RELEASE_SINCE_VERSION 1
#define EI_SCROLL_REQUEST_SCROLL_SINCE_VERSION 1
#define EI_SCROLL_REQUEST_SCROLL_DISCRETE_SINCE_VERSION 1
#define EI_SCROLL_REQUEST_SCROLL_STOP_SINCE_VERSION 1

#define EI_SCROLL_EVENT_DESTROYED_SINCE_VERSION 1
#define EI_SCROLL_EVENT_SCROLL_SINCE_VERSION 1
#define EI_SCROLL_EVENT_SCROLL_DISCRETE_SINCE_VERSION 1
#define EI_SCROLL_EVENT_SCROLL_STOP_SINCE_VERSION 1


/* Message sender functions */
extern int
ei_scroll_request_release(struct ei_scroll * ei_scroll);

extern int
ei_scroll_request_scroll(struct ei_scroll * ei_scroll, float x, float y);

extern int
ei_scroll_request_scroll_discrete(struct ei_scroll * ei_scroll, int32_t x, int32_t y);

extern int
ei_scroll_request_scroll_stop(struct ei_scroll * ei_scroll, uint32_t x, uint32_t y, uint32_t is_cancel);


/**
 * Interface to handle incoming messages for objects of type ei_scroll.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_scroll_interface {
   struct brei_result * (*destroyed)(struct ei_scroll * ei_scroll, uint32_t serial);
   struct brei_result * (*scroll)(struct ei_scroll * ei_scroll, float x, float y);
   struct brei_result * (*scroll_discrete)(struct ei_scroll * ei_scroll, int32_t x, int32_t y);
   struct brei_result * (*scroll_stop)(struct ei_scroll * ei_scroll, uint32_t x, uint32_t y, uint32_t is_cancel);
};

/**                                 ei_button                                           **/

#define EI_BUTTON_REQUEST_RELEASE_SINCE_VERSION 1
#define EI_BUTTON_REQUEST_BUTTON_SINCE_VERSION 1

#define EI_BUTTON_EVENT_DESTROYED_SINCE_VERSION 1
#define EI_BUTTON_EVENT_BUTTON_SINCE_VERSION 1

enum ei_button_button_state {
	EI_BUTTON_BUTTON_STATE_RELEASED = 0,
	EI_BUTTON_BUTTON_STATE_PRESS = 1,
};

#define EI_BUTTON_BUTTON_STATE_RELEASED_SINCE_VERSION 1
#define EI_BUTTON_BUTTON_STATE_PRESS_SINCE_VERSION 1

/* Message sender functions */
extern int
ei_button_request_release(struct ei_button * ei_button);

extern int
ei_button_request_button(struct ei_button * ei_button, uint32_t button, uint32_t state);


/**
 * Interface to handle incoming messages for objects of type ei_button.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_button_interface {
   struct brei_result * (*destroyed)(struct ei_button * ei_button, uint32_t serial);
   struct brei_result * (*button)(struct ei_button * ei_button, uint32_t button, uint32_t state);
};

/**                                 ei_keyboard                                           **/

#define EI_KEYBOARD_REQUEST_RELEASE_SINCE_VERSION 1
#define EI_KEYBOARD_REQUEST_KEY_SINCE_VERSION 1

#define EI_KEYBOARD_EVENT_DESTROYED_SINCE_VERSION 1
#define EI_KEYBOARD_EVENT_KEYMAP_SINCE_VERSION 1
#define EI_KEYBOARD_EVENT_KEY_SINCE_VERSION 1
#define EI_KEYBOARD_EVENT_MODIFIERS_SINCE_VERSION 1

enum ei_keyboard_key_state {
	EI_KEYBOARD_KEY_STATE_RELEASED = 0,
	EI_KEYBOARD_KEY_STATE_PRESS = 1,
};

#define EI_KEYBOARD_KEY_STATE_RELEASED_SINCE_VERSION 1
#define EI_KEYBOARD_KEY_STATE_PRESS_SINCE_VERSION 1
enum ei_keyboard_keymap_type {
	EI_KEYBOARD_KEYMAP_TYPE_XKB = 1,
};

#define EI_KEYBOARD_KEYMAP_TYPE_XKB_SINCE_VERSION 1

/* Message sender functions */
extern int
ei_keyboard_request_release(struct ei_keyboard * ei_keyboard);

extern int
ei_keyboard_request_key(struct ei_keyboard * ei_keyboard, uint32_t key, uint32_t state);


/**
 * Interface to handle incoming messages for objects of type ei_keyboard.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_keyboard_interface {
   struct brei_result * (*destroyed)(struct ei_keyboard * ei_keyboard, uint32_t serial);
   struct brei_result * (*keymap)(struct ei_keyboard * ei_keyboard, uint32_t keymap_type, uint32_t size, int keymap);
   struct brei_result * (*key)(struct ei_keyboard * ei_keyboard, uint32_t key, uint32_t state);
   struct brei_result * (*modifiers)(struct ei_keyboard * ei_keyboard, uint32_t serial, uint32_t depressed, uint32_t locked, uint32_t latched, uint32_t group);
};

/**                                 ei_touchscreen                                           **/

#define EI_TOUCHSCREEN_REQUEST_RELEASE_SINCE_VERSION 1
#define EI_TOUCHSCREEN_REQUEST_DOWN_SINCE_VERSION 1
#define EI_TOUCHSCREEN_REQUEST_MOTION_SINCE_VERSION 1
#define EI_TOUCHSCREEN_REQUEST_UP_SINCE_VERSION 1
#define EI_TOUCHSCREEN_REQUEST_CANCEL_SINCE_VERSION 2

#define EI_TOUCHSCREEN_EVENT_DESTROYED_SINCE_VERSION 1
#define EI_TOUCHSCREEN_EVENT_DOWN_SINCE_VERSION 1
#define EI_TOUCHSCREEN_EVENT_MOTION_SINCE_VERSION 1
#define EI_TOUCHSCREEN_EVENT_UP_SINCE_VERSION 1
#define EI_TOUCHSCREEN_EVENT_CANCEL_SINCE_VERSION 2


/* Message sender functions */
extern int
ei_touchscreen_request_release(struct ei_touchscreen * ei_touchscreen);

extern int
ei_touchscreen_request_down(struct ei_touchscreen * ei_touchscreen, uint32_t touchid, float x, float y);

extern int
ei_touchscreen_request_motion(struct ei_touchscreen * ei_touchscreen, uint32_t touchid, float x, float y);

extern int
ei_touchscreen_request_up(struct ei_touchscreen * ei_touchscreen, uint32_t touchid);

extern int
ei_touchscreen_request_cancel(struct ei_touchscreen * ei_touchscreen, uint32_t touchid);


/**
 * Interface to handle incoming messages for objects of type ei_touchscreen.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_touchscreen_interface {
   struct brei_result * (*destroyed)(struct ei_touchscreen * ei_touchscreen, uint32_t serial);
   struct brei_result * (*down)(struct ei_touchscreen * ei_touchscreen, uint32_t touchid, float x, float y);
   struct brei_result * (*motion)(struct ei_touchscreen * ei_touchscreen, uint32_t touchid, float x, float y);
   struct brei_result * (*up)(struct ei_touchscreen * ei_touchscreen, uint32_t touchid);
   struct brei_result * (*cancel)(struct ei_touchscreen * ei_touchscreen, uint32_t touchid);
};

/**                                 ei_text                                           **/

#define EI_TEXT_REQUEST_RELEASE_SINCE_VERSION 1
#define EI_TEXT_REQUEST_KEYSYM_SINCE_VERSION 1
#define EI_TEXT_REQUEST_UTF8_SINCE_VERSION 1

#define EI_TEXT_EVENT_DESTROYED_SINCE_VERSION 1
#define EI_TEXT_EVENT_KEYSYM_SINCE_VERSION 1
#define EI_TEXT_EVENT_UTF8_SINCE_VERSION 1


/* Message sender functions */
extern int
ei_text_request_release(struct ei_text * ei_text);

extern int
ei_text_request_keysym(struct ei_text * ei_text, uint32_t keysym, uint32_t state);

extern int
ei_text_request_utf8(struct ei_text * ei_text, const char * text);


/**
 * Interface to handle incoming messages for objects of type ei_text.
 *
 * After parsing the wire message, the data is dispatched into the functions below.
 */
struct ei_text_interface {
   struct brei_result * (*destroyed)(struct ei_text * ei_text, uint32_t serial);
   struct brei_result * (*keysym)(struct ei_text * ei_text, uint32_t keysym, uint32_t state);
   struct brei_result * (*utf8)(struct ei_text * ei_text, const char * text);
};

#ifdef _cplusplus
}
#endif