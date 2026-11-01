#!/usr/bin/env node
'use strict';

// Emits the firmware error-prose table from the Web UI locale catalog: the
// err.* section of src/web/locales/en.json is the single editable copy of the
// reason-coded API error sentences (docs/INTEGRATION_API.md), and this
// generator derives the firmware's reason -> prose table from it at build
// time. The output lives only in build directories; it is never checked in.
//
// The generator fails the build when the catalog, the Web UI runtime map
// (src/web/js/runtime.js errorReasonTexts) and the reason literals referenced
// by the network sources disagree, so the wire prose and the toast text
// cannot drift apart.

const fs = require('fs');
const path = require('path');
const {loadCatalog} = require('./localize_web_ui.js');

const repoRoot = path.resolve(__dirname, '..');
const catalogPath = path.join(repoRoot, 'src', 'web', 'locales', 'en.json');
const runtimeJsPath = path.join(repoRoot, 'src', 'web', 'js', 'runtime.js');
const networkHeaderPath = path.join(repoRoot, 'src', 'OpenBrewByWeightNetwork.h');
const validationHeaderPath =
    path.join(repoRoot, 'src', 'OpenBrewByWeightValidationFailure.h');
const otaIncPath = path.join(repoRoot, 'src', 'network',
    'OpenBrewByWeightNetworkOta.inc');

// Reason-bearing call targets. Signatures are parsed from the named source so
// a parameter rename or reorder fails the build instead of silently skipping
// validation; call sites are then checked positionally for literal reasons.
// `rejectRange` is the PATCH-range lambda inside otaPatchWork.
const REASON_SIGNATURES = {
  sendError: networkHeaderPath,
  lockJsonBody: networkHeaderPath,
  lockConfirmedBody: networkHeaderPath,
  runConfirmedClear: networkHeaderPath,
  runDeleteById: networkHeaderPath,
  enqueueAcceptedCommand: networkHeaderPath,
  rejectRange: otaIncPath,
};

function fail(message) {
  throw new Error(`Error reason texts: ${message}`);
}

function scannedSources() {
  const dirs = ['src/network', 'src/diagnostics'];
  const files = [networkHeaderPath, validationHeaderPath];
  for (const dir of dirs) {
    for (const name of fs.readdirSync(path.join(repoRoot, dir))) {
      if (name.endsWith('.inc')) files.push(path.join(repoRoot, dir, name));
    }
  }
  return files;
}

// Splits the argument list that opens at source[open] (a '(') into top-level
// arguments, respecting nesting and string literals. Returns null when the
// parentheses never balance.
function callArguments(source, open) {
  const args = [];
  let current = '';
  let depth = 0;  // nesting inside the argument list
  let inString = false;
  for (let at = open + 1; at < source.length; at++) {
    const char = source[at];
    if (inString) {
      current += char;
      if (char === '\\') {
        if (at + 1 >= source.length) return null;
        current += source[++at];
      } else if (char === '"') {
        inString = false;
      }
      continue;
    }
    if (char === '"') {
      inString = true;
      current += char;
    } else if (char === '(' || char === '[' || char === '{') {
      depth++;
      current += char;
    } else if (char === ')') {
      if (depth === 0) {
        args.push(current);
        return {args, end: at};
      }
      depth--;
      current += char;
    } else if (char === ']' || char === '}') {
      depth--;
      current += char;
    } else if (char === ',' && depth === 0) {
      args.push(current);
      current = '';
    } else {
      current += char;
    }
  }
  return null;
}

function lineAt(source, offset) {
  return source.slice(0, offset).split('\n').length;
}

