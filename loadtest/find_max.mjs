#!/usr/bin/env node

// Written with AI.

// Find, per scenario, the concurrency at which the server tops out.
//
// Runs shortener.js (k6) repeatedly at a fixed VU count (= concurrent requests)
// and binary-searches that count. A VU count is "worth going past" when:
//   - every endpoint stays within its p50 / p99 / error limits (shortener.js), and
//   - throughput is still below (1 - saturation) of the best req/s seen so far.
// Past saturation, extra VUs only queue up: req/s stays flat and latency grows
// linearly, so there's nothing left to learn and we stop.
//
//   1. Exponential probe: start, 2x, 4x, ... Stop when limits fail, when doubling
//      VUs gains less than `saturation` req/s, or at --max.
//   2. Binary search between the largest VU count worth going past and the
//      smallest one that isn't, until they're within `resolution` of each other.
//
// Result per scenario:
//   saturated   -> smallest VU count reaching ~peak req/s (the knee)
//   limit hit   -> largest VU count that still met the limits
//   neither     -> still scaling at --max
//
// This is a separate Node script because k6 can't change its VU count based on
// results mid-run, and a k6 script can't launch k6. Node runs k6 once per step
// and reads the JSON summary shortener.js writes. Uses a local `k6` binary if
// there is one, otherwise the grafana/k6 Docker image with host networking.
//
//   node loadtest/find_max.mjs
//   node loadtest/find_max.mjs --scenarios shortener --duration 30 -e SHORTEN_P99_MS=200
//   node loadtest/find_max.mjs --saturation 0    # ignore throughput, limits only

