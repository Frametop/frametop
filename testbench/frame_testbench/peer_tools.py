"""The MCP server's fake-peer tools for Frametop Together (peer_cli.py, docs/peers.md)."""
from typing import Annotated, Literal

from mcp.types import CallToolResult
from pydantic import Field

Peer = Annotated[int, Field(strict=True, ge=1, le=3)]
PeerName = Annotated[str, Field(strict=True, pattern=r'^[A-Za-z0-9][A-Za-z0-9 ._-]{0,31}$')]
Model = Annotated[str, Field(strict=True, pattern=r'^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$')]
Pose = Literal['stand', 'wave', 'reach', 'type', 'sit', 'lean', 'look']
Metres = Annotated[float, Field(strict=True, allow_inf_nan=False, ge=-20, le=20)]
Degrees = Annotated[float, Field(strict=True, allow_inf_nan=False, ge=-360, le=360)]
Hz = Annotated[float, Field(strict=True, allow_inf_nan=False, ge=50, le=2000)]


def register(server, execute):
    @server.tool()
    async def peer_start(peer: Peer, name: PeerName | None = None) -> CallToolResult:
        """Run fake peer 1, 2 or 3 for Frametop Together: the real session code and programs
        with nobody wearing them, taking calls on port 47193 + peer of this Frame's tailnet
        address. It stands in no room until peer_join. release_all stops every peer."""
        return await execute(['peer', str(peer), 'start'] + (['--name', name] if name else []))

    @server.tool()
    async def peer_stop(peer: Peer) -> CallToolResult:
        """The peer leaves its room and its programs end."""
        return await execute(['peer', str(peer), 'stop'])

    @server.tool()
    async def peer_state(peer: Peer) -> CallToolResult:
        """What the peer does (pose, where it walked, the note it hums, the model it wears, which
        of its programs run) and its own ft-together's state: mode, place, people, panels."""
        return await execute(['peer', str(peer), 'state'])

    @server.tool()
    async def peer_join(peer: Peer) -> CallToolResult:
        """The peer comes into the room open on this Frame. The testbench makes the invite and
        accepts the peer's ask, as the wearer would. Frametop Together must be on and the
        wearer's room open."""
        return await execute(['peer', str(peer), 'join'])

    @server.tool()
    async def peer_leave(peer: Peer) -> CallToolResult:
        """The peer leaves the room and keeps running."""
        return await execute(['peer', str(peer), 'leave'])

    @server.tool()
    async def peer_pose(peer: Peer, pose: Pose) -> CallToolResult:
        """How the peer stands: a still pose, or one that moves (wave, type, look)."""
        return await execute(['peer', str(peer), 'pose', pose])

    @server.tool()
    async def peer_walk(peer: Peer, right: Metres, back: Metres, degrees: Degrees = 0) -> CallToolResult:
        """Where the peer stands, measured from the spot the room gave it: metres to its right
        and backwards, and a turn to its left in degrees. Not added to an earlier walk."""
        return await execute(['peer', str(peer), 'walk', '--', repr(right), repr(back), repr(degrees)])

    @server.tool()
    async def peer_avatar(peer: Peer, model: Model | None = None) -> CallToolResult:
        """The peer wears a model of this Frame's avatars folder, by name, or none (the abstract
        figure). The others get its file only if the model is marked as one to pass on."""
        return await execute(['peer', str(peer), 'wear'] + ([model] if model else []))

    @server.tool()
    async def peer_speak(peer: Peer, hz: Hz | None = None) -> CallToolResult:
        """The peer hums a note of that many Hz, which also moves its mouth, or is quiet."""
        return await execute(['peer', str(peer), 'speak', 'off' if hz is None else repr(hz)])

    @server.tool()
    async def peer_away(peer: Peer, state: Literal['game', 'off', 'back']) -> CallToolResult:
        """The peer is away in a game, has its headset off, or is back."""
        return await execute(['peer', str(peer), 'away', state])
