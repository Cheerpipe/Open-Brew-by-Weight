'use strict';

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..', '..');
const slotLimits = {n16r8: 3670016};
const binPath = process.argv[2] || path.join(root, 'build-idf', 'n16r8', 'openbrewbyweight.bin');
const normalized = binPath.replace(/\\/g, '/');
const explicitArch = process.argv[3] === '--arch' ? process.argv[4] : '';
const arch = slotLimits[explicitArch] ? explicitArch :
  normalized.includes('/n16r8/') ? 'n16r8' : null;

if (!arch) {
  console.error('Firmware path must identify n16r8');
  process.exit(2);
}
const sizePath = path.join(path.dirname(binPath), 'size.json');
const sdkconfigPath = path.join(path.dirname(binPath), 'sdkconfig');
if (!fs.existsSync(binPath) || !fs.existsSync(sizePath) || !fs.existsSync(sdkconfigPath)) {
  console.error(`Firmware resource artifacts missing: ${binPath}, ${sizePath}, ${sdkconfigPath}`);
  process.exit(127);
}
const optimization = fs.readFileSync(sdkconfigPath, 'utf8').match(
  /^CONFIG_COMPILER_OPTIMIZATION_(SIZE|PERF|DEBUG|NONE)=y$/gm) || [];
if (optimization.length !== 1) {
  console.error(`Firmware optimization config must select one supported level: ${sdkconfigPath}`);
  process.exit(2);
}
const config = JSON.parse(fs.readFileSync(
  path.join(root, 'config', 'resource-baselines.json'), 'utf8'));
// Development builds compare the shared arch baseline; flag-free release
// builds of an official pair compare that pair's shipped-image baseline.
const baselines = optimization[0] !== 'CONFIG_COMPILER_OPTIMIZATION_PERF=y' ? null :
  process.argv.includes('--development') ? config.targets[arch] :
  process.argv.includes('--release') ?
    config.releaseTargets[path.basename(path.dirname(binPath))] : null;
const qualifiedSizeProfile = Boolean(baselines);
const unqualified = 'no versioned baseline for this build profile; budget evidence ' +
  'requires a --development build or a flag-free release build of an official pair';
const actual = JSON.parse(fs.readFileSync(sizePath, 'utf8'));
if (Array.isArray(actual.layout)) {
  const regions = Object.fromEntries(actual.layout.map(region => [region.name, region]));
  actual.used_diram = regions.DIRAM?.used;
  actual.flash_code = regions['Flash Code']?.parts['.text']?.size;
  actual.flash_rodata = regions['Flash Data']?.parts['.rodata']?.size;
}
actual.image = fs.statSync(binPath).size;
const failures = [];
const map = fs.readFileSync(path.join(path.dirname(binPath), 'openbrewbyweight.map'), 'utf8');
function symbolAddress(name) {
  const match = map.match(new RegExp('^\\s*(0x[0-9a-fA-F]+)\\s+' + name + '(?:\\s|$)', 'm'));
  if (!match) throw new Error(`Required memory symbol missing: ${name}`);
  return Number.parseInt(match[1], 16);
}
const externalBssBytes = symbolAddress('_ext_ram_bss_end') - symbolAddress('_ext_ram_bss_start');
if (externalBssBytes < 0 || externalBssBytes > config.maximumExternalBssBytes) {
  failures.push(`external BSS ${externalBssBytes} > budget ${config.maximumExternalBssBytes}`);
}
// The no-init segment (kept across resets) is outside external BSS.
const externalNoinitBytes =
    symbolAddress('_ext_ram_noinit_end') - symbolAddress('_ext_ram_noinit_start');
if (externalNoinitBytes < 0 || externalNoinitBytes > config.maximumExternalNoinitBytes) {
  failures.push(`external no-init ${externalNoinitBytes} > budget ${config.maximumExternalNoinitBytes}`);
}
for (const name of ['localBuzzer', 'taskProfiler', 'publishedControlStatus',
                    'controlStatusMutex', 'publishedControlGate', 'controlGateMutex']) {
  const address = symbolAddress(name);
  if (address < 0x3fc80000 || address >= 0x3fd00000) {
    failures.push(`${name} must remain in internal SRAM`);
  }
}
for (const name of ['publishedControlCurve', 'taskProfilerReport']) {
  const address = symbolAddress(name);
  if (address < symbolAddress('_ext_ram_bss_start') ||
      address >= symbolAddress('_ext_ram_bss_end')) {
    failures.push(`${name} must reside in external BSS`);
  }
}
console.log(`external BSS: ${externalBssBytes} (budget ${config.maximumExternalBssBytes})`);
console.log(`external no-init: ${externalNoinitBytes} (budget ${config.maximumExternalNoinitBytes})`);
if (actual.image > slotLimits[arch]) {
  failures.push(`image ${actual.image} > OTA slot ${slotLimits[arch]}`);
}
// A deferred gate records baseline overruns and judges them once at its end;
// the OTA slot and memory placement checks above always fail at once.
const deferred = process.env.OPENBREWBYWEIGHT_BUDGET_DEFER === '1';
const rows = [];
let over = 0;
for (const [metric, baseline] of Object.entries(baselines || config.targets[arch])) {
  const value = actual[metric];
  const limit = baseline + config.allowedGrowthBytes[metric];
  const exceeded = qualifiedSizeProfile && value > limit;
  over += exceeded;
  if (!Number.isFinite(value) || value < 0) failures.push(`${metric} is missing or invalid`);
  else if (exceeded && !deferred) failures.push(`${metric} ${value} > baseline budget ${limit}`);
  else console.log(qualifiedSizeProfile
      ? `${exceeded ? 'BUDGET EXCEEDED ' : ''}${metric}: ${value} (baseline ${baseline}, delta ${value - baseline}, limit ${limit})`
      : `${metric}: ${value} (${unqualified})`);
  rows.push({metric, value, baseline});
}
if (qualifiedSizeProfile && !failures.length && process.env.OPENBREWBYWEIGHT_RUN_DIR) {
  const profile = process.argv.includes('--development') ? 'development' : 'release';
  const target = profile === 'development' ? `targets.${arch}` :
    `releaseTargets.${path.basename(path.dirname(binPath))}`;
  const directory = path.join(process.env.OPENBREWBYWEIGHT_RUN_DIR, 'budgets');
  fs.mkdirSync(directory, {recursive: true});
  fs.writeFileSync(path.join(directory, `firmware-${profile}-${path.basename(path.dirname(binPath))}.json`),
      JSON.stringify({kind: 'firmware', target, rows}, null, 2) + '\n');
}
if (failures.length) throw new Error(failures.join('; '));
console.log(!qualifiedSizeProfile ? `${arch}: ${unqualified}; OTA slot and memory checks passed` :
  over ? `${arch}: ${over} budget overrun(s) deferred to the gate's budget verdict` :
  `${arch}: image and memory regions are within versioned budgets`);
