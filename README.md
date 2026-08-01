# Lid Closing Mode Button

A Windhawk mod for Windows 11 that adds one button immediately to the left of
the system tray.

The button cycles the lid-close action through the actions enabled in the mod
settings:

- Sleep
- Do nothing
- Shut down (disabled by default)

It follows the current power source:

- While plugged in, it displays and changes only the plugged-in (AC) value.
- On battery, it displays and changes only the battery (DC) value.

Connecting or disconnecting the charger updates the button immediately. A moon
icon means Sleep, a blocked icon means Do nothing, and a power icon means Shut
down.

Changes are written to every power plan, matching the global lid settings shown
by `powercfg.cpl`. The active plan is then reapplied so the new behavior takes
effect immediately.

The button mirrors the native Windows 11 system tray button style: the same
32-pixel layout width, inset background, one-pixel gradient hover border,
pressed state, corner radius, and 83 ms background transition. Native theme
colors are matched for light, dark, and high contrast modes.

## Settings

**Separate plugged-in and battery modes** is enabled by default:

- Enabled: the button reads and changes only the current power source.
- Disabled: the button reads and changes AC and DC together.

If AC and DC already differ when linked mode is selected, the button shows a
question mark without changing either value. The next click sets both to the
first enabled action in the cycle.

The **Cycle** group has one toggle for each supported action:

- **Include Sleep**: enabled by default.
- **Include Do nothing**: enabled by default.
- **Include Shut down**: disabled by default.

If the current action is not enabled, the next click selects the first enabled
action. If no other action is enabled, clicking the button does nothing.

### Closed-lid safety

Right-click the taskbar button to configure independent **Plugged in** and
**Battery** safety rules. A rule applies only while its power source is active,
the lid is closed, and that source's lid action is **Do nothing**.

Each rule has an explicit **Safety action** setting: **Sleep** or **Shut down**.
The plugged-in rule runs it after a configurable delay.

The battery rule has a **Trigger** setting. Choose either **After a delay** or
**At battery level**, then configure the corresponding minutes or remaining
percentage. The unselected condition is ignored. Changing power source while
the lid is closed cancels the previous source's timer and starts the new
source's rule.

Safety rules are disabled by default.

The configuration flyout is created only when it is opened. Its XAML controls
are released when it closes; only the rule values used by the scheduler remain.

## Install

1. Open Windhawk.
2. Choose **Create a new mod**.
3. Replace the editor contents with
   [`lid-closing-modes.wh.cpp`](lid-closing-modes.wh.cpp).
4. Click **Compile Mod**, then enable it.

The mod doesn't poll. It subscribes to Windows notifications for the lid
action, lid state, active power plan, AC/DC power source, and battery
percentage. Closed-lid delays use a one-shot timer. The mod also listens for
system-tray XAML reconstruction events so the button is restored when Explorer
rebuilds the Windows 11 taskbar.
