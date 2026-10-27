"""Typed stdio MCP tools for one startup-configured frame-testbench target."""
import argparse
import base64
import json
import logging
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from typing import Annotated, Literal

import anyio
import jsonschema
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError
from mcp.types import CallToolResult, ImageContent, TextContent
from pydantic import Field

from . import controller_inputs, transport

Number = Annotated[float, Field(strict=True, allow_inf_nan=False)]
Vector = tuple[Number, Number, Number]
Space = Literal['standing', 'raw']
Side = Literal['left', 'right']
Button = Literal['system', 'bumper', 'trigger', 'grip', 'thumbstick',
                 'view', 'dpad_up', 'dpad_right', 'dpad_down', 'dpad_left',
                 'menu', 'a', 'b', 'x', 'y']
TouchControl = Literal[Button, 'thumbrest']
Boolean = Annotated[bool, Field(strict=True)]
UnitInput = Annotated[float, Field(strict=True, allow_inf_nan=False, ge=0, le=1)]
AxisInput = Annotated[float, Field(strict=True, allow_inf_nan=False, ge=-1, le=1)]


@dataclass(frozen=True)
class Config:
    host: str | None = None
    remote_root: str = 'dev/frame-testbench'
    artifacts_dir: Path = Path('artifacts')


class BenchMCP(FastMCP):
    async def list_tools(self):
        tools = await super().list_tools()
        for tool in tools:
            tool.inputSchema = dict(tool.inputSchema, additionalProperties=False)
        return tools

    async def call_tool(self, name, arguments):
        # FastMCP normally coerces JSON-in-strings and ignores unknown keys.
        # Validate the advertised schema first so malformed calls never reach I/O.
        try:
            tools = {tool.name: tool for tool in await self.list_tools()}
            if name not in tools:
                raise ValueError(f'unknown tool: {name}')
            jsonschema.validate(arguments, tools[name].inputSchema)
            return await super().call_tool(name, arguments)
        except (ValueError, jsonschema.ValidationError, ToolError) as error:
            return CallToolResult(content=[TextContent(type='text', text=str(error))], isError=True)


def result(data):
    if not isinstance(data, dict) or data.get('ok') is not True:
        raise RuntimeError(f'backend failed: {data}')
    return CallToolResult(content=[TextContent(type='text', text=json.dumps(data, allow_nan=False))],
                          structuredContent=data, isError=False)


def number(value):
    # argparse treats negative scientific notation as an option, not a number.
    text = format(value, '.17g')
    return format(Decimal(text), 'f') if 'e' in text else text


def input_flags(clicked=None, touched=None):
    return [item for flag, value in [('--click', clicked), ('--touch', touched)]
            if value is not None for item in (flag, 'on' if value else 'off')]


def pose_arguments(kind, vector, rotation, space):
    return [kind, *map(number, vector), '--rotation', *map(number, rotation), '--space', space]


def run_cli(arguments):
    """Invoke the installed CLI module without relying on PATH or the current directory."""
    completed = subprocess.run(
        [sys.executable, '-c', 'from frame_testbench.cli import main; raise SystemExit(main())', *arguments],
        capture_output=True, text=True, timeout=420, check=False)
    if completed.stderr:
        print(completed.stderr, end='', file=sys.stderr)
    try:
        data = json.loads(completed.stdout)
    except ValueError:
        raise RuntimeError('frame-testbench returned invalid JSON: ' + completed.stdout[-2048:]) from None
    if completed.returncode or not isinstance(data, dict) or data.get('ok') is not True:
        message = f'frame-testbench failed ({completed.returncode}): {data}'
        logging.getLogger(__name__).error(message)
        raise RuntimeError(message)
    return data


def capture_result(data, directory, remote):
    reply = result(data)
    files = data.get('local_files' if remote else 'files', {})
    for name in ('preview.png', 'stereo.png'):
        expected = directory / name
        # Never follow backend-provided paths outside this call's owned directory.
        if files.get(name) != str(expected) or expected.is_symlink() or expected.resolve().parent != directory:
            raise ValueError(f'capture returned an unsafe or missing path for {name}')
        with expected.open('rb') as stream:
            image = stream.read(32 * 1024 * 1024 + 1)
        if len(image) > 32 * 1024 * 1024:
            raise ValueError(f'capture {name} exceeds 32 MiB image limit')
        if not image.startswith(b'\x89PNG\r\n\x1a\n'):
            raise ValueError(f'capture {name} is not PNG')
        reply.content.append(ImageContent(type='image', mimeType='image/png',
                                          data=base64.b64encode(image).decode('ascii')))
    return reply


