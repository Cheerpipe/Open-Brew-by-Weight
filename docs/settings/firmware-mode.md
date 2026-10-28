# Firmware mode

Master switch that turns every Open Brew by Weight brewing feature on or off
while leaving the controller in place.

## A new controller starts switched off

When the firmware is first installed, and after a
[factory reset](factory-reset.md), the switch is off: the machine works exactly
as it did before the controller was fitted, so someone who does not know it is
there can still pull a shot. The first-time setup page asks how you want to
brew. Choose **Stop shots by weight** and a starting recipe there, or switch the
firmware on later from Admin. A controller switched on later from Admin starts
with the **Manual** recipe; pick another one on the Home screen.

## When you would use it

- You want the machine to behave exactly as it did before the controller was
  installed, but you are not ready to disconnect the hardware.
- You are troubleshooting and want to rule the controller in or out quickly.
- You are lending the machine to someone who only wants the plain switch.

## What happens when you turn it off

Save the switch while the machine is idle: during a pour the controller
refuses the change and asks you to try again once the machine has finished.
Saving restarts the controller once. After the restart the controller becomes
transparent:

- The relay follows the physical paddle or switch one-to-one, the same way the
  machine's own wiring would. Nothing else touches the machine.
- Brew by weight, guards, presets, rinse, automatic tare, alerts, webhooks,
  the scale Bluetooth connection, time sync, and the Linea Micra cloud
  connection all stop working.
- The web interface shows only the **Admin** page. Every other tab is hidden
  and any address other than Admin's takes you there; the **Diagnostic**
  address is the one exception, because support may need its measurements.
  Inside Admin you keep Network, Power
  management, Frontend, Access, Restart, Firmware update, and
  Factory reset. The controls of disabled features are hidden with them:
  the Bluetooth section and its scan-mode options inside Power management,
  Date & time (clock sync and time zone), Webhooks and, on Linea Micra
  builds, Companion.
- Recovery mode, firmware updates over Wi-Fi (OTA), and the USB serial CLI
  keep working, so you can always reach the controller.

There is no time limit in compatibility mode: the relay keeps following the
paddle or switch even when it is held for more than 60 seconds. Releasing it
opens the relay. Turning the firmware back on restores its normal time limits.
If the paddle or switch is held during startup or after a safety stop, release
it before activating the machine again.
Automatic [Micra backflush](linea-micra.md#automatic-backflush) is available
only with the firmware enabled and WebSocket monitoring connected.

## Nothing is lost

The switch does not erase anything. Presets, guards, scale preferences,
machine integration, alerts, and every other setting stay saved exactly as
they were. When you turn the switch back on and the controller restarts, the
firmware works the way it did before you turned it off.

A [factory reset](factory-reset.md) erases your settings and leaves the switch
off, like a new controller.

## Where to find it

**Admin** (unlock with the device password) **→ Firmware → Enable Open Brew
by Weight**, or the brewing step of the
[first-time setup page](ap.md#first-connection) while the switch is off. The
change takes effect after **Save firmware settings** and the restart that
follows. On the setup page, the choice is saved in its first part (with your
password), before and independently of Wi-Fi, and applied on the restart
after the setup network closes or when you select **Finish and restart**. Unplug-level safety behavior does not change: the relay
still opens on power loss, and a hardware safety fault still stops the
machine in both modes.

| Setting | Default | Notes |
| --- | --- | --- |
| **Enable Open Brew by Weight** | Off on a new or factory-reset controller | Off = transparent compatibility mode. Saved settings are kept. |
