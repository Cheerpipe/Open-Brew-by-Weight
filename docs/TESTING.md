# Testing and validation

A **gate** is the set of checks that a change must pass before it can be accepted.
The project first classifies the changed files by risk, then runs the matching
gate. Higher levels include the lower-level checks, so a safety-critical change
receives much more scrutiny than a wording correction. The authoritative rules
and exact commands are in [Validation gates](../VALIDATION.md).

| Level | What it is for | What it checks | When it runs |
| --- | --- | --- | --- |
| R0 | Non-critical documentation and repository metadata | Documentation contracts, local links, headings, images, and known paths | For documentation-only changes and as the base of every higher gate |
| R1 | Web UI, tests, developer tooling, and isolated logic | R0 plus the normal host tests and any focused checks or generated assets relevant to the change | While developing these areas and before submitting the finished change |
| R2 | BLE, networking, saved data, OTA, and build changes | Full host coverage, ASan/UBSan, TSAN, architecture rules, Web contracts, and every built-in compatible profile pair | When a change can affect integration, concurrency, memory safety, or a firmware image |
| R3 | Relay and machine control, ISR, watchdog, boot, GPIO, partitions, remote control, or an unknown path | R2 plus Cppcheck on the changed code, a no-inlining compile of the reference pair, build variants, and the applicable HIL/manual evidence | Before accepting any safety-critical or not-yet-classified change |
| Release | A firmware image intended for distribution or installation | The complete automated analysis plus resource budgets, applicable soak tests, HIL, and the manual test plan | For a release candidate; a passing automated R3 run alone is not release approval |

**Host tests** run on the developer computer, without an ESP32 or
espresso machine. They are quick feedback for software behavior, but cannot prove
that real wiring, timing, radio conditions, or machine stopping are safe. The
focused profiles are:

| Profile | Purpose | When to use it |
| --- | --- | --- |
| `normal` | Runs the broad functional host suite without a sanitizer | During ordinary development and in every R1-or-higher gate |
| `asan` | Runs the host suite with ASan and UBSan to expose memory errors and undefined operations | For R2/R3 changes and when investigating crashes or suspicious memory behavior |
| `tsan` | Runs concurrent host scenarios with TSAN to find unsafe access to shared data | For R2/R3 changes and whenever task or thread ownership changes |
| `web` | Verifies generated Web UI assets and browser-facing contracts | After changing the Web UI, its source assets, or asset generation |
| `ble` | Focuses the normal host suite on scale protocols and BLE radio behavior | After changing scale communication or BLE protocols |
| `ota` | Exercises OTA host logic plus command-line and Web resilience cases | After changing firmware-update behavior or its interfaces |
| `tooling` | Checks the developer command facade, risk classification, and validation contracts | After changing scripts or repository workflow rules |

Run these profiles locally with `./scripts/dev test <profile>` and the risk
gates with `./scripts/dev validate --risk R0..R3`; `./scripts/dev classify`
determines the gate required for the changed paths. These commands do not
flash a board, operate the relay, or replace required HIL and manual evidence.
