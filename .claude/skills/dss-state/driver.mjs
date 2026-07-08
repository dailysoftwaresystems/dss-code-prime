#!/usr/bin/env node
// dss-state driver — measures the CURRENT state of the DSS Code Prime
// compiler by driving the real CLI, and derives every % axis live:
//
//   1. C-feature battery (probes.mjs) compiled + RUN through the actual
//      `dss-code-prime` binary on the host target → empirical coverage %,
//      a parse→semantic→codegen→runtime funnel, and a miscompile alarm.
//   2. Cross-target emit/run matrix (5 exec specs) with a canonical program.
//   3. Plan axis: `.plans/23-full-c-plan - tbd.md` FC-phase ✅/⏳ markers,
//      cycle-weighted via the calibration table below.
//   4. Anchor axis: `.plans/_deferred-anchor-registry.md` open vs ✅ rows.
//   5. Test axis: ctest LastTest.log (or `--ctest` for a live run).
//   6. Velocity: git commits matching /^v0.0.2/ (one commit per dev cycle
//      by repo convention) → cycles/day → ETA extrapolation for the
//      SQLite milestone and the full-C arc.
//
// Usage: node .claude/skills/dss-state/driver.mjs [--quick] [--ctest]
//        [--json <path>] [--md <path>] [--cli <exe>] [--jobs N] [--keep]
//        [--strict]
//
// Pure Node (>=18, tested on 22), zero dependencies. Windows-first (this
// is the dev box); host-target selection also handles linux/darwin.

