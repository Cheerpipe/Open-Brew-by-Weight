# BLE library instructions

- Editable library code is under `src/`; host tests and stubs are under `tests/`.
- This firmware is the library's only consumer: treat its headers as internal
  API, not a public one. Remove API the firmware no longer calls, together with
  its README and test coverage, instead of keeping it for outside users.
- Preserve protocol behavior, disconnect/reconnect generation guards, and
  callback lifetime rules. BLE changes are R2 unless they cross a
  machine/safety boundary, then R3.
- Do not load vendored or generated dependency trees. Validate portable state
  and protocol tests with the BLE CTest labels described in `VALIDATION.md`.
