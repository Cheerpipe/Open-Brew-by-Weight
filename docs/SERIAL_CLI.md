# USB serial CLI

Open Brew by Weight accepts **line-based commands** on the same USB serial port as
the logs (**115200** baud). Verbs are case-insensitive. SSIDs and passwords
are case-sensitive; wrap values with spaces in double quotes.

App CDC is off unless **GPIO 4 is jumpered to GND at reset**, or the
firmware was built with `./scripts/dev build --development` (the only build
that includes the USB console). See
[Hardware](HARDWARE.md). Without that Dupont (on the default build), the CLI
port does not enumerate while the app is running; use **OTA** or **BOOT + RST**
(ROM download) to flash. Remove the jumper before installing the controller in
the machine.

Type firmware commands into the serial monitor, not your shell. Build/flash
commands run in the shell and are documented in [Build scripts](SCRIPTS.md).

## Open the port

Open the port through the supported developer facade:

```sh
./scripts/dev monitor --port /dev/cu.usbmodem2101 --speed 115200
```

On Linux the port is often `/dev/ttyACM0` or `/dev/ttyUSB0`. Exit with
**Ctrl+]**. The script prompts for and remembers the port in `.openbrewbyweight`.

To build the console image, install it, and open the monitor in one ordered
command:

```sh
./scripts/dev build flash monitor --development --confirm \
  --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra \
  --port /dev/cu.usbmodem2101 --speed 115200
```

The monitor starts only if build and flash both succeed. See the
[complete `dev` examples](SCRIPTS.md) for USB flash and other pipelines.

Close other serial clients before opening this port. Type `HELLO` and press
Enter; expect `how are you`. Then use `HELP` or `NET_STATUS`.
If logs obscure input, send `SERIAL_DEBUG_OFF`. A missing port usually needs
the boot jumper described above, not a different baud rate.

Successful mutating commands print `OK queued …`, `OK …`, or a status dump.
Rejections print `ERR …`. Firmware replies do not echo passwords; avoid terminal recording/local echo
when entering credentials.

After `REBOOT` / `SET_WIFI` / `CLEAR_WIFI` / `FACTORY_RESET` the board
restarts and the monitor session may drop.

## Safety gate

Destructive commands need the same gate as the Web UI: physical paddle
**OFF**, machine circuit open, state **Ready**, no active cycle.

If USB/serial is also unavailable, use the
[paddle emergency recovery procedure](EMERGENCY_RECOVERY.md). It works before
Wi-Fi and BLE startup and keeps machine circuit open throughout the operation.

Always allowed (including during a cycle):

- `HELP`, `HELLO`
- dumps (`*_STATUS`, `NET_STATUS`, `LOG_DUMP`, `HEALTH`, `HEAP`, `BOOT_HEAP`)
- `SERIAL_DEBUG_ON` / `SERIAL_DEBUG_OFF`, `DEBUG_FULL` / `DEBUG_OFF`
- link mutations (`WIFI_CONNECT` / `DISCONNECT` / `RESTART`, `AP_START` /
  `AP_STOP`, `WEBUI_START` / `STOP` / `RESTART`)

Link mutations print `WARN cycle active; proceeding` if a shot is running.

## Holds

`WIFI_DISCONNECT`, `AP_STOP`, and `WEBUI_STOP` stay in effect until the
matching `CONNECT` / `START` / `RESTART` or a reboot. Automatic STA retry,
boot SoftAP fallback (only before a successful STA join this session), and
HTTP retry do not undo those stops.

`AP_START` keeps SoftAP up even if STA is already connected (AP+STA) and
skips the 3-minute SoftAP idle shutdown until `AP_STOP`. After a
successful STA join, SoftAP is not auto-raised on link loss — use `AP_START` or
reboot. Automatic SoftAP from boot is still torn down when STA comes up, and
also after 3 minutes idle with no SoftAP stations (latched for the rest of the
boot; `AP_START` still works).
`WEBUI_STOP` is not undone by `AP_START`.

## Probe and help

| Command | Parameters | Effect |
| --- | --- | --- |
| `HELP` | none | Prints one-line summaries and examples |
| `HELLO` | none | Replies `how are you` |

## Device

| Command | Parameters | Effect |
| --- | --- | --- |
| `REBOOT` | none | Restarts firmware after any current shot finishes |
| `FACTORY_RESET` | none | Wipes Wi-Fi, settings, calibration, and shots; device password `ineedacoffee`; restarts (safety gate) |
| `SET_DEVICE_PASSWORD` | `<password>` | Sets the device password (8–63 chars, not `ineedacoffee`). SoftAP WPA2, Admin unlock, OTA, and the optional Web UI sign-in use it. Signs out every Web UI device; Home Assistant stays connected. Does not require the current device password. Safety gate |
| `RESET_DEVICE_PASSWORD` | none | Restores device password `ineedacoffee` and turns the Web UI password off. STA unchanged. Safety gate |

