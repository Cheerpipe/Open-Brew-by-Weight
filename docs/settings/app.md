# App settings

When the Web UI runs as an installed app on a phone or tablet, Settings ends
with a **This app** group. Every choice there belongs to this one app on this
one device: the controller never sees it, and other phones, tablets, and
browsers keep their own. In a plain browser tab the group does not appear.

Like the [theme](theme.md), these choices live in the app's own storage.
Clearing the app's browser data returns them to their defaults, and a
[factory reset](factory-reset.md) does not touch them.

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
- **Hold the live view upright** keeps the live view portrait however you
  turn the phone. Android phones only.
- **Shot card colors** chooses how the Home shot card uses color; see
  [Shot card colors](theme.md#shot-card-colors) for what each mode does.

## Offline copy

An app installed over HTTPS (for example through the
[companion](../features/companion.md)) can keep a copy of your shots so it
opens Shots and History with the controller off. When such a copy exists, the
**This app** group shows it with the date it was kept since, and **Delete the
copy in this app** removes it. The copy comes back the next time you open
Shots or History while the controller is on.
