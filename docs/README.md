# Documentation

Choose a task below. User guides explain observable behavior; developer
references define implementation and validation contracts. Follow links as
needed rather than reading the entire directory.

## Setup and use

- [First setup and daily use](GETTING_STARTED.md)
- [Hardware and installation prerequisites](HARDWARE.md)
- [Build, test, and USB installation](BUILD.md)
- [Troubleshooting](FAQ.md), [USB commands](SERIAL_CLI.md), [recovery](EMERGENCY_RECOVERY.md)
- [Firmware updates (OTA)](features/ota.md), [screenshots](SCREENSHOTS.md)

## Features

- [Brew by weight](features/brew-by-weight.md), [presets](features/presets.md)
- [Tare and retare](features/tare-retare.md), [cup protection](features/cup-protection.md)
- [Fast and slow shots](features/fast-and-slow-shots.md),
  [scale-loss backup](features/auto-to-manual.md)
- [Alerts](alerts.md), [shot history](features/shot-history.md),
  [activation history](features/activation-history.md)
- [Home Assistant integration](features/home-assistant.md) ([integration repository](https://github.com/Cheerpipe/ha-open-brew-by-weight))
- [Generic webhooks](features/webhooks.md)
- [Open Brew by Weight Companion](features/companion.md) ([companion repository](https://github.com/Cheerpipe/open-brew-by-weight-companion))

## Settings

| Group | Reference |
| --- | --- |
| Physical switch | [Paddle](settings/paddle.md) / [momentary](settings/momentary.md) |
| Machine integration | [La Marzocco Linea Micra](settings/linea-micra.md) |
| Without a scale / rinse | [No-scale BBW](settings/no-scale-bbw.md) / [quick rinse](settings/quick-rinse.md) |
| Scale and cup | [Scales](settings/scales.md) / [Bluetooth on/off](settings/scales.md#bluetooth-onoff) / [cup detection](settings/cup.md) / [tare](settings/tare.md) |
| Network | [Wi-Fi](settings/wifi.md) / [access point](settings/ap.md) / [device name](settings/wifi.md#discovery-by-name) / [Web UI access](settings/web-access.md) |
| Energy | [Power management](settings/power-management.md) |
| Interface | [Theme](settings/theme.md) / [shot card colors](settings/theme.md#shot-card-colors) / [app settings](settings/app.md) |
| Maintenance | [Scheduled restart](settings/scheduled-restart.md) |
| Reset | [Factory reset](settings/factory-reset.md) / [firmware mode](settings/firmware-mode.md) |

## Developer map

Start with [Contributing](../CONTRIBUTING.md) and the applicable `AGENTS.md`.
Temporary working files live in Git-ignored `temp/` inside the local project.
Development plans live in Git-ignored `docs/plans/`, and audits in Git-ignored
`docs/audits/`. Each plan or audit is its own progress record and session handoff.
Their contents are not onboarding or background context: read them only when the
user explicitly requests reading or working with a plan or audit, or when they
are referenced within two levels from that requested record. See the
[record access rule](AI_WORKFLOW.md#explicit-access-to-plans-and-audits).
See the [shared format and recovery procedure](AI_WORKFLOW.md#plans-audits-and-session-handoff).
All three directories belong to the local project and remain accessible from
the IDE; see [local storage and Git](AI_WORKFLOW.md#local-project-files-and-git).
Use [documentation maintenance](AI_WORKFLOW.md#documentation-maintenance) to
identify which guides to update when settings, scripts, or behavior change.
The map below identifies the smallest relevant sources and tests.
`./scripts/dev context <area>` lists focused paths, not file contents; supported
areas are safety, control, machine, scale, ble, network, ota, persistence, web,
build, and tests. There is no `docs` area.

File names in the table are relative to this directory unless a source path
or root policy is named. Commands are starting points, not substitutes for the
risk gate in [VALIDATION.md](../VALIDATION.md).

| Need | Documentation | Editable sources | Tests | Command |
| --- | --- | --- | --- | --- |
| Safety and relay | `PROJECT_CHARTER.md`, `HARDWARE.md`, `MANUAL_TEST_PLAN.md` | `src/OpenBrewByWeightSafety.h`, `src/OpenBrewByWeightMachineRelay.h`, `src/OpenBrewByWeightHardware*` | `safety_external_host_test.cpp`, main host scenarios | `./scripts/dev validate --risk R3` |
| Control/brew | `ARCHITECTURE.md`, `STATE_MACHINES.md`, feature docs | `src/OpenBrewByWeightBrew*`, `src/OpenBrewByWeightDomain.h` | `open_brew_by_weight_host_test.cpp` | `./scripts/dev test normal` |
| Machine inputs | `HARDWARE.md`, `settings/paddle.md`, `settings/momentary.md` | `src/OpenBrewByWeightMachine*` | `momentary_machine_host_test.cpp`, main host tests | `./scripts/dev validate --risk R3` |
| Scale sensing | `ARCHITECTURE.md`, `CONCURRENCY.md` | `src/OpenBrewByWeightScale*` | main host tests | `./scripts/dev test normal` |
| BLE protocols | library README, `CONCURRENCY.md` | `libraries/EspressoScaleBLE/src/`, `src/OpenBrewByWeightScaleWorker.cpp` | library tests, main host BLE-scan cases | `./scripts/dev test ble` |
| Network/Webhooks | feature docs, `RESOURCE_OWNERSHIP.md` | `src/OpenBrewByWeightNetwork*`, `src/OpenBrewByWeightWebhook*` | `webhook_error_host_test.cpp`, Web contract tests | `./scripts/dev validate --risk R2` |
| Integration API | [`INTEGRATION_API.md`](INTEGRATION_API.md) | network/webhook sources | shared JSON fixtures and integration tests | `./scripts/dev validate --risk R3` |
| Companion link and relay protocol | [`COMPANION_LINK.md`](COMPANION_LINK.md) | `src/network/OpenBrewByWeightCompanion*`, `src/machine/OpenBrewByWeightMicraCompanion.h` (La Marzocco route) | `docs/schemas/companion-link-v1.vectors.json`, companion link host tests | `./scripts/dev validate --risk R3` |
| Shot trace format and flash pool | [`SHOT_TRACE.md`](SHOT_TRACE.md) | `src/OpenBrewByWeightShotTrace.h`, `src/OpenBrewByWeightShotTracePool.h`, `src/diagnostics/OpenBrewByWeightShotTraceRoutes.inc` | `TRC*`/`TPW*` host cases, `src/tests/shot_trace_test_decoder.h`, `shot_trace_pool_host_test.cpp`, `shot_trace_routes_host_test.cpp` | `./scripts/dev test normal` |
| Shot export formats and the trace view | [`schemas/README.md`](schemas/README.md) | `src/web/js/stats.js` (`shotsJson`, `decodeShotTrace`, `shotGaggiuino`, `shotVisualizer`, `shotBeanconquerorLink`) | `33-shot-export.inc.js` | `./scripts/dev test web` |
| Firmware OTA | `features/ota.md`, `features/ota-image-identity.md`, `EMERGENCY_RECOVERY.md` | `scripts/internal/ota-idf`, `src/OpenBrewByWeightOta*` | OTA host, CLI, and Web resilience tests | `./scripts/dev test ota` |
| Persistence | `ARCHITECTURE.md`, settings docs | `src/OpenBrewByWeightPersist*`, durable stores/logs | `persistence_host_test.cpp` | `./scripts/dev test normal` |
| Web UI | `SCREENSHOTS.md`, feature/settings docs | `src/web/` | Web asset/contract tests | `./scripts/dev test web` |
| Build and partitions | `BUILD.md`, `SCRIPTS.md`, `STATIC_ANALYSIS.md` | `idf/`, build scripts | profile, architecture, and size checks | `./scripts/dev profiles` |
| Hardware/machine build profiles | [`BUILD_PROFILES.md`](BUILD_PROFILES.md) | `config/hardware/`, `config/machines/`, profile resolver | profile contract tests | `./scripts/dev test tooling` |
| Scheduling/resources | `SCHEDULABILITY.md`, `P2_RESOURCE_BUDGETS.md`, `P2_TARGET_TRACE.md` | task/owner/watchdog headers | TSAN and resource-owner tests | `./scripts/dev test tsan` |
| Internal RAM and ESP-IDF heap issues | `IDF_MEMORY_WATCH.md`, `P2_RESOURCE_BUDGETS.md` | `src/OpenBrewByWeightBootHeap.h`, `src/OpenBrewByWeightHeapMap.h`, `idf/sdkconfig.defaults*` | boot heap host test | `./scripts/dev build --development` |
| Developer tooling | `AI_WORKFLOW.md`, root `VALIDATION.md` | `scripts/`, `.github/` | dev contract and risk golden tests | `./scripts/dev test tooling` |

## Repository boundaries

- Canonical firmware: `src/`; Web source: `src/web/`; BLE library:
  `libraries/EspressoScaleBLE/`; ESP-IDF integration: `idf/`.
- Generated: `src/OpenBrewByWeightVersion.h`,
  `src/OpenBrewByWeightWebAssetsGzip.h`, build trees, reports, and `artifacts/`.
- Local plans: `docs/plans/`; local audits: `docs/audits/`. Both are retained
  locally, excluded from Git, and serve as their own session handoffs.
- Local temporary working files: `temp/`, inside the project and excluded from
  Git. Keep task-specific files accessible from the IDE; Git exclusion does not
  mean storing files outside the project.
- Vendored/dependency output: `idf/managed_components/` and `node_modules/`.
  Do not review or load these trees unless dependency provenance itself is the
  task. Lockfiles and IDF configuration remain visible and reviewable.
- Current safety authority is the charter, validation policy, architecture
  documents, and manual test plan.

## Key policies

- [Project charter](../PROJECT_CHARTER.md)
- [Validation gates](../VALIDATION.md)
- [Contribution workflow](../CONTRIBUTING.md)
- [AI workflow](AI_WORKFLOW.md)
- [Build](BUILD.md) and [script reference](SCRIPTS.md)
- [Release checklist](RELEASE.md) and [changelog](../CHANGELOG.md)
- [Manual test plan](MANUAL_TEST_PLAN.md)
