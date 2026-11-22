# Mahlkönig grinder

A Mahlkönig grinder with Grind-by-Sync can learn from your shots: after each
espresso the controller tells it how long the shot really ran and how much it
weighed, and the grinder fine-tunes its grind for the next one. Because the
controller weighs every shot, the grinder learns from real results instead of
guesses.

> **Not tested with a real grinder yet.** This integration follows what is
> known about Grind-by-Sync, but nobody has confirmed it on a Mahlkönig
> grinder. Settings marks it **Untested** until it has been. If you try it,
> please tell us how it went (see [reporting a problem](#if-something-goes-wrong)).

## Turn it on

The integration is off until you turn it on in **Admin → Integrations →
Mahlkönig grinder** and save. Settings then shows a **Mahlkönig** section.
Turning it off again stops all talk with the grinder and hides the section;
your grinder setup is kept for next time.

## Which grinder you have

| Grinder | How it works | Open Brew by Weight Companion |
| --- | --- | --- |
| **E64 WS** | The grinder checks in with the controller by itself every 2 seconds. | Not needed: works well without it. |
| **E65W, E65T, E80W, E80T** | The grinder has to be asked after each grind whether you just ground for a shot. | **Strongly recommended.** |

With the **E65W, E65T, E80W or E80T**, the companion asks the grinder every 2
seconds, as Mahlkönig intended, so every shot reaches it. Without the
companion the controller asks every 15, 20 or 25 seconds (you choose; 20 is
the default), and a shot pulled soon after grinding may not sync. The
controller always asks once more as soon as a shot ends, which catches most
of those shots, but a long shot pulled right after grinding can still be
missed, because the grinder forgets an unclaimed grind after about a minute.
See [Open Brew by Weight Companion](../features/companion.md).

## Set up an E64 WS

1. In **Settings → Mahlkönig → Grinder**, choose **E64 WS**.
2. On the grinder, open **Connectivity → Machine To Machine** and turn on
   **Xenia**.
3. Open **Configuration → Edit Host** and type the address Settings shows in
   large print. Type the number, not a name.
4. Leave the grinder on. Settings moves on by itself once the grinder checks
   in.

Reserve that address in your router (a DHCP reservation) so it never changes;
if it changes, the grinder can no longer find the controller.

If your companion answers the grinder for the controller, Settings shows the
companion's address first and the controller's as **Can't connect?**: the
controller's Wi-Fi then stays free for your scale. The grinder only connects
to port 80, so a companion on another port can't do this, and Settings says
so.

## Set up an E65W, E65T, E80W or E80T

1. On the grinder, turn on **Local Wi-Fi** and join your home network.
2. Open **Machine To Machine → GbS Coffee Machines** and choose **Grouphead 1**.
3. Under **Configuration**, tap **Start Pairing**.
4. In **Settings → Mahlkönig → Grinder**, choose your model, enter the
   address the grinder shows on its Wi-Fi screen, and tap **Pair**.
5. The grinder lists **Open Brew by Weight** with a code. If it matches the
   code Settings shows, accept it on the grinder. The grinder may restart
   after you accept; that can take about two minutes, so keep the page open.

If the grinder says it is already paired, Settings asks for the **Pairing ID**
the grinder shows under **Machine To Machine**, so the controller can take
that pairing over.

**Check for a new grind** sets how often the controller asks the grinder when
the companion doesn't: every 15 seconds misses the fewest shots but keeps the
controller busiest, every 20 seconds is the recommended balance, and every 25
seconds is lightest for the controller, with more shots pulled soon after
grinding missed. While the companion talks to the grinder, it checks every 2
seconds and your choice waits for when the controller checks by itself.

## What the grinder learns

Open **Shot sync** to see the last shot and whether the grinder received it.

- Shots that reach their target yield, or the max shot time, are sent with
  their real time (until the pump stops, without the drip) and their final
  yield.
- Shots you stop by hand, channeling shots and shots that lose the scale are
  sent as **cancelled**, so the grinder doesn't learn from them.
- Rinses aren't shots and are never sent. A quick flush of the group after
  grinding (12 seconds or less) is skipped too: the grinder waits for the
  shot that follows.

The grinder aims for its own **Brew Time**. Set it to the middle of your
preset's shot time window (Settings shows the number), so the grinder and the
controller aim for the same shot.

After an E64 WS grinds for a shot, it waits for that shot and won't grind
again until it ends, for up to 3 minutes. If you change your mind, tap
**Release grinder** in **Shot sync**.

While the grinder corrects the grind from your last shot, Home pauses its own
**Grind hint**, **Dialling in** and **Checking your change** cards and shows
one line instead, so you don't adjust twice. **Shots vary** stays. The cards
come back by themselves after a shot the grinder didn't receive. See
[Fast and slow shots](../features/fast-and-slow-shots.md).

## If something goes wrong

| What Settings says | What to do |
| --- | --- |
| **Not seen** (E64 WS) | Check that the grinder is on, **Xenia** is on, and **Edit Host** shows the address Settings shows. |
| **Not answering** (E65/E80) | Check that the grinder is on and on your Wi-Fi. If its address changed, pair again with the new one. |
| **Pairing removed** | The grinder no longer accepts the controller, usually after **Clear Pairing** on the grinder. Tap **Pair again**. |
| **Grinder identity changed** | The grinder answered with another identity than when you paired it, which can happen after a reset or a firmware update. If it is your grinder, tap **Trust this grinder**; the pairing is kept. |

To report a problem, send the Web UI log (**Log**) and the Diagnostic export
(**Diagnostic → Export**); together they show what the controller and the
grinder said to each other. See the [FAQ](../FAQ.md#my-mahlkönig-grinder-does-not-sync).
