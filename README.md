# Lid Closing Mode Button

A Windhawk mod for Windows 11 that adds one button immediately to the left of
the system tray.

The button toggles the active power plan's lid-close action between:

- Sleep
- Do nothing

It updates both the plugged-in (AC) and battery (DC) values used by
`powercfg.cpl`. A moon icon means Sleep, and a blocked icon means Do nothing.

## Install

1. Open Windhawk.
2. Choose **Create a new mod**.
3. Replace the editor contents with
   [`lid-closing-modes.wh.cpp`](lid-closing-modes.wh.cpp).
4. Click **Compile Mod**, then enable it.

The mod periodically rereads the active power plan and restores its taskbar
button if Explorer rebuilds the Windows 11 XAML taskbar.
