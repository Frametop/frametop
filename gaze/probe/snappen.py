"""ft-gazeprobe's drawing without python-cairo: SteamOS 0.5 has none on the host, and a
Gtk.DrawingArea's draw function is handed a cairo context that PyGObject can't pass on then.

Canvas is the widget the probe draws on. Its draw function gets a Pen in the cairo context's
place: the cairo calls the probe uses, with cairo's meaning, on the widget's Gtk.Snapshot
(paths are Gsk's, since GTK 4.14; text is Pango's).

Two things differ from cairo, neither of which the probe relies on: after show_text there is no
current point (cairo leaves it after the text, so a later arc starts with a line from there),
and fill and stroke take the nonzero winding rule and butt caps, cairo's defaults, with no way
to change them. A call the probe starts to use has to be added here first.
"""
import math

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Gsk", "4.0")
gi.require_version("Graphene", "1.0")
from gi.repository import Gdk, Graphene, Gsk, Gtk, Pango  # noqa: E402

QUARTER = math.pi / 2  # an arc goes to Gsk in pieces no longer than this


def colour(red, green, blue, alpha):
    c = Gdk.RGBA()  # GDK 4's takes no arguments in PyGObject
    c.red, c.green, c.blue, c.alpha = red, green, blue, alpha
    return c


class Extents:
    def __init__(self, x_advance):
        self.x_advance = x_advance


class Pen:
    def __init__(self, snapshot, widget, width, height):
        self.snapshot, self.widget = snapshot, widget
        self.width, self.height = width, height
        self.colour = colour(0, 0, 0, 1)
        self.line_width = 2.0
        self.dash = []
        self.family, self.size = "sans", 10.0
        self.builder = None  # the path so far
        self.at = None       # the current point
        self.start = None    # ...and where its part of the path began

    # --- colour, line and font

    def set_source_rgb(self, red, green, blue):
        self.set_source_rgba(red, green, blue, 1)

    def set_source_rgba(self, red, green, blue, alpha):
        self.colour = colour(red, green, blue, alpha)

    def set_line_width(self, width):
        self.line_width = float(width)

    def set_dash(self, dashes):
        self.dash = [float(d) for d in dashes]

    def select_font_face(self, family):
        self.family = family

    def set_font_size(self, size):
        self.size = float(size)

    # --- the path

    def move_to(self, x, y):
        if self.builder is None:
            self.builder = Gsk.PathBuilder.new()
        self.builder.move_to(x, y)
        self.at = self.start = (x, y)

    def line_to(self, x, y):
        if self.at is None:
            return self.move_to(x, y)
        self.builder.line_to(x, y)
        self.at = (x, y)

    def new_sub_path(self):
        self.at = None

    def close_path(self):
        if self.at is not None:
            self.builder.close()
            self.at = self.start

    def rectangle(self, x, y, width, height):
        """A closed box of its own."""
        self.move_to(x, y)
        self.line_to(x + width, y)
        self.line_to(x + width, y + height)
        self.line_to(x, y + height)
        self.close_path()

    def arc(self, xc, yc, radius, angle1, angle2):
        """From angle1 round to angle2, the way the angle grows (clockwise on a screen), after a
        line from the current point when there is one."""
        while angle2 < angle1:
            angle2 += 2 * math.pi
        self.line_to(xc + radius * math.cos(angle1), yc + radius * math.sin(angle1))
        pieces = max(1, math.ceil((angle2 - angle1) / QUARTER - 1e-9))
        for i in range(1, pieces + 1):
            a = angle1 + (angle2 - angle1) * i / pieces
            self.at = (xc + radius * math.cos(a), yc + radius * math.sin(a))
            self.builder.svg_arc_to(radius, radius, 0, False, True, *self.at)

    def take(self):
        """The path, which is then empty again. None: there was none."""
        builder, self.builder, self.at, self.start = self.builder, None, None, None
        return builder.to_path() if builder else None

    # --- drawing

    def paint(self):
        self.snapshot.append_color(self.colour, Graphene.Rect().init(0, 0, self.width, self.height))

    def fill(self):
        path = self.take()
        if path:
            self.snapshot.append_fill(path, Gsk.FillRule.WINDING, self.colour)

    def stroke(self):
        path = self.take()
        if not path:
            return
        stroke = Gsk.Stroke.new(self.line_width)
        if self.dash:
            stroke.set_dash(self.dash)
        self.snapshot.append_stroke(path, stroke, self.colour)

    def layout(self, text):
        layout = self.widget.create_pango_layout(text)
        font = Pango.FontDescription.new()
        font.set_family(self.family)
        font.set_absolute_size(self.size * Pango.SCALE)
        layout.set_font_description(font)
        return layout

    def text_extents(self, text):
        """How far the text takes the pen along, as x_advance: all of cairo's extents the probe asks for."""
        return Extents(self.layout(text).get_size()[0] / Pango.SCALE)

    def show_text(self, text):
        """The text with its baseline's left end at the current point."""
        x, y = self.at or (0.0, 0.0)
        self.take()
        layout = self.layout(text)
        self.snapshot.save()
        self.snapshot.translate(Graphene.Point().init(x, y - layout.get_baseline() / Pango.SCALE))
        self.snapshot.append_layout(layout, self.colour)
        self.snapshot.restore()


class Canvas(Gtk.Widget):
    """A Gtk.DrawingArea's stand-in: draw(canvas, pen, width, height) draws it."""

    def __init__(self, draw, **properties):
        super().__init__(**properties)
        self.draw = draw

    def do_snapshot(self, snapshot):
        width, height = self.get_width(), self.get_height()
        self.draw(self, Pen(snapshot, self, width, height), width, height)
