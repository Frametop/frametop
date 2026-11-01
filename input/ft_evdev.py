"""ft_evdev: the Linux input ABI the input relay and its virtual gamepad use, from Python
(include/uapi/linux/input.h, input-event-codes.h, uinput.h).

The event record, the ioctl request numbers for evdev nodes (EVIOC*) and uinput (UI_*), and the
event types both need. Device-specific key and axis codes stay with their users. Standard
library only; nothing here opens a device. Users: input/input-relay.py, input/vrpad.py.
"""
import struct

EV_SYN, EV_KEY, EV_REL, EV_ABS, EV_MSC = 0x00, 0x01, 0x02, 0x03, 0x04
SYN_REPORT = 0
KEY_MAX = 0x2FF

# struct input_event on 64-bit: struct timeval (2 x long), u16 type, u16 code, s32 value.
EVENT = struct.Struct("llHHi")


def ioc(direction, nr, size, kind):
    """_IOC: an ioctl request number (direction 0 none, 1 write, 2 read)."""
    return (direction << 30) | (size << 16) | (ord(kind) << 8) | nr


def iow(kind, nr, size):
    return ioc(1, nr, size, kind)


def ior(kind, nr, size):
    return ioc(2, nr, size, kind)


# uinput
UI_DEV_CREATE = ioc(0, 1, 0, "U")
UI_DEV_DESTROY = ioc(0, 2, 0, "U")
UI_DEV_SETUP = iow("U", 3, 92)  # struct uinput_setup: input_id (4 x u16), name[80], u32
UI_ABS_SETUP = iow("U", 4, 28)  # struct uinput_abs_setup: u16 code, struct input_absinfo (6 x s32)
UI_SET_EVBIT = iow("U", 100, 4)
UI_SET_KEYBIT = iow("U", 101, 4)
UI_SET_RELBIT = iow("U", 102, 4)
UI_SET_ABSBIT = iow("U", 103, 4)

# evdev
EVIOCGRAB = iow("E", 0x90, 4)
EVIOCGKEY = ior("E", 0x18, (KEY_MAX + 8) // 8)
EVIOCGID = ior("E", 0x02, 8)
KEYMAP_ENTRY = struct.Struct("BBHI32s")  # struct input_keymap_entry: flags, len, index, keycode, scancode
INPUT_KEYMAP_BY_INDEX = 1
EVIOCGKEYCODE_V2 = ior("E", 0x04, KEYMAP_ENTRY.size)
EVIOCSKEYCODE_V2 = iow("E", 0x04, KEYMAP_ENTRY.size)


def eviocgbit(ev, length):
    return ior("E", 0x20 + ev, length)


def eviocgname(length):
    return ior("E", 0x06, length)


def eviocguniq(length):
    return ior("E", 0x08, length)