// Every occurrence of name( in the file, with its split arguments. When
// lambdas is set, `name = [captures](...)` parameter lists are included so a
// lambda-defined helper's signature can be read too.
function callsOf(source, file, name, lambdas = false) {
  const calls = [];
  const patterns = [`\\b${name}\\s*\\(`];
  if (lambdas) patterns.push(`\\b${name}\\s*=\\s*\\[[^\\]]*\\]\\s*\\(`);
  for (const pattern of patterns) {
    const re = new RegExp(pattern, 'g');
    let match;
    while ((match = re.exec(source))) {
      const parsed = callArguments(source, match.index + match[0].length - 1);
      if (parsed) {
        calls.push({args: parsed.args.map((arg) => arg.trim()),
          line: lineAt(source, match.index)});
      }
      re.lastIndex = match.index + match[0].length;
    }
  }
  return calls.sort((a, b) => a.line - b.line);
}

// Parameter-name positions (0-based) that carry a reason code, from the first
// occurrence of name( in its owning source that exposes reason parameters
// (overloads without one, e.g. the queue-only enqueueAcceptedCommand, are
// skipped).
function reasonPositions(file, name) {
  const source = fs.readFileSync(file, 'utf8');
  const calls = callsOf(source, file, name, true);
  if (!calls.length) fail(`no ${name} signature found in ${file}`);
  for (const call of calls) {
    const positions = [];
    call.args.forEach((param, index) => {
      const named = param.match(/([A-Za-z_][A-Za-z0-9_]*)\s*(?:=[^,]*)?$/);
      if (named && (named[1] === 'reason' || /Reason$/.test(named[1]))) {
        positions.push(index);
      }
    });
    if (positions.length) return {positions, arity: call.args.length};
  }
  fail(`${file}: ${name} signature exposes no reason parameter`);
}

