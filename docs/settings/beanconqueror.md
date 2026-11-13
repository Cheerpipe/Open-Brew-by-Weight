# Beanconqueror

[Beanconqueror](https://beanconqueror.com) can import a shot straight from a
connected espresso machine. Open Brew by Weight can play that machine:
Beanconqueror then reads your shots from the controller and fills in a new
brew with the curve, the time, the yield and more, so you only add the bean.

You set it up in **Settings → Coffee apps → Beanconqueror**.

## Choose how Beanconqueror sees the controller

| Option | Beanconqueror | What a brew gets |
| --- | --- | --- |
| **Off** (default) | — | Nothing: Beanconqueror can't read your shots. |
| **As a Gaggiuino** | 8.2 or later | Weight curve and flow, time, yield, the preset's target temperature, and the preset's name. |
| **As a GaggiMate** | 8.8 or later | All of the above, plus your dose and your rating, and the shot's exact time. |

Choose **As a GaggiMate** if your Beanconqueror is 8.8 or later: it is the more
complete of the two. **As a Gaggiuino** works with older versions.

While an option is on, the section shows a preview of your last shot as
Beanconqueror will receive it: what it fills in for you, what it works out
itself (the first drip, from its own threshold), and what it does not get.
The preview loads when you open the row, waits while a shot is brewing, and
then shows the new shot.

Some things are never sent: the controller has no pressure sensor, and the
bean, the grind and your notes are yours to pick in Beanconqueror. With
**As a Gaggiuino**, Beanconqueror times the shot to its last weight reading,
so its time includes the drip wait.

## Connect Beanconqueror

The section shows the address to use and the same steps:

1. In Beanconqueror, open **Preparations** and add an espresso preparation, or
   use yours.
2. Open its **⋯** menu, choose **Device connection** and pick **Gaggiuino** or
   **GaggiMate** as the machine, matching the option you chose.
3. Paste the address the section shows as the **Url**, for example
   `http://openbrew.local/beanconqueror`, and tap the check button.
4. After a shot, start a brew with that preparation and choose **Import a shot
   from Gaggiuino** (or **from GaggiMate**).

Use the address exactly as shown, with `http://`. If Beanconqueror can't
connect to a `.local` name (some Android phones don't resolve them), use the
address with the IP number that the section shows under it.

Beanconqueror only reads when you import, never during a shot. If you import
while a shot is brewing, it can't connect; try again once the shot has
ended. With **As a GaggiMate**, leave Beanconqueror's option to write notes
back to the machine off: the controller doesn't take notes from apps.

Every shot Beanconqueror imports shows up in the Web UI log, for example
`Beanconqueror: shot 42 sent as a Gaggiuino shot`, so you can check which
shots it took. Shots served by a companion show up in the companion's log.

## No password, by design

A Gaggiuino or a GaggiMate lets any app on your network read its shots, and
Beanconqueror expects the same: it can't sign in. So while an option is on,
anyone on your home network can read your shot history at that address, even
with the Web UI login on. It's read-only: nothing at that address can change
a setting or start the machine. Turn the option off when you don't use it.

## With a companion

If you use the [Open Brew by Weight Companion](../features/companion.md), it
can serve your shots to Beanconqueror from its own copy, even while the
machine is off. Turn on **Beanconqueror through the companion** in
**Admin → Companion** (it is on by default and needs **Shot archive in the
companion**). Open the Web UI through the companion, and the section shows
the companion's address. The controller's own address keeps working too.

Turning Beanconqueror off in Settings stops both.
