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

typedef uint64_t object_id_t;
typedef object_id_t new_id_t;

enum brei_handshake_context_type {
	BREI_HANDSHAKE_CONTEXT_TYPE_RECEIVER = 1,
	BREI_HANDSHAKE_CONTEXT_TYPE_SENDER = 2,
};
enum brei_connection_disconnect_reason {
	BREI_CONNECTION_DISCONNECT_REASON_DISCONNECTED = 0,
	BREI_CONNECTION_DISCONNECT_REASON_ERROR = 1,
	BREI_CONNECTION_DISCONNECT_REASON_MODE = 2,
	BREI_CONNECTION_DISCONNECT_REASON_PROTOCOL = 3,
	BREI_CONNECTION_DISCONNECT_REASON_VALUE = 4,
	BREI_CONNECTION_DISCONNECT_REASON_TRANSPORT = 5,
};
enum brei_device_device_type {
	BREI_DEVICE_DEVICE_TYPE_VIRTUAL = 1,
	BREI_DEVICE_DEVICE_TYPE_PHYSICAL = 2,
};
enum brei_button_button_state {
	BREI_BUTTON_BUTTON_STATE_RELEASED = 0,
	BREI_BUTTON_BUTTON_STATE_PRESS = 1,
};
enum brei_keyboard_key_state {
	BREI_KEYBOARD_KEY_STATE_RELEASED = 0,
	BREI_KEYBOARD_KEY_STATE_PRESS = 1,
};
enum brei_keyboard_keymap_type {
	BREI_KEYBOARD_KEYMAP_TYPE_XKB = 1,
};

#ifdef _cplusplus
}
#endif