import { spawn, spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync, existsSync, statSync, rmSync, readdirSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import os from 'node:os';
import { PROBES, CATEGORIES, CANONICAL_SRC } from './probes.mjs';

// ─────────────────────────── calibration ───────────────────────────
// Cycle-cost estimate per plan-23 FC phase (one "cycle" = one /dss-cycle
// green push — the repo's unit of work, one commit each). Done-phase
// estimates are calibrated against what actually happened (FC1=2 cycles,
// FC2=2, FC3=2, FC3.5=3, FC4=3). If plan-23 grows/renames phases, the
// driver FAILS LOUD below so this table never drifts silently.
const PHASE_CYCLE_EST = {
  '1': 2, '2': 2, '3': 2, '3.5': 3, '4': 3,            // cluster A (done part)
  '5': 2,                                               // cluster A remainder
  '6': 3, '7': 2, '8': 3,                               // cluster B aggregates backend
  '9': 4, '10': 2, '11': 3, '12a': 3, '12b': 2, '12c': 2, // cluster C ABI
  '13': 4, '14': 2, '15': 2,                            // cluster D preprocessor
  '16': 4, '17': 2, '17.5': 2, '18': 3,                 // cluster E completeness (+FC17.5 conformance sweep)
};
// SQLite milestone = "compile + link + run the sqlite3.c amalgamation
// (single TU, SQLITE_THREADSAFE=0, SQLITE_OMIT_LOADEXT) + a tiny shell
// driver on ONE target". Phase prerequisites per route:
const SQLITE_PATHS = {
  'linux-elf (SysV ABI)':   ['5', '6', '7', '8', '9', '12a', '13', '14', '15', '16'],
  'windows-pe (Win64 ABI)': ['5', '6', '7', '8', '10', '12b', '13', '14', '15', '16'],
};
const SQLITE_EXTRA_CYCLES = 2; // libc-FFI breadth + amalgamation-scale shakeout

// Velocity calibration. Git history can NOT supply cycle velocity directly:
// PRs are squash-merged, so one main commit absorbs many /dss-cycle cycles.
// Primary velocity therefore comes from the PLAN: completed-phase cycle
// estimates / calendar days since the FC arc opened. The git commit cadence
// is shown as a cross-check floor only.
const PLAN_ARC_START = '2026-06-10'; // FC1 cycle 1 shipped (plan-23 §0 status)
const SUSTAIN_FACTOR = 0.65; // steady-pace haircut vs the burst: the wider
// v0.0.2 window (2026-06-02→06-12, pre-FC arcs included) sustained roughly
// two-thirds of the FC-burst cadence; revisit as the arc ages.

// ───────────────────────────── helpers ─────────────────────────────
const stripAnsi = (s) => s.replace(/\x1b\[[0-9;]*m/g, '');
const pct = (n, d) => (d === 0 ? '—' : `${Math.round((n / d) * 1000) / 10}%`);
const today = () => new Date();
const isoDate = (d) => d.toISOString().slice(0, 10);
const addDays = (d, n) => new Date(d.getTime() + n * 86400000);

function run(cmd, args, { timeoutMs = 60000, cwd } = {}) {
  return new Promise((res) => {
    let out = '', err = '', done = false, timedOut = false;
    const child = spawn(cmd, args, { cwd, windowsHide: true });
    const timer = setTimeout(() => { timedOut = true; try { child.kill(); } catch {} }, timeoutMs);
    child.stdout.on('data', (d) => (out += d));
    child.stderr.on('data', (d) => (err += d));
    child.on('error', (e) => { if (!done) { done = true; clearTimeout(timer); res({ rc: null, out, err: err + String(e), timedOut, spawnError: true }); } });
    child.on('close', (code) => { if (!done) { done = true; clearTimeout(timer); res({ rc: code, out, err, timedOut }); } });
  });
}

async function pool(items, n, fn) {
  const results = new Array(items.length);
  let i = 0;
  const workers = Array.from({ length: Math.min(n, items.length) }, async () => {
    while (i < items.length) { const idx = i++; results[idx] = await fn(items[idx], idx); }
  });
  await Promise.all(workers);
  return results;
}

// Diagnostic-code first letter → pipeline tier (codes render as
// `error[S_UndeclaredIdentifier]` or numeric-band `error[P000E]`).
const TIER_ORDER = ['lex', 'parse', 'semantic', 'ir', 'backend', 'driver', 'other'];
function tierOfLetter(l) {
  if (l === 'T') return 'lex';
  if (l === 'P') return 'parse';
  if (l === 'S') return 'semantic';
  if (l === 'H' || l === 'I' || l === 'M') return 'ir';
  if (l === 'L' || l === 'A' || l === 'E' || l === 'K') return 'backend';
  if (l === 'D' || l === 'C') return 'driver';
  return 'other';
}
function classifyReject(stderrRaw) {
  const err = stripAnsi(stderrRaw);
  const tiers = [...err.matchAll(/error\[([A-Z])[A-Za-z0-9_]*\]/g)].map((m) => tierOfLetter(m[1]));
  const first = [...err.matchAll(/error\[([A-Za-z0-9_]+)\]/g)].map((m) => m[1]);
  const tier = tiers.length
    ? TIER_ORDER[Math.min(...tiers.map((t) => TIER_ORDER.indexOf(t)))]
    : 'other';
  return { tier, code: first[0] ?? '(no error code parsed)' };
}

// ─────────────────────────── environment ───────────────────────────
const scriptDir = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(scriptDir, '..', '..', '..');
if (!existsSync(join(repoRoot, '.plans'))) {
  console.error(`fatal: repo root not found at ${repoRoot} (no .plans/)`); process.exit(2);
}

const argv = process.argv.slice(2);
const flag = (f) => argv.includes(f);
const opt = (f) => { const i = argv.indexOf(f); return i >= 0 && argv[i + 1] ? argv[i + 1] : undefined; };
if (flag('--help')) {
  console.log('usage: node driver.mjs [--quick] [--ctest] [--json <path>] [--md <path>] [--cli <exe>] [--jobs N] [--keep] [--strict]');
  process.exit(0);
}
const QUICK = flag('--quick');
const JOBS = Number(opt('--jobs') ?? Math.min(8, (os.availableParallelism?.() ?? 8)));

function findCli() {
  const explicit = opt('--cli');
  if (explicit) {
    if (!existsSync(explicit)) { console.error(`fatal: --cli path not found: ${explicit}`); process.exit(2); }
    return explicit;
  }
  const exe = process.platform === 'win32' ? 'dss-code-prime.exe' : 'dss-code-prime';
  const candidates = [
    join(repoRoot, 'build', 'bin', 'dss', 'Debug', exe),
    join(repoRoot, 'build', 'bin', 'dss', 'Release', exe),
    join(repoRoot, 'build', 'bin', 'dss', exe),
    join(repoRoot, 'build-rel', 'bin', 'dss', exe),
    join(repoRoot, 'build-dbg', 'bin', 'dss', exe),
  ].filter(existsSync);
  if (!candidates.length) {
    console.error('fatal: no dss-code-prime binary found. Build one, e.g.:\n  cmake --build build --config Debug --target dss-code-prime');
    process.exit(2);
  }
  candidates.sort((a, b) => statSync(b).mtimeMs - statSync(a).mtimeMs);
  return candidates[0];
}

function hostTarget() {
  const arch = os.arch();
  if (process.platform === 'win32') return { spec: 'x86_64:pe64-x86_64-windows-exec', art: 'main.exe' };
  if (process.platform === 'linux')
    return arch === 'arm64'
      ? { spec: 'arm64:elf64-aarch64-linux-exec', art: 'main' }
      : { spec: 'x86_64:elf64-x86_64-linux-exec', art: 'main' };
  if (process.platform === 'darwin')
    return arch === 'arm64'
      ? { spec: 'arm64:macho64-arm64-darwin-exec', art: 'main' }
      : { spec: 'x86_64:macho64-x86_64-darwin-exec', art: 'main' };
  console.error(`fatal: unsupported host platform ${process.platform}`); process.exit(2);
}

const toWslPath = (p) => '/mnt/' + p[0].toLowerCase() + p.slice(2).replace(/\\/g, '/');
function wslAvailable() {
  if (process.platform !== 'win32' || QUICK || flag('--no-wsl')) return false;
  try { return spawnSync('wsl', ['-e', 'true'], { timeout: 15000 }).status === 0; } catch { return false; }
}

function git(...args) {
  const r = spawnSync('git', args, { cwd: repoRoot, encoding: 'utf8', timeout: 30000 });
  return r.status === 0 ? r.stdout.trim() : null;
}

// ───────────────────────── battery execution ─────────────────────────
async function runProbe(cli, host, tmpRoot, p) {
  const dir = join(tmpRoot, 'probes', p.id);
  const outDir = join(dir, 'out');
  mkdirSync(outDir, { recursive: true });
  const files = p.files ?? { 'main.c': p.src };
  const srcPaths = [];
  for (const [name, content] of Object.entries(files)) {
    const fp = join(dir, name);
    writeFileSync(fp, content);
    srcPaths.push(fp);
  }
  const c = await run(cli, ['--compile', ...srcPaths, '--language', 'c-subset', '--target', host.spec, '--output', outDir], { timeoutMs: 60000 });
  if (c.timedOut) return { ...p, status: 'toolfail', detail: 'compiler timeout' };
  if (c.spawnError) return { ...p, status: 'toolfail', detail: 'compiler spawn failed' };
  if (c.rc !== 0) {
    const { tier, code } = classifyReject(c.err + c.out);
    return { ...p, status: 'rejected', tier, detail: code };
  }
  const art = join(outDir, host.art);
  if (!existsSync(art)) return { ...p, status: 'toolfail', detail: `rc=0 but no artifact ${host.art}` };
  const r = await run(art, [], { timeoutMs: 15000, cwd: outDir });
  if (r.timedOut) return { ...p, status: 'crash', detail: 'runtime timeout (likely bad control flow)' };
  if (r.spawnError) return { ...p, status: 'crash', detail: 'binary failed to spawn' };
  const code = r.rc;
  const stdoutOk = p.stdout === undefined || r.out.replace(/\r\n/g, '\n') === p.stdout;
  if (code === p.expect && stdoutOk) return { ...p, status: 'pass' };
  if (code !== null && (code < 0 || code > 255))
    return { ...p, status: 'crash', detail: `exit 0x${(code >>> 0).toString(16)} (hardware fault)` };
  return { ...p, status: 'miscompile', detail: `expected exit ${p.expect}${p.stdout !== undefined ? ` + stdout ${JSON.stringify(p.stdout)}` : ''}, got exit ${code}${stdoutOk ? '' : ` + stdout ${JSON.stringify(r.out.replace(/\r\n/g, '\n'))}`}` };
}

// ───────────────────────── cross-target matrix ─────────────────────────
async function runMatrix(cli, tmpRoot, wsl) {
  // The CLI names the artifact after the FIRST source file's stem — so the
  // canonical source MUST be `main.c` for the `main(.exe)` lookups below.
  const srcDir = join(tmpRoot, 'matrix-src');
  mkdirSync(srcDir, { recursive: true });
  const src = join(srcDir, 'main.c');
  writeFileSync(src, CANONICAL_SRC);
  const rows = [
    { spec: 'x86_64:pe64-x86_64-windows-exec', label: 'x86_64 / PE (Windows)', art: 'main.exe', how: process.platform === 'win32' ? 'native' : null },
    { spec: 'x86_64:elf64-x86_64-linux-exec', label: 'x86_64 / ELF (Linux)', art: 'main', how: process.platform === 'linux' && os.arch() === 'x64' ? 'native' : (wsl ? 'wsl' : null) },
    { spec: 'arm64:elf64-aarch64-linux-exec', label: 'arm64 / ELF (Linux)', art: 'main', how: wsl ? 'wsl-qemu' : null },
    { spec: 'x86_64:macho64-x86_64-darwin-exec', label: 'x86_64 / Mach-O (macOS)', art: 'main', how: null, note: 'no runner here AND no macOS-x86_64 CI leg (known gap)' },
    { spec: 'arm64:macho64-arm64-darwin-exec', label: 'arm64 / Mach-O (macOS)', art: 'main', how: null, note: 'runtime is witnessed by the macos-latest CI leg' },
  ];
  return pool(rows, 3, async (row) => {
    const outDir = join(tmpRoot, 'matrix', row.spec.replace(/[:*]/g, '_'));
    mkdirSync(outDir, { recursive: true });
    const c = await run(cli, ['--compile', src, '--language', 'c-subset', '--target', row.spec, '--output', outDir], { timeoutMs: 60000 });
    if (c.rc !== 0) return { ...row, emit: false, run: 'n/a' };
    const art = join(outDir, row.art);
    if (!existsSync(art)) return { ...row, emit: false, run: 'n/a' };
    if (row.how === 'native') {
      const r = await run(art, [], { timeoutMs: 15000 });
      return { ...row, emit: true, run: r.rc === 42 ? 'exit 42 ✓' : `WRONG exit ${r.rc}` };
    }
    if (row.how === 'wsl' || row.how === 'wsl-qemu') {
      const wp = toWslPath(art);
      await run('wsl', ['-e', 'chmod', '+x', wp], { timeoutMs: 20000 });
      // DSS ELF executables are DYNAMIC (every program imports libc `exit`),
      // so qemu-aarch64 needs an aarch64 sysroot for the loader + libc.
      const args = row.how === 'wsl-qemu'
        ? ['-e', 'qemu-aarch64', '-L', '/usr/aarch64-linux-gnu', wp]
        : ['-e', wp];
      const r = await run('wsl', args, { timeoutMs: 30000 });
      if (r.rc === 42) return { ...row, emit: true, run: `exit 42 ✓ (${row.how})` };
      if (r.rc === 127 || /not found|No such file|Could not open/i.test(stripAnsi(r.err)))
        return { ...row, emit: true, run: `skipped (${row.how} runner unavailable — need qemu-aarch64 + aarch64 sysroot in WSL)` };
      return { ...row, emit: true, run: `WRONG exit ${r.rc} (${row.how})` };
    }
    return { ...row, emit: true, run: `emit-only — ${row.note ?? 'no runner on this host'}` };
  });
}

// ─────────────────────────── static axes ───────────────────────────
function parsePlan23() {
  const p = join(repoRoot, '.plans', '23-full-c-plan - tbd.md');
  if (!existsSync(p)) { console.error('fatal: plan-23 not found — plan axis unavailable'); process.exit(2); }
  const phases = [];
  for (const line of readFileSync(p, 'utf8').split('\n')) {
    if (!/^\|\s*\*\*FC/.test(line)) continue;
    const id = line.match(/\*\*FC([0-9]+(?:\.[0-9]+)?[a-c]?)\*\*/)?.[1];
    if (!id) continue;
    const m = line.match(/(✅|🟡|⏳)/);
    phases.push({ id, status: m ? m[1] : '?' });
  }
  if (phases.length < 15) { console.error(`fatal: plan-23 parse found only ${phases.length} FC rows — format drift, fix parsePlan23()`); process.exit(2); }
  const unknown = phases.filter((ph) => !(ph.id in PHASE_CYCLE_EST));
  return { phases, drift: unknown.map((u) => u.id) };
}

function parseRegistry() {
  const p = join(repoRoot, '.plans', '_deferred-anchor-registry.md');
  if (!existsSync(p)) return null;
  let total = 0, closed = 0;
  for (const line of readFileSync(p, 'utf8').split('\n')) {
    if (!/^\|\s*`D-/.test(line)) continue;
    total++;
    if (line.includes('✅')) closed++;
  }
  return { total, closed };
}

// The ctest axes MUST target the SAME build dir the chosen CLI came from —
// NOT a hardcoded `build/`. The repo can carry several build dirs (the MSVC
// `build/` is often a STALE/broken config that can't compile current src,
// while the live work happens in a Ninja `build-dbg/`). Deriving the build
// root from `cli` keeps the test count honest: the CLI lives at
// `<buildRoot>/bin/dss/[Debug|Release/]dss-code-prime[.exe]`, so the build
// root is the path component just above `bin/dss`, and a `Debug`/`Release`
// segment after it (if present) means a multi-config generator needing `-C`.
function buildInfoOf(cliPath) {
  const norm = cliPath.replace(/\\/g, '/');
  const m = norm.match(/^(.*)\/bin\/dss(?:\/(Debug|Release))?\/[^/]+$/);
  if (!m) return { root: join(repoRoot, 'build'), config: 'Debug' }; // fallback
  return { root: m[1], config: m[2] ?? null };
}

function parseCtestLog(buildRoot) {
  const p = join(buildRoot, 'Testing', 'Temporary', 'LastTest.log');
  if (!existsSync(p)) return null;
  const txt = readFileSync(p, 'utf8');
  const passed = (txt.match(/^Test Passed\.\r?$/gm) ?? []).length;
  const failed = (txt.match(/^Test Failed\.\r?$/gm) ?? []).length;
  return { passed, failed, asOf: statSync(p).mtime.toISOString().slice(0, 16).replace('T', ' ') };
}

async function runCtestLive(buildRoot, config) {
  console.error(`running full ctest suite in ${buildRoot} (this takes a few minutes)…`);
  const cArgs = config ? ['-C', config] : []; // single-config (Ninja) takes no -C
  const r = await run('ctest', ['--test-dir', buildRoot, ...cArgs, '-j', String(os.availableParallelism?.() ?? 8)], { timeoutMs: 2400000 });
  const m = (r.out + r.err).match(/(\d+)% tests passed, (\d+) tests failed out of (\d+)/);
  if (!m) return { passed: 0, failed: -1, asOf: 'live run — SUMMARY PARSE FAILED (see ctest output)' };
  const total = Number(m[3]), failed = Number(m[2]);
  return { passed: total - failed, failed, asOf: 'live run (just now)' };
}

function gitVelocity() {
  const head = git('rev-parse', '--short', 'HEAD') ?? '?';
  const branch = git('branch', '--show-current') ?? '?';
  const log = git('log', '--pretty=%cI\t%s', '-n', '500') ?? '';
  const cycles = [];
  for (const line of log.split('\n')) {
    const [date, ...rest] = line.split('\t');
    const subj = rest.join('\t');
    if (/^v0\.0\.2\b/.test(subj)) cycles.push({ date: date.slice(0, 10), subj });
  }
  if (!cycles.length) return { head, branch, cycles: 0 };
  const days = [...new Set(cycles.map((c) => c.date))];
  const first = cycles[cycles.length - 1].date;
  const calendarDays = Math.max(1, Math.round((today() - new Date(first)) / 86400000) + 1);
  return {
    head, branch,
    cycles: cycles.length,
    activeDays: days.length,
    calendarDays,
    firstCycle: first,
    vActive: cycles.length / days.length,
    vCalendar: cycles.length / calendarDays,
  };
}

// ───────────────────────────── main ─────────────────────────────
const cli = findCli();
const host = hostTarget();
const cliMtime = statSync(cli).mtime;
// Stale = the built ENGINE is older than the newest C++ source ON DISK
// (worktree truth, not commit timestamps — those skew by minutes when the
// binary is built during the cycle and committed right after). Two quirks:
//  * the exe is a thin main over dss-code-prime.dll/.so — compare the
//    newest of the exe + its engine sibling, not the exe alone;
//  * .json configs are loaded at RUNTIME, so they never stale the binary.
function newestCodeMtimeUnder(dir) {
  let newest = 0;
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) newest = Math.max(newest, newestCodeMtimeUnder(p));
    else if (/\.(cpp|hpp|c|h|inl)$/.test(e.name)) newest = Math.max(newest, statSync(p).mtimeMs);
  }
  return newest;
}
const engineSiblings = ['dss-code-prime.dll', 'libdss-code-prime.so', 'libdss-code-prime.dylib']
  .map((n) => join(dirname(cli), n)).filter(existsSync);
const binMtime = Math.max(cliMtime.getTime(), ...engineSiblings.map((p) => statSync(p).mtimeMs));
const cliStale = binMtime < newestCodeMtimeUnder(join(repoRoot, 'src'));
const wsl = wslAvailable();
const tmpRoot = join(os.tmpdir(), `dss-state-${process.pid}`);
mkdirSync(tmpRoot, { recursive: true });

console.error(`dss-state: cli=${cli}`);
console.error(`dss-state: host target=${host.spec}, probes=${PROBES.length}, jobs=${JOBS}, wsl=${wsl}`);

const t0 = Date.now();
const [results, matrix] = await Promise.all([
  pool(PROBES, JOBS, (p) => runProbe(cli, host, tmpRoot, p)),
  runMatrix(cli, tmpRoot, wsl),
]);
const batterySecs = Math.round((Date.now() - t0) / 1000);

const plan = parsePlan23();
const registry = parseRegistry();
const { root: buildRoot, config: buildConfig } = buildInfoOf(cli);
const ctest = flag('--ctest') ? await runCtestLive(buildRoot, buildConfig) : parseCtestLog(buildRoot);
const vel = gitVelocity();

if (!flag('--keep')) { try { rmSync(tmpRoot, { recursive: true, force: true }); } catch {} }

// ───────────────────────── computation ─────────────────────────
const byCat = {};
for (const cat of Object.keys(CATEGORIES)) byCat[cat] = { pass: 0, total: 0, fails: [] };
let miscompiles = [], toolfails = [];
for (const r of results) {
  const b = byCat[r.cat];
  b.total++;
  if (r.status === 'pass') b.pass++;
  else {
    b.fails.push(r);
    if (r.status === 'miscompile' || r.status === 'crash') miscompiles.push(r);
    if (r.status === 'toolfail') toolfails.push(r);
  }
}
const totalProbes = results.length;
const totalPass = results.filter((r) => r.status === 'pass').length;

// Funnel: how deep into the pipeline does each probe get?
const rejectedAt = (tiers) => results.filter((r) => r.status === 'rejected' && tiers.includes(r.tier)).length;
const survivedParse = totalProbes - rejectedAt(['lex', 'parse']);
const survivedSema = survivedParse - rejectedAt(['semantic']);
const producedBinary = results.filter((r) => r.status === 'pass' || r.status === 'miscompile' || r.status === 'crash').length;

// SQLite readiness: category pass-fraction × weight.
let sqliteScore = 0;
for (const [cat, meta] of Object.entries(CATEGORIES)) {
  const b = byCat[cat];
  if (meta.sqliteWeight && b.total) sqliteScore += meta.sqliteWeight * (b.pass / b.total);
}

// Plan axis (cycle-weighted).
const doneIds = plan.phases.filter((p) => p.status === '✅').map((p) => p.id);
const openIds = plan.phases.filter((p) => p.status !== '✅').map((p) => p.id);
const cyclesOf = (ids) => ids.reduce((s, id) => s + (PHASE_CYCLE_EST[id] ?? 0), 0);
const doneCycles = cyclesOf(doneIds);
const remCycles = cyclesOf(openIds);
const arcPct = pct(doneCycles, doneCycles + remCycles);

// SQLite remaining cycles = cheapest route still open + buffer.
const sqliteRoutes = Object.entries(SQLITE_PATHS).map(([name, ids]) => ({
  name, cycles: cyclesOf(ids.filter((id) => openIds.includes(id))) + SQLITE_EXTRA_CYCLES,
}));
sqliteRoutes.sort((a, b) => a.cycles - b.cycles);
const sqliteRem = sqliteRoutes[0];

// Plan-derived velocity (git is squash-blind — see calibration note above).
const arcDays = Math.max(1, Math.floor((today() - new Date(PLAN_ARC_START)) / 86400000) + 1);
const vBurst = doneCycles / arcDays;
const vSteady = vBurst * SUSTAIN_FACTOR;

// ETA scenarios.
function etaTable(remaining) {
  if (vBurst <= 0) return null;
  const sc = [
    [`burst — the FC-arc pace continues (${Math.round(vBurst * 10) / 10} cycles/day)`, vBurst, 1],
    [`steady — sustainability haircut ×${SUSTAIN_FACTOR}`, vSteady, 1],
    [`conservative — steady pace + 1.5× cycle estimates`, vSteady, 1.5],
  ];
  return sc.map(([label, v, mult]) => {
    const days = Math.ceil((remaining * mult) / v);
    return { label, days, date: isoDate(addDays(today(), days)) };
  });
}

// ───────────────────────── render ─────────────────────────
const L = [];
const sortedCats = Object.entries(CATEGORIES);
L.push(`# DSS Code Prime — compiler state report`);
L.push('');
L.push(`Generated ${new Date().toISOString().slice(0, 16).replace('T', ' ')}Z · HEAD \`${vel.head}\` (\`${vel.branch}\`) · battery ${batterySecs}s`);
L.push(`CLI: \`${cli}\` (built ${isoDate(cliMtime)}${cliStale ? ' — ⚠ built before the last src/ commit timestamp; possibly stale, rebuild to be sure' : ', fresh vs src/'})`);
L.push('');
L.push(`## Headline axes`);
L.push('');
L.push(`| Axis | Value | Basis |`);
L.push(`|---|---|---|`);
L.push(`| **Full-C arc (plan-23, cycle-weighted)** | **${arcPct}** | ${doneCycles}/${doneCycles + remCycles} est. cycles; ${doneIds.length}/${plan.phases.length} FC phases ✅ |`);
L.push(`| **Empirical C-feature battery** | **${pct(totalPass, totalProbes)}** | ${totalPass}/${totalProbes} probes compile AND run correctly on \`${host.spec}\` |`);
L.push(`| **SQLite-readiness (weighted)** | **${Math.round(sqliteScore * 10) / 10}%** | per-category battery × amalgamation-demand weights |`);
L.push(`| Test suite | ${ctest ? `${ctest.passed}/${ctest.passed + ctest.failed} (${pct(ctest.passed, ctest.passed + ctest.failed)})` : 'no data'} | ${ctest ? `ctest, ${ctest.asOf}` : 'LastTest.log missing — run with --ctest'} |`);
L.push(`| Deferred-anchor closure | ${registry ? `${registry.closed}/${registry.total} (${pct(registry.closed, registry.total)})` : 'n/a'} | registry rows marked ✅ |`);
L.push(`| Cross-target emit | ${matrix.filter((m) => m.emit).length}/${matrix.length} | canonical program, all exec formats |`);
L.push(`| Runtime witnessed here | ${matrix.filter((m) => /✓/.test(m.run)).length}/${matrix.length} | native + WSL/qemu; Mach-O is CI-leg territory |`);
L.push(`| CPU targets shipped | 2/3 | x86_64 + arm64 shipped; RISC-V is the planned V2-5 stepper row |`);
if (plan.drift.length) L.push(`| ⚠ PLAN DRIFT | FC ids unknown to calibration: ${plan.drift.join(', ')} | update PHASE_CYCLE_EST in driver.mjs |`);
L.push('');
L.push(`## Pipeline funnel (battery of ${totalProbes})`);
L.push('');
L.push(`survives parse **${pct(survivedParse, totalProbes)}** → survives semantic **${pct(survivedSema, totalProbes)}** → produces a binary **${pct(producedBinary, totalProbes)}** → runs correctly **${pct(totalPass, totalProbes)}**`);
L.push('');
L.push(`## Battery by category`);
L.push('');
L.push(`| Category | Pass | % | SQLite weight | Died at (tier×count) |`);
L.push(`|---|---|---|---|---|`);
for (const [cat, meta] of sortedCats) {
  const b = byCat[cat];
  const tierCounts = {};
  for (const f of b.fails) {
    const k = f.status === 'rejected' ? f.tier : f.status;
    tierCounts[k] = (tierCounts[k] ?? 0) + 1;
  }
  const died = Object.entries(tierCounts).map(([k, v]) => `${k}×${v}`).join(', ') || '—';
  L.push(`| ${meta.label} | ${b.pass}/${b.total} | ${pct(b.pass, b.total)} | ${meta.sqliteWeight} | ${died} |`);
}
L.push('');
if (miscompiles.length) {
  L.push(`## 🔴 MISCOMPILES / CRASHES (compiled fine, ran WRONG — investigate first)`);
  L.push('');
  for (const m of miscompiles) L.push(`- **${m.id}** (${m.cat}): ${m.detail}`);
  L.push('');
}
if (toolfails.length) {
  L.push(`## ⚠ Tool failures (neither clean reject nor run — driver/CLI problem)`);
  L.push('');
  for (const m of toolfails) L.push(`- **${m.id}**: ${m.detail}`);
  L.push('');
}
L.push(`## Failing probes (rejected — the feature gap list)`);
L.push('');
for (const [cat, meta] of sortedCats) {
  const rej = byCat[cat].fails.filter((f) => f.status === 'rejected');
  if (!rej.length) continue;
  L.push(`- **${meta.label}**: ${rej.map((f) => `${f.id} [${f.tier}: ${f.detail}]`).join(' · ')}`);
}
L.push('');
L.push(`## Cross-target matrix (canonical \`return 42\`)`);
L.push('');
L.push(`| Target | Emit | Runtime |`);
L.push(`|---|---|---|`);
for (const m of matrix) L.push(`| ${m.label} | ${m.emit ? '✓' : '✗ FAILED'} | ${m.run} |`);
L.push('');
L.push(`## Plan-23 phase board`);
L.push('');
L.push(plan.phases.map((p) => `${p.status}FC${p.id}`).join(' · '));
L.push('');
L.push(`## Velocity & ETA`);
L.push('');
if (vBurst > 0) {
  L.push(`Velocity from the plan board: **${doneCycles} cycles of completed FC phases** over ${arcDays} calendar days since the arc opened (${PLAN_ARC_START}) → **${Math.round(vBurst * 10) / 10} cycles/day burst**, **${Math.round(vSteady * 10) / 10} cycles/day steady** (×${SUSTAIN_FACTOR} sustainability haircut).`);
  if (vel.cycles) L.push(`Git cross-check: ${vel.cycles} \`^v0.0.2\` commits visible since ${vel.firstCycle} (~${Math.round(vel.vCalendar * 10) / 10}/day) — a FLOOR only, squash-merged PRs hide per-cycle commits.`);
  L.push('');
  L.push(`**Milestone: SQLite amalgamation compiles + runs** (cheapest route: ${sqliteRem.name}) — **~${sqliteRem.cycles} cycles remain** (open prerequisite phases + ${SQLITE_EXTRA_CYCLES}-cycle breadth buffer):`);
  L.push('');
  L.push(`| Scenario | Days | ETA |`);
  L.push(`|---|---|---|`);
  for (const e of etaTable(sqliteRem.cycles)) L.push(`| ${e.label} | ~${e.days} | **${e.date}** |`);
  L.push('');
  const alt = sqliteRoutes[1];
  L.push(`(Other route — ${alt.name}: ~${alt.cycles} cycles. Routes share FC5–FC8 + the preprocessor; they diverge only on which ABI cluster lands first.)`);
  L.push('');
  L.push(`**Full C23 arc (all ${plan.phases.length} FC phases, all targets)** — ~${remCycles} cycles remain:`);
  L.push('');
  L.push(`| Scenario | Days | ETA |`);
  L.push(`|---|---|---|`);
  for (const e of etaTable(remCycles)) L.push(`| ${e.label} | ~${e.days} | **${e.date}** |`);
} else {
  L.push(`No completed FC phases parsed from plan-23 — velocity unavailable, ETA suppressed (battery axes above still valid).`);
}
L.push('');
L.push(`### ETA caveats (read before quoting)`);
L.push('');
L.push(`- Cycle costs are CALIBRATED ESTIMATES (driver PHASE_CYCLE_EST), tuned on FC1–FC4 actuals; ABI (cluster C) and preprocessor (D) phases carry the most expansion risk.`);
L.push(`- Velocity assumes the agent-driven /dss-cycle cadence continues; because it is calendar-based, idle days automatically lower it on the next run.`);
L.push(`- "SQLite compiles" ≠ "SQLite test suite passes" — expect a shakeout tail beyond the buffer for a codebase that size.`);
L.push('');

const report = L.join('\n');
console.log(report);
const mdPath = opt('--md');
if (mdPath) { writeFileSync(mdPath, report); console.error(`dss-state: wrote ${mdPath}`); }
const jsonPath = opt('--json');
if (jsonPath) {
  writeFileSync(jsonPath, JSON.stringify({
    generated: new Date().toISOString(), head: vel.head, branch: vel.branch,
    cli, cliBuilt: cliMtime.toISOString(), cliStale, hostSpec: host.spec,
    battery: { total: totalProbes, pass: totalPass, results },
    funnel: { survivedParse, survivedSema, producedBinary, ranCorrect: totalPass },
    byCategory: byCat, matrix, plan, registry, ctest,
    arc: { doneCycles, remCycles, doneIds, openIds },
    sqlite: { readinessPct: Math.round(sqliteScore * 10) / 10, routes: sqliteRoutes },
    velocity: { ...vel, planArcStart: PLAN_ARC_START, arcDays, vBurst, vSteady, sustainFactor: SUSTAIN_FACTOR },
  }, null, 2));
  console.error(`dss-state: wrote ${jsonPath}`);
}
if (flag('--strict') && (miscompiles.length || toolfails.length)) process.exit(1);