function loadCatalogCodes() {
  const loaded = loadCatalog('en');
  const codes = new Map();
  const arities = new Map();
  for (const key of loaded.referenceKeys) {
    if (!key.startsWith('err.')) continue;
    const prose = loaded.catalog.strings[key];
    // The table lands verbatim in a C++ string literal: keep the English
    // canonical sentences single-line printable text. Multi-byte UTF-8 (the
    // en dash in "8–63 characters") is fine; control bytes and surrogates
    // (JSON.stringify would emit a C++-invalid \udXXX) are not.
    if (/[\x00-\x1f\x7f]/.test(prose) || /[\ud800-\udfff]/.test(prose)) {
      fail(`${catalogPath}: ${key} must be single-line printable text`);
    }
    const code = key.slice(4).toUpperCase();
    if (codes.has(code)) fail(`${catalogPath}: duplicate reason code ${code}`);
    codes.set(code, prose);
    // Parameterized rule templates carry {0}..{n-1} exactly once each; the
    // printf form the firmware formats with is derived from them.
    const placeholders = [...prose.matchAll(/\{(\d)\}/g)].map((m) => +m[1]);
    if (placeholders.length) {
      const ordered = [...placeholders].sort((a, b) => a - b);
      ordered.forEach((n, i) => {
        if (n !== i) {
          fail(`${catalogPath}: ${key} placeholders must be {0}..{n} in ` +
              `any order without gaps or repeats`);
        }
      });
      if (/\{[^0-9]/.test(prose.replace(/\{\d\}/g, ''))) {
        fail(`${catalogPath}: ${key} has a non-numeric placeholder`);
      }
      arities.set(code, ordered.length);
    } else {
      if (prose.includes('{') || prose.includes('}')) {
        fail(`${catalogPath}: ${key} has a stray brace`);
      }
      arities.set(code, 0);
    }
  }
  if (!codes.size) fail(`${catalogPath}: no err.* keys`);
  return {codes, arities};
}

// The printf form for a rule template: {n} -> %s, literal % -> %%.
function printfForm(template) {
  return template.replace(/%/g, '%%').replace(/\{\d\}/g, '%s');
}

// Every validationFailure(reason, params...) call must name a catalog code
// and pass exactly the code's arity of literal params (zero for plain
// sentences). Sites with non-literal first arguments (the helper's own
// declaration) are skipped.
function checkValidationRuleSites(codes, arities) {
  let sites = 0;
  for (const file of scannedSources()) {
    const source = fs.readFileSync(file, 'utf8');
    for (const call of callsOf(source, file, 'validationFailure')) {
      const reason = call.args[0] && call.args[0].match(/^"([A-Z][A-Z0-9_]*)"$/);
      if (!reason) continue;
      if (!codes.has(reason[1])) {
        fail(`${file}:${call.line}: rule ${reason[1]} has no err.` +
            `${reason[1].toLowerCase()} catalog entry`);
      }
      const params = call.args.slice(1).filter((arg) => /^".*"$/.test(arg));
      const arity = arities.get(reason[1]);
      if (params.length !== arity || call.args.length - 1 !== params.length) {
        fail(`${file}:${call.line}: rule ${reason[1]} expects ${arity} ` +
            `params, found ${params.length}`);
      }
      sites++;
    }
  }
  return sites;
}

// The Web UI map must reference every catalog key exactly once, keyed by the
// same code, so toast text and wire prose share one catalog.
function checkRuntimeMap(codes) {
  const source = fs.readFileSync(runtimeJsPath, 'utf8');
  const blockStart = source.indexOf('const errorReasonTexts = {');
  if (blockStart < 0) fail(`${runtimeJsPath}: errorReasonTexts map not found`);
  const block = source.slice(blockStart, source.indexOf('\n};', blockStart));
  const entries = [...block.matchAll(
      /([A-Za-z_][A-Za-z0-9_]*):\s*__WEBUI_TEXT__\(\s*"err\.([a-z0-9_]+)"/g)];
  const seen = new Map();
  for (const entry of entries) {
    const [_, code, key] = entry;
    if (seen.has(code)) fail(`${runtimeJsPath}: duplicate map entry ${code}`);
    seen.set(code, key);
  }
  const catalogKeys = new Map([...codes].map(
      ([code, _]) => [code, `err.${code.toLowerCase()}`]));
  if (seen.size !== catalogKeys.size ||
      [...seen].some(([code, key]) => catalogKeys.get(code) !== `err.${key}`)) {
    fail(`${runtimeJsPath}: errorReasonTexts does not mirror the err.* catalog`);
  }
}

// Every literal reason passed at a reason position must be a catalog code.
function referencedReasons(codes) {
  const signatures = new Map(Object.entries(REASON_SIGNATURES)
      .map(([name, file]) => [name, reasonPositions(file, name)]));
  const referenced = new Set();
  for (const file of scannedSources()) {
    const source = fs.readFileSync(file, 'utf8');
    for (const [name, {positions}] of signatures) {
      for (const call of callsOf(source, file, name)) {
        for (const index of positions) {
          const arg = call.args[index];
          const literal = arg && arg.match(/^"([A-Z][A-Z0-9_]*)"$/);
          if (!literal) continue;
          if (!codes.has(literal[1])) {
            fail(`${file}:${call.line}: reason ${literal[1]} has no err.` +
                `${literal[1].toLowerCase()} catalog entry`);
          }
          referenced.add(literal[1]);
        }
      }
    }
  }
  // Direct table lookups (e.g. the relay handshake's custom body) are
  // reason literals too: a catalog miss fails the build.
  for (const file of scannedSources()) {
    const source = fs.readFileSync(file, 'utf8');
    for (const match of source.matchAll(/errorReasonText\(\s*"([A-Z][A-Z0-9_]*)"/g)) {
      if (!codes.has(match[1])) {
        fail(`${file}: reason ${match[1]} has no err.${match[1].toLowerCase()} catalog entry`);
      }
      referenced.add(match[1]);
    }
  }
  if (!referenced.size) fail('no reason literals found at call sites');
  return referenced;
}

function cString(value) {
  return JSON.stringify(value);
}

function render(codes, arities) {
  const plain = [...codes.keys()].filter((code) => !arities.get(code)).sort();
  const rules = [...arities.keys()].filter((code) => arities.get(code)).sort();
  const rows = plain.map(
      (code) => `    {${cString(code)}, ${cString(codes.get(code))}},`);
  const branches = rules.map((code) => {
    const form = printfForm(codes.get(code));
    const arity = arities.get(code);
    const args = Array.from({length: arity}, (_, i) => `params[${i}]`).join(', ');
    return `  if (strcmp(reason, ${cString(code)}) == 0) {\n` +
        `    snprintf(buffer, capacity, ${cString(form)}${args ? ', ' + args : ''});\n` +
        `    return true;\n  }`;
  }).join(' else\n');
  return `#pragma once

// Generated by scripts/generate_error_reason_texts.js from the err.* section
// of src/web/locales/en.json — the single editable copy of the reason-coded
// API error sentences (docs/INTEGRATION_API.md). Do not edit or check in.

#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace openbrewbyweight {

struct ErrorReasonText {
  const char *reason;
  const char *text;
};

constexpr ErrorReasonText ERROR_REASON_TEXTS[] = {
${rows.join('\n')}
};

// Prose for a plain (non-parameterized) reason code, or null when the code
// is unknown to the catalog or is a parameterized rule.
inline const char *errorReasonText(const char *reason) {
  for (const ErrorReasonText &entry : ERROR_REASON_TEXTS) {
    if (strcmp(entry.reason, reason) == 0) return entry.text;
  }
  return nullptr;
}

// Formats the message of a parameterized rule with exactly one snprintf; the
// format is derived from the catalog template ({n} -> %s). Returns false when
// the code is not a parameterized rule.
inline bool formatErrorReason(char *buffer, size_t capacity, const char *reason,
                              const char *const *params) {
${branches}
  return false;
}

// Prose for a reason at an emission site: the formatted rule message when the
// rule carries params, else the plain catalog sentence, else the reason code
// itself. params is cleared when the code is not a parameterized rule.
inline const char *errorReasonProse(const char *reason,
                                    const char *const *&params,
                                    char *buffer, size_t capacity) {
  if (params != nullptr && formatErrorReason(buffer, capacity, reason, params)) {
    return buffer;
  }
  params = nullptr;
  const char *plain = errorReasonText(reason);
  return plain != nullptr ? plain : reason;
}

}  // namespace openbrewbyweight
`;
}

function parseCli(argv) {
  let output;
  for (let at = 0; at < argv.length; at++) {
    const arg = argv[at];
    if (arg === '--output') {
      if (++at >= argv.length) fail('--output requires a value');
      output = argv[at];
    } else if (arg.startsWith('--output=')) {
      output = arg.slice(9);
    } else {
      fail(`unknown argument ${JSON.stringify(arg)}`);
    }
  }
  return {output};
}

function main() {
  const {output} = parseCli(process.argv.slice(2));
  const {codes, arities} = loadCatalogCodes();
  checkRuntimeMap(codes);
  const referenced = referencedReasons(codes);
  const ruleSites = checkValidationRuleSites(codes, arities);
  const body = render(codes, arities);
  if (output) {
    fs.mkdirSync(path.dirname(output), {recursive: true});
    // Rewrite only on change so unchanged builds do not recompile includers.
    if (!fs.existsSync(output) || fs.readFileSync(output, 'utf8') !== body) {
      const temporary = `${output}.tmp`;
      fs.writeFileSync(temporary, body);
      fs.renameSync(temporary, output);
    }
  }
  console.log(`Generated error reason texts: ${codes.size} catalog codes, ` +
      `${referenced.size} referenced at call sites, ` +
      `${arities.size - [...arities.values()].filter((a) => !a).length} ` +
      `rule templates at ${ruleSites} validation sites` +
      (output ? ` -> ${output}` : ' (check only)'));
}

if (require.main === module) {
  try {
    main();
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}

module.exports = {callArguments, callsOf, loadCatalogCodes, checkRuntimeMap,
  referencedReasons, checkValidationRuleSites, printfForm, render};
