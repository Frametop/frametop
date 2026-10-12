#!/usr/bin/env python3
"""Bounded ft-screens native portal ABI client and CLI. No camera session owner."""
import argparse
import json
import math
import socket
import sys
import time

ADDRESS = "\0ft_screens"
MAX_REPLY = 4096


class PortalError(RuntimeError):
    """Unknown or unconfirmed backend state, never evidence that cameras stopped."""


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate key")
        result[key] = value
    return result


def parse_status(reply):
    if not reply.startswith("ok portal "):
        raise PortalError("Native portal backend unavailable or unsupported reply")
    try:
        data = json.loads(reply[len("ok portal "):], object_pairs_hook=_object)
        if not isinstance(data, dict):
            raise TypeError("status must be an object")
        if type(data.get("wanted")) is not bool or type(data.get("connected")) is not bool:
            raise ValueError("invalid intent or publisher status")
        return data
    except (ValueError, TypeError, RecursionError) as exc:
        raise PortalError("Invalid native portal status: " + str(exc)) from exc


class Client:
    def __init__(self, address=ADDRESS, timeout=1.0):
        if not isinstance(timeout, (float, int)) or not math.isfinite(timeout) or not 0 < timeout <= 5:
            raise ValueError("timeout must be greater than zero and at most 5 seconds")
        self.address, self.timeout = address, timeout

    def ask(self, command, *, deadline=None):
        """A fresh connected socket rejects foreign and late replies; one absolute deadline."""
        deadline = time.monotonic() + self.timeout if deadline is None else deadline
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
                sock.bind("")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError()
                sock.settimeout(remaining)
                sock.connect(self.address)
                sock.send(command.encode("ascii"))
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError()
                sock.settimeout(remaining)
                data, _ancillary, flags, _sender = sock.recvmsg(MAX_REPLY)
                if flags & socket.MSG_TRUNC:
                    raise PortalError("Native portal reply too large")
                return data.decode("utf-8", errors="strict")
        except TimeoutError as exc:
            raise PortalError("Native portal request timed out; state unknown") from exc
        except (OSError, UnicodeError) as exc:
            raise PortalError("Native portal backend unavailable; state unknown: " + str(exc)) from exc

    def status(self):
        return parse_status(self.ask("passthrough portal status"))

    def set_wanted(self, enabled):
        if type(enabled) is not bool:
            raise ValueError("native intent must be boolean")
        deadline = time.monotonic() + self.timeout
        ack = parse_status(self.ask("passthrough portal " + ("on" if enabled else "off"), deadline=deadline))
        if ack["wanted"] != enabled:
            raise PortalError("Native portal acknowledgement did not match requested intent: ")
        actual = parse_status(self.ask("passthrough portal status", deadline=deadline))
        if actual["wanted"] != enabled:
            raise PortalError("Native portal readback did not confirm requested intent")
        return actual


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", default="@ft_screens", help="abstract @name or UNIX path")
    parser.add_argument("--timeout", type=float, default=1.0, help="total operation deadline in seconds, at most 5")
    parser.add_argument("command", choices=("on", "off", "status"))
    args = parser.parse_args(argv)
    address = "\0" + args.socket[1:] if args.socket.startswith("@") else args.socket
    try:
        client = Client(address, args.timeout)
        result = client.status() if args.command == "status" else client.set_wanted(args.command == "on")
        print(json.dumps(result, sort_keys=True))
        return 0
    except (PortalError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
