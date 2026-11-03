# Webhooks

Open Brew by Weight can send shot updates to one HTTP receiver on your local
network. Webhooks are useful for software other than Home Assistant; Home
Assistant users should follow the [native integration guide](home-assistant.md)
instead of configuring this page manually.

Webhooks observe extraction activity. They cannot start or stop the machine.
Delivery is best effort and uses plain HTTP, so it must not be used as a safety
signal. Keep the controller and receiver on a trusted LAN.

## Configure a receiver

While the machine is idle, open **Admin → Webhooks**, enter a reachable
`http://` URL, choose the events, and save. **Send test** delivers a message
without shot measurements. The status below the controls shows the latest
delivery result and drop count. The drop count only includes events you
selected that could not be delivered; events you did not select are not counted.

The controller supports one callback. Changing it replaces the previous
receiver. Enable **Queue webhooks during shots** only when the receiver's
network traffic disrupts a sensitive scale connection; queued delivery may be
delayed until the extraction ends. When that option is enabled, shot entry also interrupts an active delivery.
Scale acquisition always pauses delivery, independently of this option, from
the first eligible advertisement through setup and the Bluetooth quiet interval.
Unsent events remain queued.

If a message fails, the controller tries again 3, 6, and 9 seconds later before
giving up, so a receiver that restarts or a brief Wi-Fi drop does not leave it
out of date. Messages keep their order: later events wait until the retried one
is delivered or dropped. A message that only reports the current state (brew
state, presets, Quick Settings, or IP address) is replaced instead of retried
when a newer one of the same kind is already waiting, so the receiver gets the
latest state sooner. Shot measurements such as first drop and shot end are
always retried. A failed request may already have reached the
receiver, so it can arrive twice; the copy is identical, so receivers can
ignore the repeat. A receiver that rejects a message as invalid (an HTTP 4xx
answer other than 408 or 429) is not retried, and neither is **Send test**,
which always reports its first result.

Each message opens a fresh connection, so receivers that close idle
connections never cause a lost event. A receiver name such as
`homeassistant.local` works; the controller looks it up without holding up a
shot, but a name that cannot be found within the delivery time limit (just under two seconds)
makes that message fail. For the most dependable delivery, reserve an IP
address for the receiver on your router and use it in the URL.

After a failed attempt, diagnostic status identifies the event, request phase,
transport result, and HTTP status. **Diagnostic → Log** records each failed
attempt and whether it will be retried (**Warning**), each message that is
finally dropped (**Error**), and a message that succeeds on a retry (**Info**).
The recorded destination keeps only the scheme, host, port, and path; URL query
values and credentials are never included in that diagnostic. Home Assistant
webhook IDs in the path are hidden.

For a quick local receiver, listen with a development HTTP tool and point the
controller to it. To inspect a captured payload with `curl`, save it as
`event.json` and run:

```sh
curl -X POST -H 'Content-Type: application/json' --data-binary @event.json \
  http://receiver.local:8080/shot
```

## Events

Every request is a bounded JSON `POST`. Common fields identify the schema,
event, controller, boot, cycle, event uptime, wall-clock time, and send time.
`timestamp` is UTC Unix seconds, independent of the saved time zone, and is
`0` when the controller clock is not synchronized.

| Event | Purpose | Event-specific fields |
| --- | --- | --- |
| `brew_state` | Extraction entered `brewing` or returned to `idle` | `state`, duration, target, preset ID/name, stop detail |
| `first_drop` | First qualifying flow was confirmed | first-drop time, weight, target, preset ID/name |
| `end` | Settled final result after drip delay | duration, target, preset ID/name, type, stop detail, optional first drop/weight/flow |
| `presets_changed` | Persisted preset inventory changed | active ID, revision, full bounded item list |
| `quick_settings_changed` | How shots stop changed: a temporary change from the header panel or Home Assistant, a saved setting, or a preset change | revision, active preset ID, mode, and all six boolean values |
| `controller_started` | Network-ready controller boot asks a client to reconcile | boot ID and current configuration revision |
| `integration_history_end` | New History entry, including Backflush and a No-scale BBW attempt stopped before brewing | ID, type, duration, and time |
| `test` | Receiver connectivity check | optional correlation ID used by API clients |

`first_drop` is announced at most once per shot. A later confirmed late-cup
retare can invalidate that earlier observation in the shot record; the sent
event cannot be recalled. A replacement first drop updates the final data
without another `first_drop` event. The `end` event contains the corrected
time, or omits first-drop time and average flow when no replacement was detected.

Preset names on shot events are captured when the cycle starts. Renaming a
preset later does not rewrite the name attached to that shot. The
`presetChanges` is the shared subscription for `presets_changed`,
`quick_settings_changed`, `controller_started`, and `integration_history_end`.
It is disabled for existing
configurations until selected by a compatible client. Settings events are sent
only after persistence succeeds; the startup hint is best effort and is not a
heartbeat.

Example completed shot:

```json
{"schemaVersion":1,"event":"end","deviceId":"AA:BB:CC:DD:EE:FF","bootId":123,"cycleId":42,"uptimeMs":923400,"timestamp":1767225611,"sentAtUptimeMs":923480,"durationMs":27800,"targetWeightG":36.0,"presetId":2,"presetName":"Double","shotType":"auto","stopDetail":"normal_target","firstDropMs":4480,"weightG":36.72,"averageFlowGps":1.57}
```

See the [Integration API](../INTEGRATION_API.md) for the complete field,
enumeration, security, and compatibility contract.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Test fails | Receiver address is HTTP, reachable from the controller, and listening on the configured port. With **Kept in memory** set to **Warning** under **Diagnostic → Log → Logging settings**, each failed attempt and retry is recorded with the reason |
| Messages fail with a receiver name but work with its IP address | The name is not resolvable from the controller's network; use the reserved IP address |
| Test works but shots do not arrive | Webhooks and the required event selections are enabled |
| Some measurements are absent | Optional measurements are omitted when the scale did not provide a valid value |
| Messages arrive late | Disable queuing unless your scale link needs it |
| A second receiver stops receiving | Only one callback can own the controller at a time |
