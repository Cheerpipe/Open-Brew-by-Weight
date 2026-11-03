// ESLint for the embedded Web UI sources, run by ./scripts/dev test lint.
// Correctness rules only: formatting belongs to Prettier (npm run format:web).
import js from "@eslint/js";
import globals from "globals";

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
    },
  },
];
