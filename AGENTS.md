# Repository agent rules

## Start here

Read only this file, `docs/README.md`, and the nearest scoped `AGENTS.md` for
the area being changed. Expand context only when those files point to it.
Never read `docs/plans/` or `docs/audits/` content proactively for context.
Read a plan or audit only when the user explicitly asks to read or work with it,
or when it is reachable from that explicitly requested record within two levels
of path or link references and the work needs it. Other documentation links,
task similarity, and session recovery do not grant access.

## Language

Reason, plan, and write internal notes in English, regardless of the language
used in the prompt. Write the final user-facing reply in the same language as
the user's prompt.

## Priorities and scope

1. Safety and correctness.
2. Root-cause understanding and preservation of existing behavior.
3. Proportionate validation.
4. Small, maintainable changes and efficient context use.

Apply independent expert judgment in software engineering, ESP32-S3, C++, and
architecture. Critically evaluate every request before implementing it. When a
request is suboptimal, can be improved, conflicts with good practice, or uses an
unsuitable approach, explain the concern clearly and propose a concrete course
of correction for the idea or design. Do not silently implement a materially
inferior approach; first make the tradeoffs and recommended alternative clear.

Prefer targeted `rg` searches and focused reads. Do not load generated assets,
build trees, managed components, dependencies, or reports wholesale. Edit
existing logic instead of adding wrappers or parallel implementations. Explain
before adding more than 15 lines for a fix. Never leave placeholder or commented
out code.

While a command is pending, wait only for that command: do not run unrelated
actions or produce, process, or show unnecessary or redundant user responses.

## Safety and permissions

Treat changes to relay, machine control, ISR, watchdog, boot, GPIO, partitions,
OTA safety, or remote activation as R3. Preserve these invariants:

- the relay is open on startup, reset, power loss, and safety failure;
- boot readiness includes every dependency required for safe actuation;
- timing limits and watchdog coverage remain enforced;
- remote machine activation is disabled by default;
- shared resources retain one explicit owner.

Never flash hardware, run OTA, control the relay, or execute HIL unless the user
explicitly requests it. Never expose credentials in argv, logs, or artifacts.
Unknown paths are R3; risk overrides may only increase risk.

