# Lid Closing Mode Button

A Windhawk mod for Windows 11 that adds one button immediately to the left of
the system tray.

The button toggles the active power plan's lid-close action between:

- Sleep
- Do nothing

It follows the current power source:

- While plugged in, it displays and changes only the plugged-in (AC) value.
- On battery, it displays and changes only the battery (DC) value.

Connecting or disconnecting the charger updates the button immediately. A moon
icon means Sleep, and a blocked icon means Do nothing.

The button mirrors the native Windows 11 system tray button style: the same
32-pixel layout width, inset background, one-pixel gradient hover border,
pressed state, corner radius, and 83 ms background transition. Native theme
resources cover light, dark, and high contrast modes automatically.

## Settings

**Separate plugged-in and battery modes** is enabled by default:

- Enabled: the button reads and changes only the current power source.
- Disabled: the button reads and changes AC and DC together.

If AC and DC already differ when linked mode is selected, the button shows a
question mark without changing either value. The next click explicitly sets
both to Sleep.

## Install

1. Open Windhawk.
2. Choose **Create a new mod**.
3. Replace the editor contents with
   [`lid-closing-modes.wh.cpp`](lid-closing-modes.wh.cpp).
4. Click **Compile Mod**, then enable it.

The mod doesn't poll. It subscribes to Windows notifications for the lid
action, active power plan, and AC/DC power source. It also listens for
system-tray XAML reconstruction events so the button is restored when Explorer
rebuilds the Windows 11 taskbar.
