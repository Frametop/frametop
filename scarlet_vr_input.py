#!/usr/bin/env python3
"""Controller push-to-talk for scarlet-voice: hold a bound button (or tap it, in toggle mode).

SteamVR only gives controller input to the app that has focus, and while the 2D Steam
or desktop screens are up the compositor's laser mouse holds it, so an OpenVR action
never fires there. Instead this reads raw component state from vrserver's local web
socket (127.0.0.1:27062), the same stream the controller binding page uses for its live
view. It sees the button whatever has focus. Press -> `start vr`, release -> `stop` on
the scarlet-voice control socket; the daemon applies the 2 s minimum.

The button is observed, not captured: whatever else is bound to it still happens.
Buttons come from ~/.config/scarlet-voice/bindings.json (see bindings.py and the
scarlet-config app); changes apply within a second.
"""
import asyncio
import json
import logging
import os
import socket
import time
import urllib.request
from pathlib import Path

import websockets

import bindings

SOCKET_PATH = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")) / "scarlet-voice/control.sock"
VRSERVER = "http://127.0.0.1:27062"
MAILBOX = f"scarlet_voice_ptt_{os.getpid()}"
VR_HEADERS = {"Referer": f"{VRSERVER}/dashboard/controllerbinding.html"}
DEVICE_POLL = 5  # seconds between checks for controllers connecting or leaving

log = logging.getLogger("scarlet-vr-input")


def send(command):
    try:
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(2)
            sock.connect(str(SOCKET_PATH))
            sock.sendall((command + "\n").encode())
            sock.recv(4096)
    except OSError as err:
        log.warning("scarlet-voice not reachable (%s): %s", command, err)


async def connect():
    # vrserver serves the binding page, and its socket, to local clients from that page.
    return await websockets.connect(VRSERVER.replace("http", "ws"), origin=VRSERVER, max_size=None,
                                    additional_headers=VR_HEADERS)


def connected():
    """Root paths of the controllers SteamVR has right now (vrserver only streams changes,
    so a quiet stream says nothing about whether a controller is still there)."""
    req = urllib.request.Request(f"{VRSERVER}/input/getstate.json", headers=VR_HEADERS)
    with urllib.request.urlopen(req, timeout=3) as resp:
        return {d["root_path"] for d in json.load(resp).get("devices", []) if d.get("root_path")}


async def subscribe(ws, device, mailbox, on=True):
    kind = "request_input_state_updates" if on else "cancel_input_state_updates"
    message = {"type": kind, "device_path": device, "returnAddress": mailbox}
    await ws.send(f"mailbox_send input_server {json.dumps(message)}")


def wanted():
    """device -> set of inputs, from bindings.json."""
    out = {}
    for b in bindings.load()["controller"]:
        out.setdefault(b["device"], set()).add(b["input"])
    return out


async def run_session():
    """One connection to vrserver. Returns or raises when SteamVR goes away."""
    async with await connect() as ws:
        await ws.send(f"mailbox_open {MAILBOX}")
        stamp, inputs, mode = bindings.mtime(), wanted(), bindings.load()["controller_mode"]
        for device in inputs:
            await subscribe(ws, device, MAILBOX)
        log.info("connected to SteamVR, watching %s (%s)", describe(inputs), mode)
        held = False
        pressed = set()  # (device, input) currently down
        present = await asyncio.to_thread(connected)
        next_poll = time.monotonic() + DEVICE_POLL
        try:
            while True:
                try:
                    raw = await asyncio.wait_for(ws.recv(), 1)
                except asyncio.TimeoutError:
                    raw = None
                now = time.monotonic()
                if raw is not None:
                    message = json.loads(raw)
                    device = message.get("device")
                    if message.get("type") == "update_component_states" and device in inputs:
                        components = message.get("components", {})
                        for name in inputs[device]:
                            if components.get(name):
                                pressed.add((device, name))
                            elif name in components:
                                pressed.discard((device, name))
                if bindings.mtime() != stamp:
                    stamp, new, mode = bindings.mtime(), wanted(), bindings.load()["controller_mode"]
                    for device in inputs.keys() - new.keys():
                        await subscribe(ws, device, MAILBOX, on=False)
                    for device in new.keys() - inputs.keys():
                        await subscribe(ws, device, MAILBOX)
                    inputs = new
                    pressed = {(d, i) for d, i in pressed if i in inputs.get(d, ())}
                    log.info("bindings changed, watching %s (%s)", describe(inputs), mode)
                if now >= next_poll:
                    next_poll = now + DEVICE_POLL
                    current = await asyncio.to_thread(connected)
                    for device in inputs.keys() & (current - present):
                        log.info("%s connected", device)
                        await subscribe(ws, device, MAILBOX, on=False)
                        await subscribe(ws, device, MAILBOX)
                    for device in inputs.keys() & (present - current):
                        log.info("%s disconnected", device)
                        pressed = {(d, i) for d, i in pressed if d != device}
                    present = current
                if pressed and not held:
                    held = True
                    send("toggle vr" if mode == "toggle" else "start vr")
                elif held and not pressed:
                    held = False
                    if mode == "hold":
                        send("stop")
        finally:
            if held and mode == "hold":
                send("stop")


def describe(inputs):
    return ", ".join(f"{d} {i}" for d, names in inputs.items() for i in sorted(names)) or "nothing"


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    while True:
        try:
            asyncio.run(run_session())
        except (OSError, websockets.WebSocketException) as err:
            log.info("SteamVR not reachable (%s); retrying", err)
        time.sleep(5)


if __name__ == "__main__":
    main()
