# Scheduled restart

**Admin → Scheduled restart** lets the controller restart itself once a day
at a time you choose, so it starts each day fresh without you pressing
**Restart controller**. It is off on a new installation and after a factory
reset.

## Setting it up

1. Make sure the controller knows the time: it needs Wi-Fi with internet
   access, **Automatic date & time** on, and a timezone chosen under
   **Admin → Date & time** (see [Wi-Fi](wifi.md)).
2. Open **Admin → Scheduled restart** and turn on **Restart every day**.
3. Set **Restart time (24-hour)** with the hour (0–23) and minute (0–59)
   fields. Their − and + keys wrap around, so + on 59 goes to 00 and − on
   00 goes back to 59 (23 and 00 for the hour). The factory time is 12:00.
   Choose a time when you rarely brew.
4. Press **Save scheduled restart**. Like every Admin group, nothing changes
   until you save; the arrow button beside it puts the saved values back.

The time is your local time in the controller's timezone, and it follows
daylight saving changes on its own. On the day clocks jump forward, a time
that does not exist (for example 02:30) restarts at the first minute after the
jump. On the day clocks fall back, a repeated time restarts only the first
time it comes around.

## What to expect

- **It never interrupts you.** If a shot is running, or the machine circuit is
  closed or the paddle is held, the restart waits until five minutes after it
  ends. A firmware update that is uploading, staged, or about to restart also
  holds it, and so does a freshly installed firmware that has not finished
  its first start.
- **It gives up rather than waiting all day.** If those five quiet minutes
  are not reached within 30 minutes of the restart time, that day's restart
  is skipped and the next one is planned for tomorrow.
- **It only restarts once per day.** A controller that was switched on less
  than 30 minutes before the restart time skips that day, since it has just
  started anyway. Turning the switch on, changing the time, or the clock
  arriving late never triggers a restart for a time that has already passed.
- **It needs a working clock.** Without a timezone, or before the
  controller has received the internet clock, nothing restarts. If the clock
  has not been refreshed for more than a day, the restart still happens on
  the last known time.
- **Your settings stay.** It is the same restart as **Restart controller**:
  saved settings, presets, and history are kept, and the restart history
  lists it as a software restart.

## Checking on it

The line under the time fields shows what the controller plans to do after
you save:

| Message | Meaning |
| --- | --- |
| Next restart at 12:00. | Ready; it will restart at that time. |
| The 12:00 restart is waiting until 5 minutes after the last shot. | A shot, or the five quiet minutes after it, is holding the restart. |
| The 12:00 restart is waiting for the firmware update to finish. | An update is uploading, staged, or not yet confirmed. |
| The 12:00 restart is waiting for the controller to finish another task. | Another restart or a maintenance task is in progress. |
| Waiting for the internet clock. | The controller has no time yet. Check Wi-Fi and **Automatic date & time**. |
| Choose a timezone under Date & time to use this. | No timezone is saved. |

Every restart and every skipped day is written to **Diagnostic → Audit log**
as *scheduled restart* or *scheduled restart skipped*, with the planned time
and the reason (for example `shot`, `update`, `recent_boot`, or `no_clock`).
These entries survive the restart itself. When **Diagnostic → Log** keeps
INFO messages, it also shows when the next restart was planned and each time
it was held back.
