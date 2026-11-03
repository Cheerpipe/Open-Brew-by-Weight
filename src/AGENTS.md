# Firmware instructions

- Editable sources are `src/*.{h,cpp,ino}`; generated
  `OpenBrewByWeightVersion.h` must be regenerated, not hand-edited. The Web UI
  header and source are generated into each build tree.
- Read `docs/ARCHITECTURE.md`, `docs/STATE_MACHINES.md`, and
  `docs/RESOURCE_OWNERSHIP.md` only when the affected boundary requires them.
- Control, safety, machine, relay, ISR, watchdog, boot, GPIO, and remote-control
  changes are R3. Preserve the root safety invariants and external HTTP, BLE,
  OTA, serial, persistence, and Web UI contracts.
- Keep target-specific code behind existing boundaries. Run the gate selected by
  `VALIDATION.md`; record HIL/manual work as pending rather than simulating it.
- When changing an HTTP route or handler definition, run `./scripts/dev test web`;
  its `http-route-capacity` check must pass before broader Web contracts.
