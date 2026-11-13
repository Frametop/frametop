#!/usr/bin/env python3
"""ft-gazeprobe's pen (gaze/probe/snappen.py): what its cairo calls put on a Gtk.Snapshot, with
python-cairo out of reach, as on SteamOS 0.5's host.

  python3 gaze/test/snappen-test.py

Needs the host's GTK 4 (4.14 or later, for Gsk's paths): skipped where there's none, as in the
dev container. The text and the widget need a display too.
"""
import math
import os
import sys
import unittest

sys.modules["cairo"] = None  # "import cairo" fails from here on, PyGObject's own too

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "probe"))
try:
    import snappen
    from gi.repository import Graphene, Gsk, Gtk
    HAVE = hasattr(Gsk, "PathBuilder")
# AttributeError: a "gi" that isn't PyGObject (the dev image has an empty namespace one)
except (ImportError, ValueError, AttributeError):
    HAVE = False
DISPLAY = HAVE and Gtk.init_check()


def pen(widget=None, width=200, height=100):
    return snappen.Pen(Gtk.Snapshot(), widget, width, height)


def node(p):
    """What the pen drew when it drew one thing."""
    return p.snapshot.to_node()


def box(thing):
    """(left, top, right, bottom) of a node or, for a path, of its outline."""
    b = thing.get_bounds()
    if isinstance(b, tuple):  # a path: (whether it has any, the bounds)
        b = b[1]
    return b.get_x(), b.get_y(), b.get_x() + b.get_width(), b.get_y() + b.get_height()


def inside(path, x, y):
    return path.in_fill(Graphene.Point().init(x, y), Gsk.FillRule.WINDING)