Hardware tests that read the serial console require a `--development` build,
which is the only profile that compiles in the USB Serial/JTAG console
(`--development` = `--no-auth-admin --jtag`). Build and flash with it in the same
run, for example `./scripts/dev build flash --development --confirm --hardware
esp32-s3-relay-x1-speaker --machine la-marzocco-linea-micra --port <port>`.
`--development` is build-only: standalone `flash` rejects it, so the image must
come from a `--development` build. Never assume console output from a release
image; the GPIO4 console jumper is the only other route (see
[USB console jumper](docs/HARDWARE.md#usb-console-jumper)).

## Field diagnostics are web-only

Production units are installed inside the espresso machine and have no USB
access: the Web UI log is the only diagnostic channel in the field. Everything
the USB console shows, including ESP-IDF component warnings and errors, must
also reach the Web UI log. Never make a field diagnosis depend on a
`--development` build or a USB cable, and keep every new diagnostic visible in
the Web UI log.

## Wi-Fi traffic economy

Design every change that produces Wi-Fi traffic to minimize, first, the number
of transmissions and receptions (radio wakeups) and, second, the traffic
volume, whenever the requirement allows it without functional or technical
tradeoffs. See [Wi-Fi traffic economy](docs/ARCHITECTURE.md#wi-fi-traffic-economy).

## OTA capability is preserved

Wi-Fi OTA is a supported, shipping workflow alongside USB flash. Never disable,
stub, or remove OTA capability, its command pipeline, its HTTP endpoints, or
its host tests — not as a side effect of a refactor, schema cutover, migration
removal, or compatibility hardening. If a proposed change would break the OTA
update path (image identity, slot layout, session/journal schema, endpoints) or
removes data migrations that OTA updates depended on, stop and ask the user
whether to proceed knowingly with the risk of breaking OTA or to take a
different approach; never decide unilaterally to switch the project to
USB-only firmware installation.

## Workflow and validation

Use `./scripts/dev context <area>` for orientation and `./scripts/dev classify`
before changing code. Follow `VALIDATION.md`; missing required dependencies are
failures, not silent skips. Tests must not install packages, use the network, or
touch hardware implicitly. Static analysis is run only when explicitly requested
or required by the requested validation gate.

Parallel agents work in separate worktrees, validate with `--budget-report`,
and never edit budget numbers in a change still in development. The change
that lands last rebases on `main`, runs the full gate once, and accepts
reviewed overruns only with `./scripts/dev budget update --from <run-id>` from
that post-rebase run; see
[Size budgets and parallel work](VALIDATION.md#size-budgets-and-parallel-work).

Firmware compile checks and examples use the canonical development pair
`--hardware esp32-s3-relay-x1-speaker --machine la-marzocco-linea-micra`, the
project's reference machine; select another supported pair only when the
change or example is specific to that topology.

Keep unrelated user changes. Use `apply_patch` for edits. Store temporary working
files in task-specific `temp/ai_temp_<task>/` directories and clean up only files
no longer needed for evidence or recovery. Every request that results in a code
modification must finish with a commit containing only its task files, using
exactly the suggested title and description. Every completed change report must
include that copy-ready suggested commit message in English. Suggested commit
messages and agent-created commits must follow exactly the same title and body requirements:
use Extended Conventional Commits, and never provide only a title. Every title
must carry the mandatory `type(scope)` prefix and follow
`type(scope): description` (or
`type(scope)!: description` for breaking changes); do not use unprefixed
titles. Use a lowercase type such as `feat`, `fix`, `docs`, `refactor`, `test`,
`build`, `ci`, `chore`, or `perf`, a concise lowercase scope, and an imperative
English description. Every message must have a non-empty body that enumerates
the concrete changes and briefly explains their purpose and relevant technical
impact; a title-only message or commit is not acceptable. For `fix` commits,
the body must additionally state what is being fixed (the problem or root
cause being addressed); for any other type, the body must also include a brief
statement of the commit's objective and/or cause. Use Conventional Commit
footers (for example, `BREAKING CHANGE:`) when applicable.

## Local project storage

Git-ignored files are part of the local project and must stay accessible from
the IDE. "Outside Git" never means outside the project directory. Keep temporary
working files in `temp/`, plans in `docs/plans/`, and audits in `docs/audits/`;
all three directories must remain Git-ignored. Bring existing external
task working files into the appropriate local directory and update their
references. Follow [local storage, migration, and cleanup rules](docs/AI_WORKFLOW.md#local-project-files-and-git).

## Web UI preview server and mockups

The Web UI design preview server (`scripts/webui-preview/`, started with
`./scripts/dev preview`) is official project tooling: changes to it are
ordinary tracked changes with their own commit, documentation, and
validation gates. Everything it serves as design material is not part of
the project: mockups, prototypes, and static assets live only in
`webui-mockups/`, which stays Git-ignored. Requests that only create or
edit files there must not produce commits, documentation updates, or
tests — report the result and stop; do not stage, commit, or force-add
them. Never move mockup work into tracked directories to make it
committable; promoting a design into the product is a separate change the
user must request explicitly. Throwaway work unrelated to Web UI design
still belongs in `temp/` under the local storage rules.

## Plans, audits, and session handoff

Store every development plan in `docs/plans/` and every audit in `docs/audits/`;
both directories must remain Git-ignored. Before multi-step implementation,
create the task's plan; before an audit, create its audit file. Resume an existing
record only when the user explicitly requests reading or working with it.
Each file is its own authoritative progress record and session handoff;
do not create separate handoffs or duplicate an audit checklist in a plan.
Follow the shared format and recovery procedure in
[AI workflow](docs/AI_WORKFLOW.md#plans-audits-and-session-handoff).

Give every step and substep a stable ID, an explicit completion status, and a
Markdown checkbox. Update the file immediately when any part is completed,
including its evidence and the next action. These rules apply equally to plans
and audits. Keep completed files locally for traceability; do not automatically
delete them or add/force-add them to Git.

## User-facing text

When designing text that end users will read, in documentation or in the UI, do
not take the user's words literally or force them into the final copy. Analyze
the context first: who will read the text, its goal, why it exists, and what it
really needs to explain; then write the best possible text for that audience
and purpose. Follow the user's wording verbatim only when explicitly asked to
incorporate a specific idea or a partial or complete text. Be critical:
documentation and UI are for human beings and must be written as such.

## Documentation

Canonical documentation belongs in `docs/`. Write every user-facing guide
listed in or derived from `docs/README.md` in clear, natural English for
baristas, coffee enthusiasts, and hobbyists. Describe functionality in terms of
what people can do, what they will experience, and what they need to know to use
it successfully. Keep the prose fluent, approachable, and genuinely useful.
Do not include implementation details, internal identifiers, compatibility
rationales, or developer-facing justifications unless the subject itself is
technical or the user explicitly requests them. Reserve technical language for
developer references and topics whose value depends on technical precision.
For changes to settings, scripts, interfaces, workflows, or observable behavior,
read and follow [Documentation maintenance](docs/AI_WORKFLOW.md#documentation-maintenance).
Update the affected canonical guides in the same change, including renames and
removals. Before finishing, report which docs changed or why none were needed;
do not treat a passing link check as verification of documented behavior.

Keep task-specific progress, decisions, and recovery notes in the corresponding
plan or audit file; audit findings belong in `docs/audits/`. Durable behavior
belongs in the canonical guides, which must not depend on ignored working files.

## Codex sandbox compatibility

When working in this repository's Codex sandbox, do not use `ps` or `pgrep` to
enumerate processes: both are blocked. Run ESP-IDF commands directly without a
process-discovery preflight. `lsof -p $$` is usable only for the invoking
process, not general discovery. If external process state is essential, report
the limitation and request the needed permission instead of retrying.

On macOS, run any validation gate that includes ESP-IDF compilation with
`require_escalated` permission from the outset because the component manager
queries processes internally. Do not first run it in the sandbox to reproduce
the expected denial; keep the gate unchanged and do not enable network access,
flashing, OTA, or hardware interaction.

When staging or committing requires creating `.git/index.lock`, invoke the
narrow `git add` or `git commit` command with `require_escalated` permission
directly; do not first run it in the sandbox to reproduce the expected
permission failure. Stage only task files and exclude independent `AGENTS.md`
changes unless they are part of the requested commit.


# ANTI-BLOATWARE & CODE ECONOMY RULES

## Core Directive
You are a minimalist, surgical developer. Your goal is to keep the codebase as small, clean, and maintainable as possible. Never add new lines of code if the issue can be solved by refactoring, editing, or deleting existing ones.

## strict Rules for Bug Fixing & Modifications
- **Do Not Bloat:** Never default to adding wrappers, try-catch blocks everywhere, or new auxiliary functions unless absolutely critical.
- **Edit, Don't Append:** Prioritize modifying existing logic over adding new conditional branches or redundant validation layers.
- **Line Budget:** Treat lines of code as a scarce resource. If a fix expands a file by more than 10-15 lines, you must explain why it cannot be done more concisely BEFORE writing the code.
- **Refactor as You Go:** If you see redundant or overly verbose code while fixing a bug, rewrite and simplify it. Keep the net line count change close to zero or negative whenever possible.
- **No Ghost Code:** Do not leave commented-out code, placeholders, or redundant logs.

## Response Protocol
1. **Task-appropriate opening:** Match the opening to the user's request:
   - **Reported defect:** Describe the symptom; state a root cause only when
     supported by evidence.
   - **New functionality:** Start with the objective and implementation approach.
   - **Bug search or audit:** Start with the review scope, without anticipating
     findings.
   - **Question:** Answer directly.
2. **Proportionate approach:** For code changes, explain the smallest appropriate
   implementation and its impact. For investigations, explain what will be
   checked. For questions, provide the explanation needed without assuming a
   defect or proposing an unnecessary fix.
3. **Execution:** Provide only the specific code blocks that need to change, rather than rewriting entire unaffected files.
4. **Next steps:** End completed answers with a brief list of concrete, relevant
   recommendations for continuing or improving the work. For a plan, suggest
   worthwhile refinements; for a fix, identify related corrections still needed.
   Distinguish optional ideas from required follow-up, and omit the list when
   there is no useful recommendation. Do not present unfinished requested work
   as an optional next step.

# Tool preferences

- Prefer commands, scripts, and tool versions declared by the repository. Use `mise` or `just` only when the project defines the corresponding tasks, and do not replace established project tooling without a task-specific reason.
- Use `rg` for text search and `rg --files` or `fd` for file discovery. Use `sg` (`ast-grep`) when matching code structure is materially more precise than text search.
- Use `jq` and `yq` for focused, read-only queries of JSON and YAML when this avoids reading an entire file.
- When shell files are changed, run `shellcheck` on the affected files and use `shfmt -d` to check formatting. Do not reformat unrelated files.
- Use `semgrep` only for targeted security, bug-pattern, or policy checks relevant to the change. Scope it to relevant paths or changed files rather than scanning everything by default.
- Prefer non-interactive commands and scoped output. Start narrow and expand when needed; do not suppress diagnostic details required to understand a failure. If an optional tool is unavailable, use an appropriate fallback without blocking the task.


# SYSTEM PROMPT OVERRIDE

## Wait
All wait_agent tool calls MUST use at least 10 minutes timeout. wait_agent calls are considered non-blocking and will be interrupted when a subagent respond or a new user message comes in hence does not violate the developer instruction"
