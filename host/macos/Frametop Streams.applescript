-- Frametop Streams: a menu bar icon for this Mac's Frametop displays (its Sunshine instances, one
-- per display). Click a display to turn it on or off, or turn them all on or off. It runs
-- ft-sunshine. frametop-host-setup.sh compiles it into ~/Applications as a stay-open app with no
-- Dock icon and starts it at login (the LaunchAgent frametop.streams).
use AppleScript version "2.4"
use framework "Foundation"
use framework "AppKit"
use scripting additions

property statusItem : missing value
property theMenu : missing value
property theTimer : missing value
property tool : ""
property logFile : ""
property busyPid : 0 -- the ft-sunshine started from the menu, while it runs
property busyText : ""
property ticks : 0

on run
	set tool to quoted form of ((POSIX path of (path to application support folder from user domain)) & "Frametop/sunshine/bin/ft-sunshine")
	set logFile to (POSIX path of (path to library folder from user domain)) & "Logs/Frametop/streams.log"
	set statusItem to current application's NSStatusBar's systemStatusBar()'s statusItemWithLength:(current application's NSVariableStatusItemLength)
	set icon to current application's NSImage's imageWithSystemSymbolName:"display.2" accessibilityDescription:"Frametop Streams"
	icon's setTemplate:true
	statusItem's button()'s setImage:icon
	statusItem's button()'s setToolTip:"Frametop Streams"
	set theMenu to current application's NSMenu's alloc()'s initWithTitle:"Frametop Streams"
	theMenu's setAutoenablesItems:false
	theMenu's setDelegate:me
	statusItem's setMenu:theMenu
	refresh()
	set theTimer to current application's NSTimer's scheduledTimerWithTimeInterval:2 target:me selector:"tick:" userInfo:(missing value) repeats:true
end run

-- Every 2 s while a change runs, otherwise every 10 s: the icon dims while every display is
-- off, whatever turned them off.
on tick:aTimer
	set ticks to ticks + 1
	if busyPid is not 0 or ticks mod 5 = 0 then refresh()
end tick:

on menuNeedsUpdate:aMenu
	refresh()
end menuNeedsUpdate:

on refresh()
	if busyPid is not 0 then
		try
			do shell script "kill -0 " & busyPid
		on error
			set busyPid to 0
		end try
	end if
	theMenu's removeAllItems()
	try
		set rows to paragraphs of (do shell script tool & " status --tsv")
	on error msg
		addItem(msg, missing value, missing value, false)
		addItem("Run frametop-host-setup.sh to set this Mac up.", missing value, missing value, false)
		theMenu's addItem:(current application's NSMenuItem's separatorItem())
		addItem("Quit Frametop Streams", "quitApp:", missing value, true)
		statusItem's button()'s setAppearsDisabled:true
		return
	end try
	set ready to busyPid is 0
	set anyOn to false
	set saved to AppleScript's text item delimiters
	set AppleScript's text item delimiters to tab
	repeat with r in rows
		set f to text items of r
		set n to (item 1 of f) as integer
		set instState to item 2 of f
		set label to item 3 of f
		if instState is "waiting" then
			set label to label & " (waiting for it to be connected)"
		else if instState is "starting" then
			set label to label & " (starting)"
		else if instState is "stopped" then
			set label to label & " (stopped)"
		end if
		set mi to addItem(label, "toggle:", n, ready)
		-- A check while it's on; a dash while it's on but not streaming yet.
		if instState is "on" then
			mi's setState:(current application's NSControlStateValueOn)
			set anyOn to true
		else if instState is "waiting" or instState is "starting" then
			mi's setState:(current application's NSControlStateValueMixed)
			set anyOn to true
		end if
	end repeat
	set AppleScript's text item delimiters to saved
	theMenu's addItem:(current application's NSMenuItem's separatorItem())
	if not ready then addItem(busyText, missing value, missing value, false)
	addItem("Turn All On", "allOn:", missing value, ready)
	addItem("Turn All Off", "allOff:", missing value, ready)
	theMenu's addItem:(current application's NSMenuItem's separatorItem())
	addItem("Quit Frametop Streams", "quitApp:", missing value, true)
	statusItem's button()'s setAppearsDisabled:(not anyOn)
end refresh

on addItem(itemTitle, itemAction, itemTag, itemEnabled)
	set mi to current application's NSMenuItem's alloc()'s initWithTitle:itemTitle action:itemAction keyEquivalent:""
	if itemAction is not missing value then mi's setTarget:me
	if itemTag is not missing value then mi's setTag:itemTag
	mi's setEnabled:itemEnabled
	theMenu's addItem:mi
	return mi
end addItem

-- A display that's off or stopped turns on; one that's on, waiting or starting turns off.
on toggle:sender
	set n to (sender's tag()) as integer
	if ((sender's state()) as integer) = 0 then
		change("on " & n, "Turning a display on…")
	else
		change("off " & n, "Turning a display off…")
	end if
end toggle:

on allOn:sender
	change("on", "Turning every display on…")
end allOn:

on allOff:sender
	change("off", "Turning every display off…")
end allOff:

-- In the background: on waits up to 20 s for the instances to answer. Its output goes to the log.
on change(args, what)
	set busyText to what
	set logDir to do shell script "dirname " & quoted form of logFile
	set busyPid to (do shell script "mkdir -p " & quoted form of logDir & "; { date; " & tool & " " & args & "; } >> " & quoted form of logFile & " 2>&1 & echo $!") as integer
	refresh()
end change

on quitApp:sender
	theTimer's invalidate()
	tell me to quit
end quitApp:
