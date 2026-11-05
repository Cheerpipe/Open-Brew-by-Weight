# Notifications

Your phone can tell you how a shot went, even with the app closed and the screen
off: its time, its yield, and whether it ran fast, on recipe, or slow.

```text
Fast shot: 21.0 s
Yield: 36.0 g
```

Tap the notification to open **Shots** on that shot's card.

If you have a Linea Micra, it can also
[tell you when La Marzocco needs you to accept new terms](#la-marzocco-terms).

## What you need

- [Open Brew by Weight Companion](companion.md), paired with your controller and
  connected to the internet. The companion sends the notifications, so without
  it there are none. Everything else keeps working as usual.
- The Web UI installed as an app from the companion's address,
  `https://openbrewbyweight-secure.local`.
- On iPhone and iPad, iOS or iPadOS 16.4 or newer.

## Turn them on

Notifications are off until you turn them on, separately on each device.

1. Open the installed app.
2. Go to **Settings → This device** and turn on **Notify me when a shot ends**.
3. Allow notifications when your device asks.

The setting records what you want, so it stays on even when notifications can't
work yet. If something is in the way, a red note below the setting says what it is
and what to do. Once that's sorted, notifications start on their own the next time
you open the app or return to **Settings**.

To stop them, turn the setting off. Signing out on the device turns them off too.

## La Marzocco terms

From time to time La Marzocco publishes new terms, and its cloud stops working for
your account until someone accepts them. Your machine looks fine, but the controller
can no longer see it or turn it on, and nothing at the machine tells you why.

Turn on **Notify me when La Marzocco needs me to accept its terms** in
**Settings → This device** to hear about it, even with the app closed. It is a separate
choice from shot notifications, so you can have either one or both.

Tap the notification to open Home, where a card explains your options: the best one is
to accept in the La Marzocco app, and you can also accept right there on the card. See
[When La Marzocco updates its terms](../settings/linea-micra.md#when-la-marzocco-updates-its-terms).

You hear about it once each time terms are pending, and again if the controller restarts
before they are accepted. If your phone is off, the notification can still reach it
within a day; after that, the card on Home is still waiting for you.

## What to expect

- **After the shot, not during it.** While a shot runs, the controller keeps its
  radio for your scale, so the notification leaves once the shot and its drip
  delay are over and the shot is saved. If something keeps the machine busy, such
  as a paddle left on, it waits for that too.
- **Which shots notify.** Shots too short to count, or with no yield at all, do not
  notify, and neither does one that ended more than ten minutes before the controller
  could send it. If the scale was lost during the shot, you see the estimated yield.
- **Away from home.** Notifications still arrive over mobile data, with no extra
  setup. Tapping one needs your home network to show the shot, so away from home the
  app opens with a short message instead.
- **Colors.** Notifications are text only. iPhones and iPads always show the app's
  icon, so the result is written out.
- **Best effort.** A notification can arrive late or not at all. Never rely on one
  for anything that matters to your machine. It cannot start, stop, or change anything.

## Privacy

Your phone only accepts notifications through its maker's or browser's service:
Apple, Google, Mozilla, or Microsoft. You do not need an account for this. The message
is encrypted, so that service cannot read the time, yield, or result. It can see that
a message was sent, when, and from which internet address.

## Test and troubleshoot

**Diagnostic → Push** shows where a notification stopped. It appears when you open
the Web UI through the companion.

To test, choose **On recipe**, **Fast**, **Slow**, or **La Marzocco terms**. A sample goes to this device
only. Select **Wait 30 seconds** first if you want to close the app and lock the
screen before it arrives.

| What you see | What to do |
| --- | --- |
| A red note appears below the setting | It says why notifications may not arrive: usually the app was installed from another address, or the companion needs pairing or an update |
| **Permission** says `denied` | Allow notifications for this app in your system settings |
| **Last error** names a problem, and **Accepted** does not grow | Check that the companion can reach the internet |
| The push service answered `201`, and nothing arrived | The service accepted it, so check Focus or Do Not Disturb, and your notification settings |
| **A device wants notifications** says **No** | Open the app again on your device so it signs up again; the companion may have been reset |
| **Why the last shot was not sent** says `bad` or `stale` | The shot was too short or had no yield (`bad`), or ended too long ago (`stale`) |
