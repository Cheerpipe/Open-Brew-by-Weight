# Factory reset

A factory reset also restores the La Marzocco Cloud options to their
recommended defaults. Disconnecting only the account keeps your option choices.
During an
unconfirmed firmware trial, settings saves and resets wait for confirmation
so a rollback can recover the prior settings.

Erases saved configuration and returns the controller to first-boot
credentials. Firmware on the flash is not erased.

## What is erased

Wi-Fi (STA and last-known-good network), workflow settings, presets,
calibration (including the learned offset), preferred scale,
shot history, activation history, and saved crash records on 16 MB controllers.
The saved time zone and its first-detection record are also erased; the next
authorized Web UI setup can detect a zone again.
On Linea Micra firmware it also erases
the cloud account credentials, installation key, selected machine, and session.
Presets return to the five factory presets (Double · Adaptive, Single · Adaptive,
Double · Fixed, Single · Fixed and Manual) with Manual active. Its three
integration options return to ON, and factory preset boiler targets return to
93.0 °C. ESP32 power management, Wi-Fi sleep, and the BLE scan mode return to
their factory values: ESP32 power management on, Wi-Fi sleep on, and Balanced
scan mode. The machine-aware scale search options return to off. The device
password returns to **`ineedacoffee`** and the device name to
**`openbrewbyweight`**. The [Web UI password](web-access.md) turns off, every
remembered device and Home Assistant's access are forgotten, and Home shows
the access note again. The [firmware mode](firmware-mode.md) switch is
left off, so the machine works as usual until you choose how to brew on the
setup page or in Admin. The device then restarts.

## Ways to run it

Use Web/USB reset while the machine is idle, the activator is OFF, the relay
is open and the controller is Ready. The physical recovery gesture runs before
Wi-Fi and keeps the relay open. Check actual idle state on momentary machines.

| Path | How | Notes |
| --- | --- | --- |
| **Web UI** | Admin → unlock with the device password → Factory reset, confirm `ERASE_ALL_SETTINGS` | Device password required. Device restarts. |
| **USB serial** | `FACTORY_RESET` | Same erase. See [USB serial CLI](../SERIAL_CLI.md). |
| **Paddle gesture** | Power on with paddle ON, hold it still through the 30 s wait, then five `OFF→ON` cycles | Last-resort. See [Emergency recovery](../EMERGENCY_RECOVERY.md). |

A shorter paddle gesture (three cycles) restores the device password / forgets STA
**without** erasing recipes, calibration, or history. It also turns off the
[Web UI password](web-access.md).

## After reset

Factory Single/Double use adaptive EWMA with offsets at their 0.50/0.50 g
baselines, current/base α=0.30, initial provenance and empty candidate
evidence. In Settings, offset-only reset restores the saved baseline; **Reset
EWMA learning** additionally resets the gain to the preset's saved alpha
baseline and clears evidence. Neither is a device factory reset. See
[BBW](../features/brew-by-weight.md#cutoff-behavior-and-learning).

**Tare when you place a cup** and **Retare when adding or removing an
accessory** return to ON. See [Tare](tare.md) for the
placement trigger and how a tared cup at zero satisfies the cup-start guard.

Follow [first setup](../GETTING_STARTED.md) using the
[factory AP connection](ap.md#first-connection). Which scale returns to **My scale first**, with
**First detected** shown until the first compatible scale connects
successfully. See [Scales](scales.md), [AP](ap.md), and the
[README first connection](../../README.md#first-connection).

## If reset fails

Before changing settings, factory reset writes a durable recovery intent. If
that write fails specifically because NVS is full, it removes only the stored
last-shot aggregates, retries the intent once, and then performs the full reset
(which itself clears the flash-backed shot and activation histories).
Timeouts, corruption, and other storage errors do not trigger this space
recovery. If the intent still cannot be saved, settings remain unchanged and
the controller does not restart. Check **Diagnostic → NVS** for capacity and
the last storage error.

Once the controller restarts, it finishes the reset before anything else
starts. If it cannot finish, it plays a long-short-long sound and starts
normally, so the Web UI and its access point stay reachable. If the reset
keeps crashing the controller instead, after three failed restarts in a row it
only restores the factory Wi-Fi and password, so you can reach it through its
access point again; shots and other data may be partly erased. Only if that
also keeps failing does it start with its previous settings. Either way the
Web UI log shows `recovery=fail`; run the factory reset again once you are
connected. A power cut during the reset is not a failure: the next start
simply finishes it.

Related: [Wi-Fi](wifi.md), [Emergency recovery](../EMERGENCY_RECOVERY.md),
[Linea Micra](linea-micra.md), [FAQ](../FAQ.md).
