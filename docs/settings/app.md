# App settings

When the Web UI runs as an installed app on a phone or tablet, Settings ends
with a collapsible **This app** group, just below **Alerts**. Every choice
there belongs to this one app on this one device: the controller never sees it,
and other phones, tablets, and browsers keep their own. In a plain browser tab
the group does not appear.

Like the [theme](theme.md), these choices live in the app's own storage.
Clearing the app's browser data returns them to their defaults, and a
[factory reset](factory-reset.md) does not touch them.

- **Show splash screen** plays a short animation each time you open the app:
  the logo drops in, its switch clicks shut and a few drops fill a cup at the
  bottom of the screen while the app gets ready. As soon as it is ready, the
  coffee rises and the app appears. Tap anywhere to skip it. It is on until
  you turn it off, and it never plays in a browser tab or when the app
  reloads.
- **Keep the screen on while a shot runs** holds the screen awake from the
  start of an extraction until a minute after it ends, wherever in the app
  you are. The minute of grace gives you time to read the result without the
  screen locking. The live view keeps the screen on by itself, with or
  without this setting.
- **Open the live view when a shot starts** raises the full-screen timer by
  itself when a shot begins while you are on Home — start the shot at the
  machine and the timer appears on the counter.
- **Vibrate when a shot ends** gives a short buzz when the extraction
  finishes. Android phones only.
- **Hero card** picks which Home shot card this device shows — the recipe
  graph of **Technical** or the full-color **Colorful** card — overriding the
  machine's choice in Admin → Frontend for this device only. See
  [Hero card](theme.md#hero-card) for what each one shows.
- **Shot card colors** chooses how the Home shot card uses color; see
  [Shot card colors](theme.md#shot-card-colors) for what each mode does.

## Shots with the controller off

The app keeps no copy of your shots on the device. Opened through the
[companion](../features/companion.md#with-the-controller-off), it can show
the shots and activations the companion keeps while the controller is off,
as long as **Shot archive in the companion** is on in Admin → Companion.
Copies that earlier versions of the app kept on the device are deleted the
first time the new version opens.
