# Settings for this device

Settings ends with a collapsible **This device** group, just below **Alerts**.
Every choice there belongs to this one browser or installed app on this one
device: the controller never sees it, and other phones, tablets, and browsers
keep their own. Tabs and windows of the same browser share the same choices.
In a browser tab, the group leaves out the settings marked *installed app only*.

The [preset lock](../features/presets.md#lock-the-preset-list-on-home) on
Home is kept the same way, per device, though you set it on Home itself.

Like the [theme](theme.md), these choices live in the browser's own storage.
Clearing its data returns them to their defaults, and a
[factory reset](factory-reset.md) does not touch them.

- **Show splash screen** (*installed app only*) plays a short animation each
  time you open the app: the logo drops in, its switch clicks shut and a few drops fill a cup at the
  bottom of the screen while the app gets ready. As soon as it is ready, the
  coffee rises and the app appears. Tap anywhere to skip it. It is off until
  you turn it on, and it never plays when the app reloads.
- **Keep the screen on while a shot runs** (*installed app only*) holds the
  screen awake from the start of an extraction until a minute after it ends, wherever in the app
  you are. The minute of grace gives you time to read the result without the
  screen locking. The live view keeps the screen on by itself, with or
  without this setting.
- **Open the live view when a shot starts** (*installed app only*) raises the
  full-screen timer by itself when a shot begins while you are on Home — start the shot at the
  machine and the timer appears on the counter.
- **Keep the live view in landscape** (*installed app only*) always lays the
  live view out sideways, with the chart across the long side of the screen,
  even with the phone upright or its rotation locked. Turn the phone on its
  side to read it. It is off until you turn it on; without it, the live view
  follows the way you hold the phone.
- **Notify me when a shot ends** (*installed app only*) shows the shot's time,
  yield, and result on this device, even with the app closed. It needs the
  companion and is off until you turn it on; unlike the rest of this group, the
  companion keeps this device's choice so it can send them. If notifications
  can't work on this device, the setting stays on and a red note below it says
  why. See [Notifications](../features/notifications.md).
- **Notify me when La Marzocco needs me to accept its terms** (*installed app
  only*, Linea Micra) tells this device when La Marzocco's cloud stops working
  for your account until new terms are accepted. Tap it to open Home and see
  what to do. It needs the companion too and is off until you turn it on. See
  [Notifications](../features/notifications.md#la-marzocco-terms).
- **Vibrate when a shot ends** gives a short buzz when the extraction
  finishes. Android phones only.
- **Show battery percentage** adds your scale's charge as a number next to
  the battery icon at the top of the screen. It is off until you turn it on.
  Without it, tap the Bluetooth icon to read the percentage.
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
