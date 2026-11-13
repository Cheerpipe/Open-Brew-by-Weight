# Machine control

**Admin → Machine control** decides whether your machine can be switched on
and off from the Web UI. It appears only on firmware built for a machine the
controller can switch on and off, which today means the La Marzocco Linea
Micra through its [cloud connection](linea-micra.md). It is off on a new
controller, after a factory reset, and after updating from firmware that did
not have it.

## Allow turning the machine on and off

1. Open **Admin**, unlock it with the device password, and open **Machine
   control**.
2. Switch on **Allow turning the Micra on and off**.
3. Tap **Save machine control settings**.

Like everything else in Admin, nothing changes until you save; **Revert**
puts the switch back.

Home then shows a round power button next to the machine's name, as long as
the La Marzocco account is connected and **Show power state** is on in
Settings, because the button needs to know whether the machine is on.

## Use the power button

Press and hold the button for about a second. A ring fills around it as you
hold: yellow when you are turning the machine on, red when you are turning it
off. Let go before the ring closes and nothing happens, so a quick tap or a
brush of the screen never switches the machine. With a keyboard, press Enter
or Space once to arm the button and again within three seconds to act.

The button's colour tells you where the machine is:

| Button | Machine | Holding it |
| --- | --- | --- |
| Outline only | Off (standby) | Turns it on |
| Yellow, with a filling ring | On and heating up | Turns it off |
| Red | On | Turns it off |
| Pulsing yellow outline | Waiting for the machine to confirm | Nothing until it answers |
| Dimmed | Not available right now | Nothing; see below for why |

While the machine heats up, the ring around the yellow button fills from the
moment the controller saw the warm-up start until La Marzocco's estimate of
when it will be ready. If the controller restarts during a warm-up, the ring
starts from that point instead of guessing. A new estimate from the machine
slows the ring down; it never jumps back.

The button is dimmed during a shot, a rinse, a backflush or any other active
cycle, while La Marzocco refuses the account or the machine's state is
unknown, and while Admin is locked. Turning the machine on or off always
needs an unlocked Admin, so a visitor who can only see Home cannot do it.

## What stays the same

- **Turn the Micra on with the scale** and **Standby when the scale turns
  off** in **Settings → La Marzocco Cloud → Machine power** keep working
  exactly as before, whether this switch is on or off. They have their own
  options.
- With the switch off, the controller refuses every request to turn the
  machine on or off, whichever app, script or device sends it, including Home
  Assistant and the Companion. It never forwards such a request to La
  Marzocco.
- The machine is never switched on or off during a shot or any other active
  cycle.
- The Web UI log records every request the controller accepts, and the ones
  it refuses with the reason.

## Know the limits

- This switch protects this controller only. La Marzocco's own app, and
  anything else signed in to your La Marzocco account, can still switch the
  machine.
- Anyone who can unlock Admin can turn the switch on. If anyone on your
  network can open the Web UI, protect it with a password in
  [Web UI access](web-access.md) before you allow machine control.
- Switching the machine on remotely means it heats up with nobody next to it.
  Turn this on only if you accept that responsibility.