import { spawnSync, execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { cpus } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SCRIPT = 'shortener.js';
const K6_IMAGE = process.env.K6_IMAGE || 'grafana/k6:1.0.0';

const { values: opts } = parseArgs({
  options: {
    'base-url': { type: 'string', default: 'http://127.0.0.1:8080' },
    scenarios: { type: 'string', default: 'health,shortener' },
    start: { type: 'string', default: '1' }, // first VU count to try
    max: { type: 'string', default: '4096' }, // upper bound on VUs
    saturation: { type: 'string', default: '0.05' }, // "flat" = within 5% of peak req/s; 0 disables
    resolution: { type: 'string', default: '0.05' }, // stop when bracket is within 5% of lo
    warmup: { type: 'string', default: '5' }, // seconds of ramp-up per run, not measured
    duration: { type: 'string', default: '20' }, // measured seconds per run
    cooldown: { type: 'string', default: '3' }, // seconds between runs
    env: { type: 'string', short: 'e', multiple: true, default: [] }, // KEY=VAL passed to k6
    docker: { type: 'boolean', default: false }, // force the Docker image
    help: { type: 'boolean', short: 'h', default: false },
  },
});

if (opts.help) {
  console.log(readFileSync(fileURLToPath(import.meta.url), 'utf8').split('\n\nimport')[0]);
  process.exit(0);
}

const cfg = {
  baseUrl: opts['base-url'],
  scenarios: opts.scenarios.split(',').map((s) => s.trim()).filter(Boolean),
  start: parseInt(opts.start, 10),
  max: parseInt(opts.max, 10),
  saturation: parseFloat(opts.saturation),
  resolution: parseFloat(opts.resolution),
  warmup: parseInt(opts.warmup, 10),
  duration: parseInt(opts.duration, 10),
  cooldown: parseFloat(opts.cooldown),
  extraEnv: Object.fromEntries(opts.env.map((kv) => [kv.slice(0, kv.indexOf('=')), kv.slice(kv.indexOf('=') + 1)])),
  docker: opts.docker,
};

const has = (bin) => spawnSync('sh', ['-c', `command -v ${bin}`]).status === 0;
if (!cfg.docker && !has('k6')) {
  if (!has('docker')) {
    console.error('Neither k6 nor docker found');
    process.exit(1);
  }
  console.log(`k6 not found, using Docker image ${K6_IMAGE}`);
  cfg.docker = true;
}

const sleep = (s) => new Promise((r) => setTimeout(r, s * 1000));

// summaryRel is relative to loadtest/, so it works both locally and in Docker.
function k6Command(env, summaryRel) {
  let cmd, script;
  if (cfg.docker) {
    env = { ...env, SUMMARY_FILE: `/lt/${summaryRel}` };
    cmd = ['docker', 'run', '--rm', '--network', 'host',
      '-u', `${process.getuid()}:${process.getgid()}`,
      '-v', `${HERE}:/lt`, K6_IMAGE, 'run', '--quiet'];
    script = `/lt/${SCRIPT}`;
  } else {
    env = { ...env, SUMMARY_FILE: path.join(HERE, summaryRel) };
    cmd = ['k6', 'run', '--quiet'];
    script = path.join(HERE, SCRIPT);
  }
  for (const [k, v] of Object.entries(env)) cmd.push('-e', `${k}=${v}`);
  return [...cmd, script];
}

function runOnce(scenario, vus) {
  const env = {
    BASE_URL: cfg.baseUrl,
    SCENARIO: scenario,
    VUS: vus,
    WARMUP: cfg.warmup,
    DURATION: cfg.duration,
    ...cfg.extraEnv,
  };
  const summaryRel = path.join('results', `.step_${scenario}.json`);
  const summaryAbs = path.join(HERE, summaryRel);
  rmSync(summaryAbs, { force: true });

  const [bin, ...args] = k6Command(env, summaryRel);
  // Exit code 99 = thresholds crossed, which is a normal "fail" for us.
  const proc = spawnSync(bin, args, { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
  if (!existsSync(summaryAbs)) {
    process.stderr.write((proc.stdout || '') + (proc.stderr || '') + (proc.error ? String(proc.error) : ''));
    console.error(`\nk6 produced no summary (exit ${proc.status}); see output above`);
    process.exit(1);
  }
  const summary = JSON.parse(readFileSync(summaryAbs, 'utf8'));
  rmSync(summaryAbs);
  return summary.scenarios[scenario];
}

const fix = (v, d = 2) => (v === null || v === undefined ? '-' : v.toFixed(d));
const pct = (v) => (v === null || v === undefined ? '-' : fix(v * 100) + '%');

function printStep(vus, sc, note) {
  const eps = Object.entries(sc.endpoints);
  eps.forEach(([ep, e], i) => {
    console.log(
      `${(i === 0 ? String(vus) : '').padStart(6)}  ${ep.padEnd(8)}  ${fix(e.rps, 1).padStart(10)}  ` +
      `${fix(e.p50_ms).padStart(8)}  ${fix(e.p99_ms).padStart(8)}  ${pct(e.error_rate).padStart(7)}  ` +
      `${e.passed ? 'PASS' : 'FAIL'}`,
    );
  });
  if (eps.length > 1) console.log(`${''.padStart(6)}  ${'total'.padEnd(8)}  ${fix(sc.rps, 1).padStart(10)}`);
  if (note) console.log(`${''.padStart(6)}  ${note}`);
}

async function search(scenario) {
  console.log(`\n=== ${scenario} ===`);
  console.log(`${'VUs'.padStart(6)}  ${'endpoint'.padEnd(8)}  ${'req/s'.padStart(10)}  ${'p50 ms'.padStart(8)}  ` +
    `${'p99 ms'.padStart(8)}  ${'errors'.padStart(7)}  limits`);

  const history = new Map(); // vus -> scenario result
  const peak = () => Math.max(0, ...[...history.values()].map((h) => h.rps));

  async function probe(vus, note) {
    if (!history.has(vus)) {
      history.set(vus, runOnce(scenario, vus));
      await sleep(cfg.cooldown); // let TIME_WAIT sockets and the server settle
    }
    const h = history.get(vus);
    printStep(vus, h, note?.(h));
    return h;
  }

  // Worth going past this VU count? Re-evaluated against the latest peak.
  const worthMore = (h) => h.passed && (cfg.saturation <= 0 || h.rps < (1 - cfg.saturation) * peak());

  // hi = smallest probed VU count not worth going past; lo = largest one below it that is.
  function bounds() {
    const vs = [...history.keys()].sort((a, b) => a - b);
    const hi = vs.find((v) => !worthMore(history.get(v)));
    const below = vs.filter((v) => (hi === undefined || v < hi) && worthMore(history.get(v)));
    return { lo: below.length ? below[below.length - 1] : 0, hi };
  }

  // 1. Exponential probe.
  let prev = null;
  for (let vus = cfg.start; ; vus = Math.min(vus * 2, cfg.max)) {
    const gain = (h) => (prev ? `+${fix((h.rps / prev.rps - 1) * 100, 1)}% req/s vs ${prev.vus} VUs` : '');
    const h = await probe(vus, gain);
    const saturated = cfg.saturation > 0 && prev && h.rps < prev.rps * (1 + cfg.saturation);
    if (!h.passed || saturated || vus >= cfg.max) break;
    prev = { vus, rps: h.rps };
  }

  // 2. Binary search.
  let { lo, hi } = bounds();
  while (hi !== undefined && hi - lo > Math.max(1, Math.floor(lo * cfg.resolution))) {
    await probe(Math.floor((lo + hi) / 2));
    ({ lo, hi } = bounds());
  }

  let reason, maxVus;
  const at = (v) => history.get(v);
  if (hi === undefined) {
    maxVus = lo;
    reason = `still scaling at --max ${cfg.max} VUs; raise --max to go further`;
  } else if (!at(hi).passed) {
    maxVus = lo;
    reason = lo === 0
      ? 'fails the limits even at 1 VU'
      : `limits hit at ${hi} VUs (${failedLimits(at(hi))}) before throughput saturated`;
  } else {
    maxVus = hi;
    reason = `throughput saturates at ~${fix(peak(), 0)} req/s; beyond ${hi} VUs only latency grows`;
  }
  console.log(`-> ${scenario}: ${maxVus} concurrent requests. ${reason}`);

  const steps = Object.fromEntries([...history.entries()].sort((a, b) => a[0] - b[0]));
  return { max_vus: maxVus, reason, peak_rps: peak(), at_max: at(maxVus) || null, steps };
}

function failedLimits(sc) {
  const out = [];
  for (const [ep, e] of Object.entries(sc.endpoints)) {
    const l = e.limits;
    if (e.requests === 0) out.push(`${ep}: no requests`);
    if (e.p50_ms > l.p50) out.push(`${ep} p50 ${fix(e.p50_ms)} > ${l.p50} ms`);
    if (e.p99_ms > l.p99) out.push(`${ep} p99 ${fix(e.p99_ms)} > ${l.p99} ms`);
    if (e.error_rate > l.err) out.push(`${ep} errors ${pct(e.error_rate)} > ${pct(l.err)}`);
  }
  return out.join(', ');
}

function machineInfo() {
  const info = { host_cpus: cpus().length, cpu: cpus()[0]?.model };
  try {
    info.commit = execFileSync('git', ['rev-parse', '--short', 'HEAD'],
      { cwd: HERE, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();
  } catch {
    info.commit = null;
  }
  return info;
}

mkdirSync(path.join(HERE, 'results'), { recursive: true });

const report = { date: new Date().toISOString(), machine: machineInfo(), settings: { ...cfg }, scenarios: {} };
for (const s of cfg.scenarios) report.scenarios[s] = await search(s);

const stamp = new Date().toISOString().replace(/[-:]/g, '').replace('T', '_').slice(0, 15);
const out = path.join(HERE, 'results', `max_${stamp}.json`);
writeFileSync(out, JSON.stringify(report, null, 2));

const m = report.machine;
console.log(`\nCommit ${m.commit}, CPU ${m.cpu} (${m.host_cpus} threads)`);
console.log('\n| Scenario | Endpoint | Concurrency | req/s | p50 (ms) | p99 (ms) | Error rate |');
console.log('|---|---|---|---|---|---|---|');
for (const [s, r] of Object.entries(report.scenarios)) {
  if (!r.at_max) {
    console.log(`| ${s} | - | <1 | - | - | - | - |`);
    continue;
  }
  for (const [ep, e] of Object.entries(r.at_max.endpoints)) {
    console.log(`| ${s} | \`${ep}\` | ${r.max_vus} | ${fix(e.rps, 0)} | ${fix(e.p50_ms)} | ${fix(e.p99_ms)} | ${pct(e.error_rate)} |`);
  }
}
console.log(`\nFull results: ${path.relative(process.cwd(), out)}`);
