# Web UI instructions

- Edit `src/web/css/`, `src/web/app.js`, `src/web/html/`, and `src/web/js/`.
  Never edit generated `src/OpenBrewByWeightWebAssetsGzip.h` by hand.
- The stylesheet ships as one `/app.css`: `CSS_SOURCES` in
  `scripts/localize_web_ui.js` owns the layer order — tokens, shell, shared
  components, views, global — and the generator fails on any `src/web/css`
  file outside that list. Place new CSS where it is owned: code shared by two
  or more views goes in `components/`, one view's code in its `views/` file.
  A component's states, dark-mode rules, and media queries stay with the
  component; never add `responsive.css`, `dark.css`, or catch-all files.
- Sources stay Prettier-formatted: run `npm run format:web` after edits. Only
  the generator compacts them, so never hand-minify markup or CSS; contracts
  match the compact form the fixtures derive, not the source text.
- Preserve progressive enhancement, admin-lock behavior, remote-actuation
  lockdown, endpoint schemas, and the embedded asset budget.
- `node_modules/` is dependency output and must not be loaded wholesale.
  Bootstrap explicitly with `npm ci`; tests never install packages.
- `js/runtime.js` is always loaded: keep in it only code shared by Home, the
  shell, or several views. Code one view uses belongs in that view's module;
  mutable state that views update lives in the exported `uiState` object.
- Web-only changes are R1 unless they touch OTA or remote control, which are at
  least R2 and R3 respectively.