## STA credentials (persist + reboot)

`SET_WIFI` always uses **DHCP**. Set a static IP from the Web UI.

| Command | Parameters | Effect |
| --- | --- | --- |
| `SET_WIFI` | `<ssid> [password]` | Saves home Wi-Fi (DHCP), **commits** it (no 3-minute Web UI confirm window), and restarts. Omit password if open. Quote spaces. Safety gate |
| `CLEAR_WIFI` | none | Forgets saved STA only; restarts (safety gate) |
| `RESET_NETWORK_AP` | none | Forgets STA, restores device password `ineedacoffee`, and turns the Web UI password off; restarts (safety gate) |

## STA link (no NVS change)

| Command | Parameters | Effect |
| --- | --- | --- |
| `WIFI_CONNECT` | none | Associates the saved STA. Errors if none is configured |
| `WIFI_DISCONNECT` | none | Drops STA and holds reconnect. SoftAP auto-raise only if STA never connected this session; otherwise use `AP_START` |
| `WIFI_RESTART` | none | Drops then reconnects saved STA (no ESP reboot) |
| `WIFI_STATUS` | none | Dumps STA config, link, timers, and holds |

## SoftAP

| Command | Parameters | Effect |
| --- | --- | --- |
| `AP_START` | none | Raises SoftAP (`OpenBrewByWeightAP-xxxxxxxx` at `192.168.4.1`). Stays up if STA is connected. Skips SoftAP idle shutdown until `AP_STOP`. Does not start HTTP if `WEBUI_STOP` is held |
| `AP_STOP` | none | Stops SoftAP and holds auto-raise. HTTP stays if STA is up |
| `AP_STATUS` | none | Dumps SoftAP state including the live name and Wi-Fi sleep (never the device password) |

## Web UI

| Command | Parameters | Effect |
| --- | --- | --- |
| `WEBUI_START` | none | Starts the HTTP server |
| `WEBUI_STOP` | none | Stops HTTP and holds auto-start |
| `WEBUI_RESTART` | none | Bounces HTTP. Sessions in RAM survive |
| `WEBUI_STATUS` | none | Dumps HTTP / session / bind state |

## Network shortcut

| Command | Parameters | Effect |
| --- | --- | --- |
| `NET_STATUS` | none | Prints `WIFI_STATUS`, then `AP_STATUS`, then `WEBUI_STATUS` |

## Debug

`SERIAL_DEBUG_ON` turns USB traces on at **info**. `DEBUG_FULL` turns USB
traces on at **debug** and sets the Web UI log ring to **debug**. Both
persist.

| Command | Parameters | Effect |
| --- | --- | --- |
| `SERIAL_DEBUG_ON` | none | USB traces at info; ring unchanged |
| `SERIAL_DEBUG_OFF` | none | USB traces off; ring unchanged. Replies before silencing |
| `DEBUG_FULL` | none | USB traces + ring at debug |
| `DEBUG_OFF` | none | USB traces off and ring none. Replies before silencing |
| `DEBUG_STATUS` | none | Shows `serialLogLevel` and `ringRetainLogLevel` |

## Diagnostics

| Command | Parameters | Effect |
| --- | --- | --- |
| `LOG_DUMP` | none | Prints the RAM debug ring (oldest first), one event at a time. Deferred while a cycle is active or machine circuit is closed. Says so if empty or retain is none |
| `HEALTH` | none | Heap, PSRAM, BLE host alloc counters, loop gap (interval + max), task stacks, CPU load, temperature, flash read mode (effective and built), flash and PSRAM clocks, PSRAM mode, alert latches |
| `SCALE_STATUS` | none | BLE scale link, preferred MAC/name, weight freshness, recovered stale count/time, and the saved `scanIntensity` (`aggressive` / `balanced` / `relaxed`) |
| `NTP_STATUS` | none | Wall clock / NTP state, saved IANA `timezoneId`, and the offset applied at the current UTC instant. Notes if STA is down |
| `HEAP` | none | Internal memory heap summary plus the list of free blocks (size and start address, up to 12) so you can see which gaps bound the largest allocation. Reports `freeBlocksTruncated` when more free blocks exist |
| `BOOT_HEAP` | none | Retained memory samples from the current boot, including the startup reservation and first network/cloud/WebSocket stages. Works with debug traces off |
| `AUDIT_DUMP` | optional count | Prints the curated audit log (newest first), at most the given number of records (default 24, capped at 128), with the store state, retained count, and dropped/rate-limited counters |
| `TASK_PROFILE` | optional `on`/`off` | `on` starts the task profiler (it stops itself after 5 minutes) and `off` stops it. Without a parameter it prints every task's lowest free stack in bytes and its CPU share, then the control-loop peak gap and each loop phase's longest run. Stack minimums are only collected while the profiler runs |
| `AUDIT_CLEAR` | none | Erases the durable audit log and the RAM latch immediately |
| `AUDIT_VERBOSE` | optional `on`/`off` | Arms (default) or stops a 30-minute verbose hunt session: marked debug events join the audit log. Needs a synchronized clock |

