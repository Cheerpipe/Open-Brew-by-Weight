# OTA

Loading settings during a trial boot never writes them back. Settings saves
and resets wait until the image is confirmed, retaining the previous durable
record for rollback. Pending edits remain queued; confirmation does not depend
on saving those edits. The existing Wi-Fi update, rollback and USB recovery
paths remain available. Downgrade restrictions above still apply. If a new
version finds a saved setting it no longer accepts, only that group of
settings returns to its factory values; Wi-Fi, the device password, and every
other group are kept, and the Web UI log notes the repair.

Firmware can be updated over Wi-Fi without opening the case or using USB.
Upload a built image with the project scripts while the controller is on
your network.

## Requirements

- Authenticate each CLI run using the hidden password prompt or
  `OPENBREWBYWEIGHT_DEVICE_PASSWORD`. Scripts never store it. Do not put secrets
  in command arguments or shared logs.
- From **Admin → Firmware update**, unlock administration first. The Web UI
  does not ask for the device password again.
- Image must match the board architecture, hardware-profile compatibility ID,
  and machine-profile compatibility ID, and must not be older than the running
  version (downgrades are refused).

Compatibility mode does not restrict updates: with the [firmware mode
switch](../settings/firmware-mode.md) off, the Admin **Firmware update**
section and the whole OTA pipeline keep working.

## While an update runs

The Web UI stays live while the image uploads, verifies and flashes, and Home
Assistant and webhooks keep working. The update steps themselves run one at a
time, in the order they arrive. If the controller's web server is restarting,
a step answers `OTA_SERVER_STOPPING`; send it again once the controller is
back. The La Marzocco cloud connection also stays up, so power state and
machine commands keep working until the controller restarts into the new
firmware.

After the restart, an open Web UI page notices the new version and reloads
itself once. The first time you open the Web UI afterwards it may also reload
once; after that, the start page and the page you opened load the new version
directly. Other pages opened straight from a bookmark may still reload once
on their first visit.

## Safety behavior

The shot always has priority. The paddle is never blocked by an update.

- **No upload or flash during a shot.** Starting an update while the machine is pouring is refused (`CONFIG_LOCKED_DURING_ACTIVE_CYCLE`) so Wi-Fi does not compete with the scale.
- **Paddle during transfer aborts the upload** (`SAFETY_LOST`). The spare slot is discarded; the running firmware is untouched. A verified staged image is kept if you pull a shot *after* verify — flash waits until idle.
- **Discard waits for Ready.** **Discard** and `--discard-ota-session` are refused during a shot (`CONFIG_LOCKED_DURING_ACTIVE_CYCLE`), like the other update steps.
- **An abandoned update clears itself.** If the browser tab closes or Wi-Fi drops mid-upload and nothing resumes it, the partial update is dropped after 15 minutes without activity. Until then the controller keeps the radio at full power; afterwards it returns to normal on its own.
- **No planned restart during a shot.** Flash, Admin Restart, and serial `REBOOT` wait until the pour ends and the circuit is open. They do not open the relay to make way for the reset.
- Confirming or rolling back a `PENDING_VERIFY` image writes otadata (flash cache off). That write is deferred while a shot is pouring or GATT is up.

Updates use a dual slot. After reboot, the new image boots as
`PENDING_VERIFY` and is confirmed when HTTP is available and boot uptime is
at least 15 s. Opening a browser is not required. A connected scale can defer
confirmation until 180 s; an active cycle or closed relay always defers it.
A second OTA while verification is still pending is refused
(`PENDING_VERIFY`).

If HTTP never comes up, the controller waits up to 180 s, then:

- **Previous slot bootable:** the running image is marked invalid and
  the bootloader rolls back on restart — no USB required. That restart
  still waits if a shot is in progress.
- **No bootable previous slot:** the running image is confirmed so the
  machine is not left without an application. Recover over USB; see
  [Emergency recovery](../EMERGENCY_RECOVERY.md).

Watchdog and panic still open the circuit and reset immediately: a hung
firmware cannot wait for the shot to finish.

