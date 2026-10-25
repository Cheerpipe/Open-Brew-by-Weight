# Emergency Recovery with Paddle

Restore access using the physical activator when Web UI, Wi-Fi, BLE, and USB
are unavailable. This also works on momentary builds: hold the button at
power-on, then release/press for each OFF→ON cycle. The scale is not needed.

The controller keeps its relay open during the 30-second wait and during
recovery. On momentary machines, an open relay is not proof that a machine
already running has stopped. Perform recovery with the machine idle.

## Choosing the Procedure

| Procedure | Complete OFF→ON cycles | Erases | Preserves |
| --- | ---: | --- | --- |
| Recover access | 3 | Wi-Fi, static IP, last-known-good network, device password | Machine settings, recipes, calibration, scales, history |
| Factory reset | 5 | All saved configuration, calibration, scales, history and last shot | Firmware |

Both restore the [factory AP access](settings/ap.md#first-connection) and turn
off the [Web UI password](settings/web-access.md), so the Web UI opens without
signing in.
Factory reset cannot be undone.

## Before You Begin

- Start from a **power-on**, with the paddle ON or momentary button held.
  Keep it ON and still: the controller waits **30 seconds** before recovery
  begins, sounding the same double beep used to ask for the paddle back
  after a shot, once every 5 seconds. This hold does not count as a cycle.
- Releasing the activator during those 30 seconds cancels recovery for this
  boot. Power the controller off and on to try again.
- Once announced, recovery listens for 60 s. Complete the movements within
  5 s, then hold ON without movement for 3 s to confirm.
- Beeps require a compiled and connected buzzer. A silent build uses the same
  counts and timing; absence of sound is not evidence that nothing happened.

## Recover Wi-Fi, AP, and Password

1. Power off the controller; move the paddle ON or hold the button.
2. Power on. Keep the activator ON and still through the 30-second wait —
   six double beeps, one every 5 s. A 1.5 s continuous beep then announces
   recovery on buzzer builds.
3. Within the recovery window, perform three cycles in less than 5 s:
   `OFF → ON → OFF → ON → OFF → ON`.
4. Hold the final ON for 3 s. Three short beeps indicate successful recovery.
5. Return the activator OFF and allow the restart. Connect using the
   [AP instructions](settings/ap.md#first-connection).

Example timing: first OFF at 0 s, successive transitions every 0.5 s, final
ON at 2.5 s, confirmation at 5.5 s.

## Perform a Factory Reset

Follow the access-recovery steps, but perform **five** cycles before the
3-second final hold:

`OFF → ON → OFF → ON → OFF → ON → OFF → ON → OFF → ON`

For example, transitions every 0.4 s reach the fifth ON at 3.6 s; confirmation
finishes at 6.6 s. Five short beeps indicate success. Return OFF, wait for
restart, then repeat [first setup](GETTING_STARTED.md).

Do not pause for 3 s after the third ON while intending five cycles: that
would confirm access recovery before the longer gesture is complete.

## Cancel Without Erasing Data

**Before confirmation**, move the activator OFF and keep it OFF until the
60-second recovery window expires. OFF prevents a three/five-cycle candidate
from confirming. Do not simply stop moving while ON after three or five cycles:
that is the confirmation gesture.

You can also remove controller power before confirmation. Once the operation
has begun, power loss is not cancellation: a durable recovery intent makes
the next boot resume it. After expiry, normal startup continues with the relay
open until the normal start conditions are met.

## Common Mistakes

| Symptom | Check |
| --- | --- |
| Normal boot instead of recovery | Power-on must begin with the activator ON; pressing later does not enter recovery. |
| Recovery never started | The activator must stay ON and still for the whole 30-second wait; any release or paddle movement cancels it for this boot. Power off and on again. |
| Gesture not accepted | Four cycles do nothing; movements slower than 5 s invalidate the attempt. Retry within the 60 s window. |
| Access reset instead of factory reset | A 3 s pause after the third cycle confirms the shorter operation. |
| No beeps | The buzzer may be absent; use the counts and timings above. |
| Operation interrupted by power loss | Pending recovery resumes at the next boot. |

## If It Does Not Restart or AP Does Not Appear

Wait at least 20 s, return the activator OFF, then power the controller on
again. A successful access/factory reset clears saved Wi-Fi, so the AP should
be available at startup; its [idle shutdown](settings/ap.md#idle-shutdown)
still applies.

A long-short-long sound means storage could not complete the operation. The
firmware leaves recovery and continues startup; access depends on the settings
it could recover. If the operation keeps crashing the controller instead, it
gives up after three failed restarts in a row and continues startup the same
way; the Web UI log shows `recovery=fail`. If the AP is still unavailable, use
[USB commands](SERIAL_CLI.md) or [USB firmware recovery](BUILD.md#6-flash-usb).

Implementation: [recovery state machine](STATE_MACHINES.md#12-recovery-gesture).
Bench verification: [manual test plan](MANUAL_TEST_PLAN.md).