`HEALTH` stack watermarks are bytes (`stackUnit=bytes`). Legacy `Words` suffixes
are retained without rescaling their values. `4294967295` means unavailable;
zero is a valid exhausted margin and must never be filtered out.

### Capture startup memory

The controller captures startup memory automatically. You can connect the USB
monitor after startup; you do not need to enable debug logs beforehand. USB
must be enabled by the boot jumper or a development build as described above.

1. Start the controller with the firmware you want to measure. Each reboot
   replaces the previous capture; the report is retained in RAM, not flash.
2. Open the [USB monitor](#open-the-port) at 115200 baud and enter `BOOT_HEAP`.
3. If the report says `status=recording`, wait and request it again. Capture
   starts when networking starts, before Wi-Fi, mDNS and the HTTP server, and
   finishes once that 60-second window and any first WebSocket outcome window
   have ended. An attempt without an outcome finishes once both
   `network_start` and `ws_start` are at least 60 seconds old.
4. When it says `status=complete`, copy the entire response into a local text
   file. Save the firmware version, board profile and whether cloud observation
   used HTTP or WebSocket beside it. Keep the same settings for comparisons.

The comma-separated rows contain `stage,result,ms,free,largest,allocated,
blocks,dmaFree,dmaLargest,psramFree,psramLargest`. Memory values are bytes;
`ms` is time since boot. Rows are grouped by stage; timestamps show their actual
order. `free` and `largest` describe internal byte-accessible memory; `dmaFree`
and `dmaLargest` describe internal DMA-capable memory. These overlapping pools
must not be added together.

Compare `network_start` with `wifi_init`, `mdns_ready` and `http_ready` to see
what Wi-Fi bring-up takes, then `ws_start` and `ws_done` for the first
WebSocket attempt. `ws_init` samples completion of the client setup step;
`ws_task` samples completion of the task-start step. Results are `ok` or
`failed`; they distinguish successful setup from failure or a step prevented
by an earlier error or gate. Callbacks may run before the task-start call
returns; use timestamps to compare the rows. Missing stages were not reached
during the capture window, for example cloud/WebSocket when they are disabled.
Each stage retains its first outcome, including failures; later retries do not
overwrite it. `cloud_done` reports transport completion, not account validity.

`post_min` covers the first 60 seconds after network start; `ws_min`
independently covers the first 60 seconds from `ws_done`, whether that first
outcome was successful or failed. The report includes `postWindowMs` and
`wsWindowMs`. These minima combine stage samples and sampling by the health
task at approximately 100 ms intervals. Brief dips between samples can be
missed. `samples=0` means the phase has no measurements, not zero available
memory. The heap minimum shown by `HEAP` and `HEALTH` is measured from network
start and may combine region minima reached at different times.

Release images expose the same capture as the `bootHeap` section of
`/api/v1/debug/export`: `stages` rows are `[stage,result,ms,free,largest]` and
`minima` holds `[samples,free,largest]` for `post` and `ws`. For where memory
blocks actually sit at each stage, download **Diagnostic → HEAP → Heap map**.
Use the same firmware revision, board, account, connection settings and task
stack sizes for each build.
This trial also reduces the WS SDK task stack from 8 to 6 KiB; capture the
`websocket_task` and `micra_cloud` stack watermarks after cold connection,
renewal and reconnection before accepting it. Save separate boot captures for
successful startup and a failed connection. Reconnection activity after the
capture window requires
fresh `HEAP` and `NET_STATUS` reports before and after; boot-stage rows retain
only their first outcome.

`status=not_started` means network startup has not yet reached the reservation
attempt. `BOOT_HEAP` is read-only and does not start a new capture or change
network settings. To inspect current memory after the capture ends, use `HEAP`.

## Shot history

| Command | Parameters | Effect |
| --- | --- | --- |
| `CLEAR_SHOTS` | none | Clears recorded shot history (safety gate) |

## Short workflows

- **Inspect a problem:** `HELLO` → `NET_STATUS` → `SCALE_STATUS` → `HEALTH`.
  Save a redacted transcript with firmware and scale versions. If `HEALTH`
  shows a small largest heap block, follow with `HEAP` to see the free-block
  layout.
- **Restore AP without erasing Wi-Fi:** `AP_START`; use `WEBUI_START` too if
  HTTP was explicitly stopped.
- **Forgot Admin password, keep Wi-Fi:** while idle, `RESET_DEVICE_PASSWORD`.
  Then change the factory password from Admin.
- **Full reset:** compare [what is erased](settings/factory-reset.md) before
  sending `FACTORY_RESET`.

SSID/password values with spaces need double quotes in the firmware command,
for example `SET_WIFI "Coffee Lab" "<your-network-password>"`. Enter the real
secret only in the serial session; do not include it in shared transcripts.
