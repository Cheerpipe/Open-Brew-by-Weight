# AI development workflow

## Progressive discovery

Start with `AGENTS.md`, `docs/README.md`, and one scoped `AGENTS.md`; keep this
orientation below about 16 KiB. Run `./scripts/dev context <area>` to list the
smallest relevant docs, sources, tests, and command before reading content.
Search symbols with targeted `rg`; inspect callers and tests before expanding to
an adjacent subsystem. Do not bulk-load builds, managed components,
`node_modules`, reports, generated Web assets, binaries, or STL files.
Exclude `docs/plans/` and `docs/audits/` from proactive context gathering and
content searches. Their contents require an explicit user request under the
[record access rule](#explicit-access-to-plans-and-audits).

## Work and evidence

Classify paths first and state safety invariants for R2/R3. Prefer existing
boundaries and focused tests. A run record under `artifacts/runs/<run-id>/`
contains the full log and a redacted `summary.json`; `latest.json` points to the
most recent summary. Reuse a recorded result only when its SHA and inputs are
unchanged. Runtime firmware log level is independent of `scripts/dev`
verbosity.
User-facing API error text is born in the locale catalog
(`src/web/locales/en.json`, `err.*`): the generator fails the build when a
reason code lacks its catalog entry, so no new error prose may be written
inline in firmware or Web UI sources.

<a id="temporary-working-state"></a>

## Local project files and Git

The local project includes both Git-tracked files and Git-ignored working files.
"Outside Git" means excluded from version control, never outside the project
directory. Keep all task-created working files inside the project so they can be
opened from the IDE, including files previously kept in external temporary
directories or session workspaces.

| Local project directory | Contents |
| --- | --- |
| `temp/` | Temporary notes, scratch scripts, intermediate files, and other task working files without an existing project-local output location |
| `docs/plans/` | Development plans, including their progress and handoff |
| `docs/audits/` | Audits, including their findings, progress, and handoff |

The root `.gitignore` must contain `/temp/`, `/docs/plans/`, and `/docs/audits/`.
Never stage or force-add their contents or add tracked placeholders. Check an
actual path with `git check-ignore -v`, and confirm
`git ls-files -- temp docs/plans docs/audits` is empty. Ignore rules do not untrack
existing files; resolve any tracked local working files within the authorized
scope while preserving their local contents.

Use a descriptive, task-specific subdirectory under `temp/`, with the
`ai_temp_` prefix on that directory rather than every file. Create directories
when needed. Use project-local paths for scratch files instead of `/tmp`,
`/private/tmp`, or an external session workspace. Existing project-local tool
outputs such as `artifacts/runs/` and `reports/` keep their documented locations.

When resuming work, identify any existing external files belonging to that task
and bring them into `temp/`, or the appropriate plan/audit directory. Verify
their contents at the destination and update references in the owning plan or
audit before removing the old copies. Preserve name collisions and files still
used by running processes; record any pending relocation in the handoff. This
applies to task working files, not unrelated personal files or installed tools.
For plans and audits, migration or IDE accessibility does not authorize reading
their contents; follow the [record access rule](#explicit-access-to-plans-and-audits).

Keep these directories accessible through project-relative paths. Do not add
them to IDE file-explorer exclusions. Git ignore and editor visibility are
separate concerns; if a tool hides ignored files, open the path directly or use
`rg --files --hidden --no-ignore` with the specific task directory. Avoid loading
all local working files just to discover one task.

Clean up only task-owned temporary files when no longer needed. Preserve files
still referenced as evidence or needed for recovery until their contents have
been retained in the appropriate local record. Plans and audits remain locally
after completion and are removed only when requested. Never use cleanup as a
reason to move working files outside the project.

<a id="plans-and-session-handoff"></a>

## Plans, audits, and session handoff

### Explicit access to plans and audits

Read the contents of a stored plan or audit only when the user explicitly asks
to read or work with that record. Limit access to the requested files or stated
scope. Do not open, search, summarize, or load these directories as background
context during orientation, general discovery, or session recovery.

A record reachable from the requested record within two levels of path or link
references is within scope: read it when the requested work needs it. For
example, a request to implement plan A permits reading record B when A links to
B, and record C when B links to C; references found in C do not extend the
grant. If a reference is ambiguous, ask before reading.

File availability, a similar task name, an instruction to maintain records, or a
link in any other document does not grant access. A request to change plan/audit
policies is not a request to read stored plans or audits.

Without an explicit request, continue from the conversation and canonical
documentation. Creating and updating the current task's record from information
already available remains part of the tracking workflow; it does not authorize
reading stored records to gather context. Do not reopen old records on your own
or make reading one a routine prerequisite for otherwise independent work.

### Location and ownership

Store every development plan in `docs/plans/` and every audit in `docs/audits/`.
Use a descriptive name such as `YYYY-MM-DD-short-task.md`; add a suffix if needed
to avoid a collision. Create the plan before multi-step implementation, or the
audit file before an audit; resume an existing record only under the explicit
access rule above. Each file owns its checklist, progress,
decisions, evidence, and session handoff. An audit's checklist belongs in its
audit file; do not create a duplicate plan just to track the review. Any transient
planning UI must stay consistent with the owning file, not replace it.

Apply the shared [local storage and Git rules](#local-project-files-and-git).
Plans and audits are part of the local project even though Git ignores them.

Retain completed plans and audits locally for traceability and recovery. Both
are exempt from temporary-directory naming and automatic temporary-file cleanup;
remove them only when requested. A Git clone does not carry these ignored files.
Keep secrets and unredacted logs out of both. Record durable behavior in its
canonical guide; versioned documentation must not rely on or link to a specific
local plan or audit.

### Checklist and progress contract

These requirements apply equally to every plan and audit:

- State the objective, scope, acceptance criteria, branch and starting commit,
  relevant pre-existing changes, risk, overall status, and created/updated times.
  Use ISO 8601 timestamps with a timezone.
- Break work into concrete steps and substeps. Every step and substep must have
  a stable ID, a Markdown checkbox, and one explicit status: `pending`,
  `in_progress`, `blocked`, `completed`, or `cancelled`.
- Use `[x]` only for `completed`; all other statuses use `[ ]`. Complete a parent
  only after its required substeps and acceptance conditions are complete. Explain
  cancelled or superseded work in the progress log; do not silently erase it,
  renumber existing IDs, or mark it as successfully completed.
- Mark a step/substep `in_progress` before starting it. Immediately after any
  part completes, update its checkbox/status, parent status, timestamp, evidence,
  and next action in the same file. Do not defer updates until the session ends.
  If a substep completes only partly, split out the remaining work with new IDs
  and record the completed part accurately.
- Keep a chronological progress log keyed by IDs: meaningful changes, affected
  paths, decisions and reasons, command results, failures, and blockers. Reference
  full logs by path instead of copying them; distinguish verified results from
  assumptions and pending checks.
- Maintain a recovery checkpoint with the current state, exact next action,
  changed files, validation evidence still needed, blockers, and outstanding
  permissions or user decisions. Save it before long-running or consequential
  operations, when blocked, and before pausing or handing back the task.
- Finish only when acceptance criteria and required validation are satisfied.
  Record final results and remaining limitations; leave blocked or incomplete
  work visible rather than claiming completion.

### Resume after an interruption

Only when the user explicitly requests reading or resuming a plan or audit,
open the requested record. If needed, use filename-only discovery within that
request's scope; do not read candidate contents to choose one. If the intended
record remains ambiguous, clarify which file is requested before reading it.
Do not automatically search for a matching record after an interruption.

For the requested record, read the checklist, recent progress, findings when
applicable, and recovery checkpoint. Resume that same file rather than creating
a duplicate handoff.

Compare the recorded branch, commit, changed files, and evidence with the actual
worktree and any running commands. A crash may happen between an operation and
its recorded update, so verify the outcome before repeating it. Reconcile stale
statuses, preserve unrelated work, and continue from the first unfinished
dependency. An old plan or audit does not grant new hardware, destructive, or
external action permissions.

### Audit findings and completion

Keep the audit's scope, review steps/substeps, findings, evidence, and recovery
checkpoint together in its file under `docs/audits/`. Give findings stable IDs
and record severity, affected paths, supporting evidence, and disposition;
distinguish confirmed findings from unverified concerns. Update findings and
the review checklist as each part of the audit completes.

A completed audit means its review scope and required checks are complete, not
that all findings are fixed. Keep unresolved findings and coverage limitations
explicit. If remediation is authorized, track implementation in `docs/plans/`
and reference the audit path and finding IDs rather than duplicating findings.
Keep each file's own progress and handoff current.

### Plan and audit template

Adapt this skeleton with concrete actions and acceptance criteria before work.
Add as many steps and substeps as needed, keeping every actionable item tracked.
For an audit, replace implementation actions with specific review and evidence
collection actions, and add a Findings section using the fields above. Save the
adapted file in the directory for its record type.

```markdown
# <Task title>

- Record type: <development plan or audit>
- Objective and scope: <requested outcome and boundaries>
- Acceptance criteria: <observable completion conditions>
- Local project / branch / starting HEAD: <path, branch, commit>
- Pre-existing changes to preserve: <relevant paths or none>
- Risk and applicable invariants: <classification and constraints>
- Created / last updated: <ISO 8601 timestamps with timezone>
- Overall status: pending

## Steps

- [ ] 1. [pending] Establish scope and approach.
  - [ ] 1.1 [pending] Inspect relevant contracts and existing behavior.
  - [ ] 1.2 [pending] Classify paths and define acceptance checks.
- [ ] 2. [pending] Implement the agreed change.
  - [ ] 2.1 [pending] Apply the task-specific changes.
  - [ ] 2.2 [pending] Update affected canonical documentation.
- [ ] 3. [pending] Verify and deliver.
  - [ ] 3.1 [pending] Run required checks and record results and log paths.
  - [ ] 3.2 [pending] Review acceptance criteria and record the final handoff.

## Progress and decisions

- <timestamp> — <step ID>: <change, result, evidence, decision or blocker>

## Recovery checkpoint

- Current state and changed files: <facts needed to resume>
- Validation: <commands, outcomes, evidence paths, pending checks>
- Next action: <step ID and concrete operation>
- Blockers / outstanding permissions or decisions: <details or none>
```

## Documentation maintenance

Documentation is part of the implementation, not a later task. Apply this guide
when adding, changing, renaming, or removing settings, commands, interfaces,
supported hardware, workflows, or observable behavior. Internal refactors need
documentation only if they change a documented contract or make a reference stale.

### Choose the owner

Read only the matching guides below and the sources needed to verify the change.
Paths in this table are relative to `docs/` unless prefixed with `../`.
Update additional pages only when their own instructions or claims are affected.

| Change | Canonical documentation to update |
| --- | --- |
| Setting, default, range, or UI control | Matching guide under `settings/` or `features/`, found through [the index](README.md). Brew parameters belong to their feature guide; update [presets](features/presets.md) when preset scope or save/load behavior changes. |
| Brewing behavior, guards, outcomes, or alerts | Matching `features/` guide and [alerts](alerts.md) where affected; [state machines](STATE_MACHINES.md) for transitions or outcome contracts. |
| Shell script, flag, environment variable, or local config | [Script reference](SCRIPTS.md); [Build](BUILD.md) if setup/build steps change, [static analysis](STATIC_ANALYSIS.md) if analysis setup changes. Firmware USB commands belong in [Serial CLI](SERIAL_CLI.md), not the shell reference. |
| Validation gate or contributor workflow | Root [validation policy](../VALIDATION.md), [Contributing](../CONTRIBUTING.md), and applicable `AGENTS.md`; this guide only when agent workflow changes. |
| Webhook or external payload | [Generic webhooks](features/webhooks.md), the [Integration API](INTEGRATION_API.md), and the native [Home Assistant integration](features/home-assistant.md); entity or payload changes also need the [integration repository](https://github.com/Cheerpipe/ha-open-brew-by-weight)'s guides and fixtures. Other interfaces belong in their owning feature or architecture reference. |
| Persistence, reset, or migration | Owning settings/feature guide for user effects; [architecture](ARCHITECTURE.md) for storage contracts and [factory reset](settings/factory-reset.md) if reset behavior changes. |
| BLE support or protocol | [Library README](../libraries/EspressoScaleBLE/README.md); [scale settings](settings/scales.md) for user-visible compatibility or connection behavior. Distinguish implemented support from hardware-tested support. |
| OTA, image identity, or recovery | [OTA](features/ota.md), [image identity](features/ota-image-identity.md), or [recovery](EMERGENCY_RECOVERY.md), according to the affected procedure. |
| Hardware, safety, ownership, or scheduling | [Hardware](HARDWARE.md), [charter](../PROJECT_CHARTER.md), [architecture](ARCHITECTURE.md), [ownership](RESOURCE_OWNERSHIP.md), [concurrency](CONCURRENCY.md), or [scheduling](SCHEDULABILITY.md), as applicable; update [manual acceptance](MANUAL_TEST_PLAN.md) when acceptance steps change. |

Update [first setup](GETTING_STARTED.md) only if onboarding or daily-use steps
change, and [FAQ](FAQ.md) when symptoms or recovery advice change. Keep the root
[README](../README.md) an overview, not a second settings or command reference.
Add a link in [the index](README.md) for a new guide; change screenshots only when
they are misleading, and identify historical captures rather than inventing evidence.

### Verify and write

1. Identify the owner before editing. Check implementation, callers, and relevant
   tests; do not use old prose, screenshots, or external examples as proof.
2. For a **setting**, verify its exact UI label, purpose, units, accepted range,
   default, and global/per-preset scope. State persistence, when changes take
   effect, and important prerequisites or interactions. Check initialization,
   validation, preset seeds, UI visibility, and serialization where applicable;
   distinguish fresh-install defaults from values retained or migrated on upgrade.
3. For a **command**, verify flags and behavior against its parser/help and
   implementation. Give prerequisites, working directory, a minimal invocation,
   expected output/artifact, and relevant failure recovery. Document changed
   defaults, CLI/environment/config precedence, and interactive/non-interactive
   behavior. Mark hardware or destructive actions explicitly; never include secrets.
4. Write the shortest useful procedure: purpose, conditions, steps, expected
   result, then meaningful exceptions. Keep advanced contracts separate. Provide
   one runnable happy-path example and an edge case only when it clarifies a real
   decision or failure; use explicit placeholders for user-supplied values.
5. Maintain one parameter table or reusable example per topic and link to it.
   Extend the existing owner before creating a page. Use targeted `rg` for old
   and new setting names, UI labels, commands, or flags across the affected sources
   and guides to find stale references. For removals, delete obsolete instructions
   and explain replacements or migration when users need them. Preserve inbound
   anchors when moving content, or update their repository links.

For example, changing a cup threshold requires its owning settings table and any
affected numerical example, not a new README section. Adding a build-script flag
requires its script reference entry; update the build walkthrough only if users
need the flag there.

### Completion check

Review the final diff for contradictory values, broken reading order, and
unnecessary duplication. Verify example names, units, syntax, and calculations
against the final implementation. Run relevant safe example/contract checks when
available; never install dependencies, contact hardware, flash, or run OTA implicitly.

Classify and validate **all changed files**, including code and docs, using
[VALIDATION.md](../VALIDATION.md). Markdown does not automatically mean R0.
The offline link check does not certify external URLs or example behavior.
In the completion report, name the updated guides (or give a specific reason no
documentation was needed), checks performed, and any unverified claims or manual
evidence still required. Do not present assumed behavior as tested behavior.