@unittest.skipUnless(HAVE, "the pen is on GTK 4.14's paths: no PyGObject with them here (it runs on the Frame)")
class PenTest(unittest.TestCase):
    def near(self, got, want, by=0.01):
        for g, w in zip(got, want, strict=True):
            self.assertAlmostEqual(g, w, delta=by)

    def test_a_filled_circle(self):
        p = pen()
        p.set_source_rgba(1, 0.5, 0.25, 0.75)
        p.arc(50, 40, 10, 0, 2 * math.pi)
        path = p.take()
        self.near(box(path), (40, 30, 60, 50))
        self.assertTrue(inside(path, 50, 40) and inside(path, 57, 47))
        self.assertFalse(inside(path, 58, 48))  # just past the rim, inside the square round it
        p.arc(50, 40, 10, 0, 2 * math.pi)
        p.fill()
        self.assertIsNone(p.builder)  # the path went with the fill
        p.fill()                      # ...so this draws nothing more
        n = node(p)
        self.assertIsInstance(n, Gsk.FillNode)
        self.near(box(n.get_path()), (40, 30, 60, 50))
        c = n.get_child().get_color()
        self.near((c.red, c.green, c.blue, c.alpha), (1, 0.5, 0.25, 0.75))

    def test_part_of_a_ring(self):
        # the probe's progress ring: from the top, clockwise
        p = pen()
        p.set_line_width(3)
        p.arc(100, 50, 22, -math.pi / 2, -math.pi / 2 + 2 * math.pi * 0.25)
        self.near(box(p.take()), (100, 28, 122, 50))  # the top right quarter
        p.arc(100, 50, 22, -math.pi / 2, -math.pi / 2 + 2 * math.pi * 0.25)
        p.stroke()
        n = node(p)
        self.assertIsInstance(n, Gsk.StrokeNode)
        self.assertEqual(n.get_stroke().get_line_width(), 3)
        p = pen()
        p.arc(100, 50, 22, -math.pi / 2, -math.pi / 2)  # nothing of it yet: no error, nothing to see
        p.stroke()

    def test_an_end_angle_before_the_start_goes_round(self):
        p = pen()
        p.arc(50, 50, 10, math.pi, 0)  # cairo: the end is taken a turn later, so the top half
        self.near(box(p.take()), (40, 40, 60, 50))

    def test_a_rounded_box_is_one_closed_outline(self):
        p = pen()
        x, y, w, h, r = 20, 10, 100, 60, 6
        p.new_sub_path()
        p.arc(x + w - r, y + r, r, -math.pi / 2, 0)
        p.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
        p.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
        p.arc(x + r, y + r, r, math.pi, 1.5 * math.pi)
        p.close_path()
        path = p.take()
        self.assertTrue(path.is_closed())
        self.near(box(path), (20, 10, 120, 70))
        self.assertTrue(inside(path, 70, 40) and inside(path, 21, 40) and inside(path, 70, 11))
        self.assertFalse(inside(path, 20.5, 10.5))  # the corner is cut round

    def test_an_arc_after_a_point_starts_with_a_line(self):
        p = pen()
        p.move_to(0, 0)
        p.arc(50, 0, 10, 0, math.pi / 2)
        self.assertTrue(p.take().to_string().startswith("M 0 0 L 60 0"))
        p = pen()
        p.move_to(0, 0)
        p.new_sub_path()
        p.arc(50, 0, 10, 0, math.pi / 2)
        self.assertIn("M 60 0", p.take().to_string())

    def test_lines_and_dashes(self):
        p = pen()
        p.set_line_width(1)
        p.set_dash([6, 5])
        for x in (10, 20):
            p.move_to(x, 0)
            p.line_to(x, 100)
        p.stroke()
        n = node(p)
        self.near(box(n.get_path()), (10, 0, 20, 100))
        self.assertEqual(n.get_stroke().get_dash(), [6, 5])
        p.set_dash([])
        p.line_to(5, 5)   # no current point: starts there, as cairo does
        p.line_to(15, 5)
        self.near(box(p.take()), (5, 5, 15, 5))

    def test_a_box(self):
        p = pen()
        p.rectangle(10, 20, 30, 40)
        p.rectangle(100, 20, 30, 40)  # a second one isn't joined to the first
        path = p.take()
        self.assertEqual(path.to_string().count("Z"), 2)  # each closed
        self.near(box(path), (10, 20, 130, 60))
        self.assertTrue(inside(path, 11, 21) and inside(path, 129, 59))
        self.assertFalse(inside(path, 70, 40))

    def test_paint_covers_it(self):
        p = pen(width=320, height=240)
        p.set_source_rgb(0.11, 0.12, 0.14)
        p.paint()
        n = node(p)
        self.assertIsInstance(n, Gsk.ColorNode)
        self.near(box(n), (0, 0, 320, 240))

    @unittest.skipUnless(DISPLAY, "needs a display")
    def test_text_stands_on_its_point(self):
        p = pen(Gtk.Label())
        p.select_font_face("sans")
        p.set_font_size(26)
        p.move_to(40, 60)
        p.show_text("Hxk")
        left, top, right, bottom = box(node(p))
        self.assertTrue(38 <= left <= 44 and right > 70, (left, right))
        self.assertTrue(60 - 26 <= top <= 60 - 13 and 59 <= bottom <= 62, (top, bottom))  # no letter goes below the line
        self.assertIsNone(p.at)  # an arc after it doesn't start with a line from the text

    @unittest.skipUnless(DISPLAY, "needs a display")
    def test_text_is_as_wide_as_it_draws(self):
        p = pen(Gtk.Label())
        p.set_font_size(26)
        wide, narrow = p.text_extents("Tracked").x_advance, p.text_extents("Tr").x_advance
        self.assertTrue(0 < narrow < wide < 26 * 7, (narrow, wide))
        p.move_to(0, 60)
        p.show_text("Tracked")
        left, _top, right, _bottom = box(node(p))
        self.assertTrue(abs(right - wide) < 4 and left < 4, (left, right, wide))

    @unittest.skipUnless(DISPLAY, "needs a display")
    def test_the_canvas_draws_with_a_pen(self):
        seen = []
        canvas = snappen.Canvas(lambda *a: seen.append(a), hexpand=True)
        self.assertTrue(canvas.get_hexpand())
        canvas.do_snapshot(Gtk.Snapshot())
        (widget, p, width, height), = seen
        self.assertIs(widget, canvas)
        self.assertIsInstance(p, snappen.Pen)
        self.assertEqual((width, height, p.width, p.height), (0, 0, 0, 0))  # not laid out yet


if __name__ == "__main__":
    unittest.main()