def create_server(config: Config, *, runner=run_cli):
    if config.host:
        transport.remote_argv(config.host, config.remote_root, [])
    prefix = ['--host', config.host, '--remote-root', config.remote_root] if config.host else []
    server = BenchMCP('frame-testbench')
    lock = anyio.Lock()

    async def execute(arguments) -> CallToolResult:
        async with lock:
            # run_sync shields its worker from MCP cancellation until completion.
            data = await anyio.to_thread.run_sync(runner, prefix + arguments, abandon_on_cancel=False)
            return result(data)

    @server.tool()
    async def status() -> CallToolResult:
        """Read runtime, compositor, HMD and input override status."""
        return await execute(['status'])

    @server.tool()
    async def worn(state: Literal['on', 'off', 'physical']) -> CallToolResult:
        """Override HMD proximity, or restore the physical sensor. Persists until released."""
        return await execute(['worn', state])

    @server.tool()
    async def hmd_pose_set(position: Vector, rotation: Vector = (0, 0, 0),
                           space: Space = 'standing') -> CallToolResult:
        """Set persistent HMD XYZ position in meters and yaw/pitch/roll in degrees."""
        return await execute(['pose', 'set', *pose_arguments('--position', position, rotation, space)])

    @server.tool()
    async def hmd_pose_move(translation: Vector = (0, 0, 0), rotation: Vector = (0, 0, 0),
                            space: Space = 'standing') -> CallToolResult:
        """Offset HMD XYZ in meters and yaw/pitch/roll in degrees. Requires pose set first."""
        return await execute(['pose', 'move', *pose_arguments('--translation', translation, rotation, space)])

    @server.tool()
    async def hmd_pose_reset() -> CallToolResult:
        """Restore physical HMD tracking without changing the worn override."""
        return await execute(['pose', 'reset'])

    @server.tool()
    async def controller_pose_set(side: Side, position: Vector, rotation: Vector = (0, 0, 0),
                                  space: Space = 'standing') -> CallToolResult:
        """Set a persistent left/right controller pose. XYZ meters, yaw/pitch/roll degrees."""
        return await execute(['controller', side, 'set', *pose_arguments('--position', position, rotation, space)])

    @server.tool()
    async def controller_pose_move(side: Side, translation: Vector = (0, 0, 0),
                                   rotation: Vector = (0, 0, 0), space: Space = 'standing') -> CallToolResult:
        """Offset one controller in meters/degrees. Requires that controller's pose set first."""
        return await execute(['controller', side, 'move', *pose_arguments('--translation', translation, rotation, space)])

    @server.tool()
    async def controller_pose_reset(side: Side) -> CallToolResult:
        """Clear inputs and disconnect one synthetic controller, leaving other devices unchanged."""
        return await execute(['controller', side, 'reset'])

    @server.tool()
    async def controller_button(side: Side, button: Button, pressed: Boolean,
                                touched: Boolean | None = None) -> CallToolResult:
        """Set a button click, optionally touch. Requires pose set; omitted touch is unchanged.

        Both sides: system, bumper, trigger, grip, thumbstick. Right: menu, a, b, x, y.
        Left: view, dpad_up, dpad_right, dpad_down, dpad_left.
        """
        controller_inputs.validate_control(side, button)
        return await execute(['controller', side, 'button', button, 'on' if pressed else 'off',
                              *input_flags(touched=touched)])

    @server.tool()
    async def controller_trigger(side: Side, value: UnitInput, clicked: Boolean | None = None,
                                 touched: Boolean | None = None) -> CallToolResult:
        """Set trigger value 0..1. Requires pose set. Omitted click/touch stay unchanged; no threshold inference."""
        return await execute(['controller', side, 'trigger', number(value), *input_flags(clicked, touched)])

    @server.tool()
    async def controller_grip(side: Side, value: UnitInput, clicked: Boolean | None = None,
                              touched: Boolean | None = None) -> CallToolResult:
        """Set grip value 0..1. Requires pose set. Omitted click/touch stay unchanged; no threshold inference."""
        return await execute(['controller', side, 'grip', number(value), *input_flags(clicked, touched)])

    @server.tool()
    async def controller_thumbstick(side: Side, x: AxisInput, y: AxisInput,
                                    clicked: Boolean | None = None, touched: Boolean | None = None) -> CallToolResult:
        """Set thumbstick axes -1..1. Requires pose set. Omitted click/touch stay unchanged."""
        return await execute(['controller', side, 'thumbstick', number(x), number(y), *input_flags(clicked, touched)])

    @server.tool()
    async def controller_touch(side: Side, control: TouchControl, touched: Boolean) -> CallToolResult:
        """Set touch independently. Same handed names as controller_button, plus thumbrest on both sides.

        Requires controller pose set first. Does not change click or analog values.
        """
        controller_inputs.validate_control(side, control, touch=True)
        return await execute(['controller', side, 'touch', control, 'on' if touched else 'off'])

    @server.tool()
    async def controller_inputs_reset(side: Side) -> CallToolResult:
        """Neutralize one controller's buttons, axes and touches, retaining its pose."""
        return await execute(['controller', side, 'inputs-reset'])

    @server.tool()
    async def controller_input_status() -> CallToolResult:
        """Read independent SteamVR controller actions, not the proxy's commanded input state."""
        return await execute(['inputs'])

    @server.tool()
    async def compositor(mode: Literal['awake', 'auto']) -> CallToolResult:
        """Keep the compositor awake or restore automatic standby. This setting persists."""
        return await execute(['compositor', mode])

    @server.tool()
    async def capture() -> CallToolResult:
        """Capture fresh compositor PNG images and runtime metadata."""
        async with lock:
            parent = config.artifacts_dir.expanduser().resolve()
            parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            # The native observer and SSH fetch require a path that does not yet exist.
            directory = Path(tempfile.mkdtemp(prefix='capture-', dir=parent)) / 'images'
            arguments = prefix + ['capture', '--fetch' if config.host else '--output', str(directory)]
            data = await anyio.to_thread.run_sync(runner, arguments, abandon_on_cancel=False)
            return await anyio.to_thread.run_sync(
                capture_result, data, directory, bool(config.host), abandon_on_cancel=False)

    @server.tool()
    async def release_all() -> CallToolResult:
        """Clear controller inputs and release all pose/worn overrides. Does not change compositor policy."""
        return await execute(['release'])

    return server


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', help='fixed SSH hostname or configured alias')
    parser.add_argument('--remote-root', default='dev/frame-testbench', help='remote checkout path')
    parser.add_argument('--artifacts-dir', type=Path, default=Path('artifacts'),
                        help='local directory for retained captures')
    args = parser.parse_args(argv)
    logging.basicConfig(stream=sys.stderr, level=logging.INFO)
    try:
        server = create_server(Config(**vars(args)))
    except ValueError as error:
        parser.error(str(error))
    server.run(transport='stdio')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