Flashing leaves Wi-Fi credentials, presets, and calibration untouched. A firmware
version that changes history formats may start incompatible records empty.
An update over Wi-Fi never changes how the controller's storage is laid out;
a controller still running a development build from before v1 needs the
[clean USB installation](../BUILD.md#clean-installation) once.

## How to run

OTA can be performed from the Web UI or from the command line. In both cases,
the controller and the computer must be on the same Wi-Fi network (or the
controller's SoftAP network).

### Web UI

Open **Admin → Firmware update**, unlock Admin with the device password, then
select the firmware `.bin`. Choose **Upload and verify** and, once verification
succeeds, select **Flash and restart**. The controller IP is shown on the
Admin page; when using its SoftAP it is always `192.168.4.1`.

### Command line (CLI)

Use the project scripts from the repository root. The CLI authenticates with
the device password; enter it at the hidden prompt or supply it through your
environment's secret mechanism. The password is never saved by the scripts.

The CLI computes the image SHA-256 before the transfer, creates a named OTA
session, and sends 64 KiB ranges. If Wi-Fi drops, it queries the confirmed
offset and repeats only the unconfirmed range; it never resends the complete
image. A later CLI invocation queries the controller first and adopts its
`transferId` when SHA-256, size, architecture, hardware, machine, and version
match. The device
keeps a checksummed, double-record journal every 512 KiB so the same transfer
can continue after a client or controller restart. A TCP cut keeps complete
4 KiB sectors; reboot can retreat to the last durable checkpoint and resend
its tail. Staged state is RAM-only: reboot after staging requires a new upload.
A different build is never overwritten silently: the CLI explains what the
slot holds and asks whether to discard it — Y is the default, so pressing
Enter replaces it and continues. `--discard-ota-session` answers that question
in advance, and `--yes` assumes yes for unattended runs.

Success after reboot requires a changed `bootId`, the expected running image
digest and `confirmed: true`. Matching version strings alone are insufficient,
including reinstalling the same ROM. The Web retains the expected identity
across reloads and continues checking when Admin is unlocked again; it stores
no password. Older firmware without the evidence fields is reported as
unverified rather than successfully updated.

Session failures report the transport result, HTTP status, stable server error
code, and safe message. A controller with the immediately preceding resumable
schema can receive this upgrade. A controller that only exposes the older
single-POST protocol requires one USB update; the CLI never falls back to that
protocol silently.

`--no-check` is intentionally unavailable for resumable OTA because an image
without a verified SHA-256, architecture, hardware, machine, and version cannot
be safely matched to a staged slot or committed. Legacy arch-only images do
not match named profiles; install a current image over USB when crossing that
identity boundary.

The Web UI reads and hashes the image incrementally. It scans the whole file
for the identity (the linker may place it at any offset), validates the ESP32-S3
header and appended image checksum, and then resumes only a matching remote
session. Each range is checked against the hashes captured during that scan
before it is sent: if the file changes on disk while the upload runs —
typically because another build rewrote it — the upload stops with an error
instead of sending a mix of two builds. Selecting a different file never
discards the existing session.

From the repository root, build and upload one exact profile pair:

```sh
./scripts/dev build ota --confirm \
  --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra --host 192.168.1.50
```

Enter the password when prompted, verify the reported image, then confirm
commit. Use `--host 192.168.4.1` when connected to the controller's AP.
Use the exact same profile pair that produced the image.

For an image already obtained elsewhere, add
`--image /path/to/openbrewbyweight.bin`; you do not need to compile it again.
The local image identity must match the selected board.

OTA commit consent and post-boot verification are independent:

| Options | Commit behavior | Completion behavior |
| --- | --- | --- |
| neither | Ask in the terminal. | Return after commit is accepted. |
| `--yes` | Assume yes for every prompt: commit without asking, and discard a different remote image automatically. | Return after commit is accepted. |
| `--wait-for-confirmation` | Ask in the terminal. | Verify the expected new boot and confirmed image. |
| both | Commit without asking. | Verify the expected new boot and confirmed image. |

In unattended automation, provide `OPENBREWBYWEIGHT_DEVICE_PASSWORD` through a
secret environment and pass both `--yes` and `--wait-for-confirmation` when a
confirmed boot is required:

```sh
OPENBREWBYWEIGHT_DEVICE_PASSWORD="$DEVICE_SECRET" \
  ./scripts/dev ota --confirm \
    --hardware esp32-s3-relay-x1-speaker \
    --machine la-marzocco-linea-micra \
    --host 192.168.1.50 \
    --yes --wait-for-confirmation
```

`--confirm` authorizes the facade's hardware action; it does not imply commit
consent or verification. A non-interactive OTA without `--yes` fails before
upload. The removed `--force` spelling is rejected with migration guidance.

When USB is connected too, append `monitor` and provide its local port. The
network host and serial endpoint are separate:

```sh
./scripts/dev build ota monitor --confirm \
  --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra \
  --host 192.168.1.50 \
  --port /dev/cu.usbmodem2101 --speed 115200 \
  --yes --wait-for-confirmation
```

Build and flash flow: [Build environment](../BUILD.md). Script flags and
CLI reference: [Build scripts](../SCRIPTS.md).

Related: [Wi-Fi](../settings/wifi.md), [AP](../settings/ap.md),
[Emergency recovery](../EMERGENCY_RECOVERY.md).

## Session-start troubleshooting

1. Read the reported HTTP status, `error`, and `message`; do not retry blindly.
2. For `PENDING_VERIFY`, inspect `confirmBlockReason`, `confirmAttempts`,
   `confirmLastError`, and `bootState` in OTA status. `WAIT_UPTIME`, `WAIT_HTTP`,
   `WAIT_BLE`, `WAIT_CYCLE`, and `WAIT_RELAY` identify the unmet gate.
   `FLASH_BUSY`, `STATE_ERROR`, `CONFIRM_ERROR`, and `ROLLBACK_ERROR` preserve
   the actual error and retry at one-second intervals. If it persists beyond
   180 s at idle, save the diagnostic export rather than repeatedly uploading.
3. For `CONFIG_LOCKED_DURING_ACTIVE_CYCLE`, stop the cycle and wait for Ready.
4. For a matching partial image, select the same file or run the same CLI
   command; it resumes automatically from `nextOffset`.
5. For a different partial image, the CLI asks before replacing it (Y is the
   default). To keep it, answer no and re-upload the matching build later, or
   discard it on purpose with **Discard** in Admin or `--discard-ota-session`.
   Unattended `--yes` runs discard it automatically.
6. For `NO_IDENTITY` or a controller without the resumable session schema,
   install one current image over USB. Do not use `--no-check` or a legacy OTA
   fallback to bypass the incompatibility.
7. For `OTA_ALREADY_FLASHED`, the new firmware is already selected and only a
   restart is missing. Restart the controller to start it; repeating the CLI
   flash also requests the restart again. A new upload or **Discard** is
   refused until then.

## Protocol details and verification

Image parsing and identity fields have one canonical reference:
[OTA image identity contract](ota-image-identity.md).

Session POST accepts exactly `size`, `sha256`, `arch`, `hardware`, `machine`,
`version`, and `transferId`.
PATCH offsets and non-final lengths must be multiples of 4096; the advertised
maximum remains 65536. On interruption, query the session and reconstruct the
next range from `nextOffset`, including backward movement after reboot.
`running.imageSha256` and `staged.imageSha256` are the verified appended image
digest; session `sha256` remains the hash of the entire file, including that
digest. `bootState` uses IDF image states (unknown read failure is -2).

Run the focused functional regression suite with `bash src/tests/run_ota_tests.sh`.
Hardware qualification must cover Web and CLI without a browser open during
confirmation, fragmented/disconnected transfers, power loss around a journal
checkpoint, equal-version builds, same-ROM reinstall, and rollback. Host
tests do not certify radio timing, physical circuit safety, or power-cut behavior.
