// Lists the Mac's displays for Frametop's host setup, one per line, tab-separated:
//   id  key  name  width  height  x  y  builtin  main
// id is the CGDirectDisplayID, Sunshine's output_name. It can change when a display is plugged in
// again, so instances are tied to key (vendor-model-serial, -2 and up for identical monitors) and
// ft-sunshine-instance looks the id up at every start. width and height are capture pixels.
// Run with: osascript -l JavaScript displays.js
ObjC.import('AppKit');
ObjC.import('CoreGraphics');

function run() {
  const screens = $.NSScreen.screens;
  const rows = [];
  for (let i = 0; i < screens.count; i++) {
    const screen = screens.objectAtIndex(i);
    const id = ObjC.unwrap(screen.deviceDescription.objectForKey('NSScreenNumber'));
    const frame = screen.frame;
    const scale = screen.backingScaleFactor;
    rows.push({
      id,
      base: [$.CGDisplayVendorNumber(id), $.CGDisplayModelNumber(id), $.CGDisplaySerialNumber(id)].join('-'),
      name: ObjC.unwrap(screen.localizedName).replace(/[\t\n]/g, ' '),
      width: Math.round(frame.size.width * scale),
      height: Math.round(frame.size.height * scale),
      x: frame.origin.x,
      y: frame.origin.y,
      builtin: $.CGDisplayIsBuiltin(id) ? 1 : 0,
      main: $.CGDisplayIsMain(id) ? 1 : 0,
    });
  }
  // Identical monitors without serial numbers share a base; number them left to right.
  rows.sort((a, b) => a.x - b.x || a.y - b.y);
  const seen = {};
  for (const r of rows) {
    seen[r.base] = (seen[r.base] || 0) + 1;
    r.key = seen[r.base] === 1 ? r.base : `${r.base}-${seen[r.base]}`;
  }
  return rows.map(r => [r.id, r.key, r.name, r.width, r.height, r.x, r.y, r.builtin, r.main].join('\t')).join('\n');
}
