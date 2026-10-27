"""Frame controller component names and pre-I/O validation, without runtime resources."""
import math

COMMON = ('system', 'bumper', 'trigger', 'grip', 'thumbstick')
HANDED = {
    'left': ('view', 'dpad_up', 'dpad_right', 'dpad_down', 'dpad_left'),
    'right': ('menu', 'a', 'b', 'x', 'y'),
}


def validate_control(side, control, *, touch=False):
    names = COMMON + HANDED[side] + (('thumbrest',) if touch else ())
    if control not in names:
        raise ValueError(f'unsupported {side} controller {"touch" if touch else "button"}: {control}')


def scalar(value, minimum=0, maximum=1):
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or not math.isfinite(value) or not minimum <= value <= maximum):
        raise ValueError(f'input value must be a finite number in [{minimum}, {maximum}]')
    return format(value, '.17g')


def command(args):
    """Validate a parsed input operation and encode its entire atomic native batch."""
    if args.command != 'controller':
        return None
    operation = args.pose_command
    if operation == 'inputs-reset':
        return 'controller-input-release ' + args.side
    if operation not in ('button', 'touch', 'trigger', 'grip', 'thumbstick'):
        return None
    pairs = []
    if operation in ('button', 'touch'):
        validate_control(args.side, args.control, touch=operation == 'touch')
        control = args.control
        pairs.append(('touch' if operation == 'touch' else 'click', '1' if args.state == 'on' else '0'))
    else:
        control = operation
        if operation == 'thumbstick':
            pairs.extend([('x', scalar(args.x, -1, 1)), ('y', scalar(args.y, -1, 1))])
        else:
            pairs.append(('value', scalar(args.value)))
        if args.click is not None:
            pairs.append(('click', '1' if args.click == 'on' else '0'))
    if operation != 'touch' and args.touch is not None:
        pairs.append(('touch', '1' if args.touch == 'on' else '0'))
    return 'controller-inputs ' + args.side + ' ' + ' '.join(
        f'/input/{control}/{component} {value}' for component, value in pairs)
