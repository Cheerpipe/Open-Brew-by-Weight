// ESLint for the embedded Web UI sources, run by ./scripts/dev test lint.
// Correctness rules only: formatting belongs to Prettier (npm run format:web).
import js from "@eslint/js";
import globals from "globals";

const RECORDS_PRELUDE = ["downloadCsv", "actionMenu", "ICON_TRASH", "createFilterMenu", "armListSentinel"];

export default [
  {
    files: ["src/web/**/*.js"],
    languageOptions: {
      ecmaVersion: 2024,
      sourceType: "module",
      globals: {
        ...globals.browser,
        // Build-time constants and markers that scripts/gen_web_ui.js and
        // scripts/localize_web_ui.js replace or fold before minification.
        __WEBUI_TEXT__: "readonly",
        __LINEA_MICRA__: "readonly",
        __REMOTE_CONTROL__: "readonly",
        __homeModule: "readonly",
        __SW_ASSETS__: "readonly",
        __SHELL_PATHS__: "readonly",
      },
    },
    linterOptions: { reportUnusedDisableDirectives: "error" },
    rules: {
      ...js.configs.recommended.rules,
      "no-unused-vars": ["error", { caughtErrors: "none" }],
      "no-empty": ["error", { allowEmptyCatch: true }],
    },
  },
  // The generator prepends js/records.js to stats.js and history.js, so its
  // helpers are globals there and look unused in the prelude itself.
  {
    files: ["src/web/js/stats.js", "src/web/js/history.js"],
    languageOptions: { globals: Object.fromEntries(RECORDS_PRELUDE.map((n) => [n, "readonly"])) },
  },
  {
    files: ["src/web/js/records.js"],
    rules: {
      "no-unused-vars": [
        "error",
        { caughtErrors: "none", varsIgnorePattern: `^(${RECORDS_PRELUDE.join("|")})$` },
      ],
    },
  },
];
