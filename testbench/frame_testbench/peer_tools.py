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
Panel = Annotated[int, Field(strict=True, ge=0, le=31)]
Part = Annotated[float, Field(strict=True, allow_inf_nan=False, ge=0, le=1)]
Gaze = Annotated[float, Field(strict=True, allow_inf_nan=False, ge=-63.5, le=63.5)]
Far = Annotated[float, Field(strict=True, allow_inf_nan=False, ge=-100, le=100)]
Button = Annotated[int, Field(strict=True, ge=1, le=3)]
Scroll = Annotated[int, Field(strict=True, ge=-2400, le=2400)]
Key = Annotated[int, Field(strict=True, ge=1, le=111)]
Down = Annotated[bool, Field(strict=True)]
Width = Literal[640, 896, 1280, 1408, 1664, 1920]   # what the Frame's encoder is known to take (peershare.py)
Height = Annotated[int, Field(strict=True, ge=360, le=1200, multiple_of=8)]
Person = Annotated[int, Field(strict=True, ge=0, le=3)]
Windows = Annotated[int, Field(strict=True, ge=1, le=4)]   # a PC's panels in a room (peerpc.py)


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

    register_acts(server, execute)
    register_share(server, execute)
    register_pc(server, execute)


def register_acts(server, execute):
    """What a peer does in a room with its hand and its eyes (peeracts.py)."""
    @server.tool()
    async def peer_point(peer: Peer, panel: Panel | None = None, x: Part = 0.5, y: Part = 0.5, down: Down = False) -> CallToolResult:
        """The peer's pointer on a panel of the room, by the panel's id (peer_state lists them):
        x across and y down its picture, each 0 to 1, with a button held when down. Everyone in
        the room sees it. Without a panel, its pointer is on none."""
        if panel is None:
            return await execute(['peer', str(peer), 'point', 'off'])
        return await execute(['peer', str(peer), 'point', str(panel), repr(x), repr(y)] + (['--down'] if down else []))

    @server.tool()
    async def peer_look(peer: Peer, yaw: Gaze | None = None, pitch: Gaze = 0) -> CallToolResult:
        """The peer shares where it looks: degrees to its left (yaw) and up (pitch) from the way
        its head faces. The others see its avatar's eyes turn and a faint mark on the panel the
        look meets. Without a yaw, it shares that no longer."""
        if yaw is None:
            return await execute(['peer', str(peer), 'look', 'off'])
        return await execute(['peer', str(peer), 'look', '--', repr(yaw), repr(pitch)])

    @server.tool()
    async def peer_control_ask(peer: Peer, panel: Panel) -> CallToolResult:
        """The peer asks a panel's owner for control of it. Only the owner gives it: for a panel
        of this Frame's, its wearer, on the Frametop Together page. No tool here does."""
        return await execute(['peer', str(peer), 'ask', str(panel)])

    @server.tool()
    async def peer_act_move(peer: Peer, panel: Panel, x: Part, y: Part) -> CallToolResult:
        """On a panel the peer was given: its pointer on the owner's desktop goes there, x
        across and y down the picture, each 0 to 1. Refused for a panel it wasn't given."""
        return await execute(['peer', str(peer), 'act', str(panel), 'move', repr(x), repr(y)])

    @server.tool()
    async def peer_act_button(peer: Peer, panel: Panel, button: Button, down: Down) -> CallToolResult:
        """On a panel the peer was given: mouse button 1 (left), 2 (right) or 3 (middle) is
        pressed or let go where its pointer is. A click is one of each."""
        return await execute(['peer', str(peer), 'act', str(panel), 'button', str(button), str(int(down))])

    @server.tool()
    async def peer_act_scroll(peer: Peer, panel: Panel, right: Scroll = 0, down: Scroll = 0) -> CallToolResult:
        """On a panel the peer was given: a scroll, 120 a notch, to the right and down."""
        return await execute(['peer', str(peer), 'act', str(panel), 'scroll', '--', str(right), str(down)])

    @server.tool()
    async def peer_act_key(peer: Peer, panel: Panel, code: Key, down: Down) -> CallToolResult:
        """On a panel the peer was given: a key is pressed or let go, by its Linux code (30 is
        A). It goes where the owner's desktop has the typing. Codes 1 to 111, not 99."""
        return await execute(['peer', str(peer), 'act', str(panel), 'key', str(code), str(int(down))])

    @server.tool()
    async def peer_put(peer: Peer, panel: Panel, x: Far, y: Far, z: Far, degrees: Degrees = 0) -> CallToolResult:
        """The peer puts someone else's panel there in the room: its middle in metres and a turn
        to its left. Its owner's Frame moves it, if the peer may: it owns the room or was given
        the panel."""
        return await execute(['peer', str(peer), 'put', str(panel), '--', repr(x), repr(y), repr(z), repr(degrees)])


def register_share(server, execute):
    """A peer's own test panel in a room (peershare.py)."""
    @server.tool()
    async def peer_share(peer: Peer, width: Width = 1280, height: Height = 720, x: Far | None = None, y: Far = 1.5, z: Far = -2.5,
                         degrees: Degrees = 0) -> CallToolResult:
        """The peer puts a test panel of its own into the room: a picture that counts, of that
        many pixels, two metres wide, its middle at x, y, z in metres and turned to its left.
        Without x it stays where it was, or stands on a spot of the peer's own. Said again it
        moves the panel. The picture is encoded on this Frame, only while someone watches."""
        at = [] if x is None else ['--at', repr(x), repr(y), repr(z), '--turn', repr(degrees)]
        return await execute(['peer', str(peer), 'share', '--size', '%dx%d' % (width, height)] + at)

    @server.tool()
    async def peer_unshare(peer: Peer) -> CallToolResult:
        """The peer takes its test panel out of the room."""
        return await execute(['peer', str(peer), 'unshare'])

    @server.tool()
    async def peer_control_give(peer: Peer, person: Person) -> CallToolResult:
        """The peer gives its own test panel to the person in that place of the room (0 is the
        room's owner), asked or not. What they then do on it reaches the peer, which has no
        desktop: peer_state counts it. Never a panel of the wearer's: no tool gives those."""
        return await execute(['peer', str(peer), 'give', str(person)])

    @server.tool()
    async def peer_control_take(peer: Peer, person: Person | None = None) -> CallToolResult:
        """The peer takes its test panel back from that person, or from everyone, and says no to
        their ask."""
        return await execute(['peer', str(peer), 'take'] + ([] if person is None else [str(person)]))


def register_pc(server, execute):
    """A peer's split compute PC and its windows in a room (peerpc.py)."""
    @server.tool()
    async def peer_pc(peer: Peer, windows: Windows | None = None) -> CallToolResult:
        """The peer's split compute PC comes to it, and that many of its windows (as many as it
        had, or 1) are panels in the room, a metre wide, in a row above the peer's own panel:
        640 by 360, a second of a picture that counts, again and again. Said again it changes
        how many. No encoder runs for them: the pictures were encoded once. With the peer's
        own panel that is up to five panels a peer, for a room with more than a Frame decodes
        at once."""
        return await execute(['peer', str(peer), 'pc'] + ([] if windows is None else [str(windows)]))

    @server.tool()
    async def peer_unpc(peer: Peer) -> CallToolResult:
        """The peer's PC's windows leave the room, and the PC goes."""
        return await execute(['peer', str(peer), 'unpc'])
