// Written with AI.

// k6 load test for the URL shortener.
//
// One run = a fixed number of concurrent VUs (closed model: every VU keeps
// exactly one request in flight, no think time), so VUS == concurrent requests.
// find_max.mjs binary-searches VUS by running this script many times.
//
// Two scenarios, run one after the other:
//   health     GET /health in a loop. No DB: measures the HTTP layer alone.
//   shortener  the real user flow, per iteration:
//                1. POST /shorten-url with a new, unique URL  (INSERT)
//                2. GET  /get-shortened-url/<code just created> (SELECT)
//              and checks the resolved URL is the one that was shortened.
//
// Standalone usage:
//   k6 run -e VUS=50 loadtest/shortener.js                        # both scenarios
//   k6 run -e VUS=50 -e SCENARIO=shortener loadtest/shortener.js  # one only
//
// Env vars (all optional):
//   BASE_URL      http://127.0.0.1:8080
//   SCENARIO      health | shortener | both (default both)
//   VUS           concurrent requests (default 10)
//   WARMUP        seconds of ramp-up, not measured (default 5)
//   DURATION      seconds at full VUS, measured (default 20)
//   REQ_TIMEOUT   per-request timeout, counted as an error (default 5s)
//   SUMMARY_FILE  where to write the JSON summary (default: not written)
//   Limits, per endpoint (ms / fraction):
//   HEALTH_P50_MS=10   HEALTH_P99_MS=50   HEALTH_MAX_ERR=0.01
//   SHORTEN_P50_MS=25  SHORTEN_P99_MS=100 SHORTEN_MAX_ERR=0.01
//   RESOLVE_P50_MS=25  RESOLVE_P99_MS=100 RESOLVE_MAX_ERR=0.01
//
// Note: every shortener iteration inserts a row. Clean up afterwards with
//   DELETE FROM urls WHERE long_url LIKE 'https://example.com/loadtest/%';

import exec from 'k6/execution';
import http from 'k6/http';
import { Counter, Rate, Trend } from 'k6/metrics';

const BASE_URL = __ENV.BASE_URL || 'http://127.0.0.1:8080';
const SCENARIO = (__ENV.SCENARIO || 'both').toLowerCase();
const VUS = parseInt(__ENV.VUS || '10', 10);
const WARMUP = parseInt(__ENV.WARMUP || '5', 10);
const DURATION = parseInt(__ENV.DURATION || '20', 10);
const REQ_TIMEOUT = __ENV.REQ_TIMEOUT || '5s';
const GRACE = 5; // seconds a VU gets to finish its last iteration

const num = (name, def) => (__ENV[name] !== undefined ? parseFloat(__ENV[name]) : def);

// The point where we call a load "too much". In a closed-loop test, once the
// server saturates, extra VUs only queue up, so latency grows linearly with VUS
// (Little's law: latency = VUS / throughput). find_max.mjs also stops when
// throughput stops growing; these limits catch the case where latency or
// errors get bad before that.
//   health:  no DB, pure HTTP path. A few ms is already a lot on loopback.
//   shorten/resolve: one INSERT / one indexed SELECT through the pool.
const limit = (prefix, p50, p99, err) => ({
  p50: num(`${prefix}_P50_MS`, p50),
  p99: num(`${prefix}_P99_MS`, p99),
  err: num(`${prefix}_MAX_ERR`, err),
});
export const LIMITS = {
  health: limit('HEALTH', 10, 50, 0.01),
  shorten: limit('SHORTEN', 25, 100, 0.01),
  resolve: limit('RESOLVE', 25, 100, 0.01),
};

// Which endpoints each scenario exercises.
const SCENARIO_ENDPOINTS = {
  health: ['health'],
  shortener: ['shorten', 'resolve'],
};

const ACTIVE = SCENARIO === 'both' ? Object.keys(SCENARIO_ENDPOINTS) : [SCENARIO];
for (const s of ACTIVE) {
  if (!SCENARIO_ENDPOINTS[s]) throw new Error(`SCENARIO must be health, shortener or both, got "${SCENARIO}"`);
}

// Custom metrics, recorded only after warm-up and tagged by endpoint.
// req_latency is the full client-side time including connect, because the
// server closes every connection (Connection: close) and accept-queue delay
// under load would otherwise be invisible in http_req_duration.
const latency = new Trend('req_latency', true);
const errors = new Rate('req_errors');
const count = new Counter('req_count');

const scenarios = {};
const thresholds = {};
ACTIVE.forEach((s, i) => {
  scenarios[s] = {
    executor: 'ramping-vus',
    exec: s,
    startVUs: 0,
    startTime: `${i * (WARMUP + DURATION + GRACE)}s`,
    stages: [
      { duration: `${WARMUP}s`, target: VUS },
      { duration: `${DURATION}s`, target: VUS },
    ],
    gracefulRampDown: '0s',
    gracefulStop: `${GRACE}s`,
  };
  for (const ep of SCENARIO_ENDPOINTS[s]) {
    const l = LIMITS[ep];
    thresholds[`req_latency{endpoint:${ep}}`] = [`p(50)<=${l.p50}`, `p(99)<=${l.p99}`];
    // Stop early when the server is clearly falling over, so the search moves on.
    thresholds[`req_errors{endpoint:${ep}}`] = [
      { threshold: `rate<=${l.err}`, abortOnFail: true, delayAbortEval: '5s' },
    ];
    thresholds[`req_count{endpoint:${ep}}`] = ['count>0'];
  }
});

export const options = {
  scenarios,
  thresholds,
  // The server answers with "Connection: close" and has no keep-alive, so
  // don't let k6 try to reuse sockets: one TCP connection per request.
  noConnectionReuse: true,
  summaryTrendStats: ['avg', 'min', 'p(50)', 'p(90)', 'p(99)', 'max'],
};

export function setup() {
  // Fail fast if the server isn't up at all.
  const h = http.get(`${BASE_URL}/health`);
  if (h.status !== 200) throw new Error(`setup: /health returned ${h.status}`);
}

function record(ep, res, ok) {
  // Skip the ramp-up: only requests started after WARMUP count.
  if (Date.now() - exec.scenario.startTime < WARMUP * 1000) return;
  const t = res.timings;
  const tags = { endpoint: ep };
  latency.add(t.blocked + t.connecting + t.duration, tags);
  errors.add(!ok, tags);
  count.add(1, tags);
}

export function health() {
  const res = http.get(`${BASE_URL}/health`, { timeout: REQ_TIMEOUT, tags: { endpoint: 'health' } });
  record('health', res, res.status === 200);
}

export function shortener() {
  // Unique per VU and iteration. Plain characters only: the handler
  // concatenates the body into SQL.
  const url = `https://example.com/loadtest/${exec.scenario.startTime}/${exec.vu.idInTest}/${exec.vu.iterationInScenario}`;

  const s = http.post(`${BASE_URL}/shorten-url`, url, {
    timeout: REQ_TIMEOUT,
    headers: { 'Content-Type': 'text/plain' },
    tags: { endpoint: 'shorten' },
  });
  const m = s.status === 200 && /To: ([0-9A-Za-z]{10})$/.exec(s.body || '');
  record('shorten', s, !!m);
  if (!m) return; // nothing to resolve; the failure is already counted on shorten

  const r = http.get(`${BASE_URL}/get-shortened-url/${m[1]}`, {
    timeout: REQ_TIMEOUT,
    tags: { endpoint: 'resolve', name: 'GET /get-shortened-url/:id' },
  });
  // A 404 here is an error: the code was created a moment ago.
  record('resolve', r, r.status === 200 && (r.body || '').endsWith(url));
}

function stat(metrics, name, field) {
  const m = metrics[name];
  return m && m.values[field] !== undefined ? m.values[field] : null;
}

export function handleSummary(data) {
  const m = data.metrics;
  const out = { base_url: BASE_URL, vus: VUS, warmup_s: WARMUP, duration_s: DURATION, scenarios: {} };

  for (const s of ACTIVE) {
    const endpoints = {};
    for (const ep of SCENARIO_ENDPOINTS[s]) {
      const l = LIMITS[ep];
      const requests = stat(m, `req_count{endpoint:${ep}}`, 'count') || 0;
      const p50 = stat(m, `req_latency{endpoint:${ep}}`, 'p(50)');
      const p99 = stat(m, `req_latency{endpoint:${ep}}`, 'p(99)');
      const err = stat(m, `req_errors{endpoint:${ep}}`, 'rate');
      endpoints[ep] = {
        requests,
        rps: requests / DURATION,
        p50_ms: p50,
        p99_ms: p99,
        error_rate: err,
        limits: l,
        passed: requests > 0 && p50 !== null && p50 <= l.p50 && p99 <= l.p99 && err <= l.err,
      };
    }
    const all = Object.values(endpoints);
    out.scenarios[s] = {
      // Total HTTP requests/s across the scenario's endpoints.
      rps: all.reduce((a, e) => a + e.rps, 0),
      passed: all.every((e) => e.passed),
      endpoints,
    };
  }
  out.passed = Object.values(out.scenarios).every((s) => s.passed);

  const f = (v, d = 2) => (v === null || v === undefined ? '-' : v.toFixed(d));
  let text = `\nVUs=${VUS}  window=${DURATION}s after ${WARMUP}s warm-up\n`;
  text += 'endpoint   requests      req/s    p50 ms    p99 ms   errors  verdict\n';
  for (const sc of Object.values(out.scenarios)) {
    for (const [ep, e] of Object.entries(sc.endpoints)) {
      text +=
        `${ep.padEnd(9)} ${String(e.requests).padStart(9)} ${f(e.rps, 1).padStart(10)} ` +
        `${f(e.p50_ms).padStart(9)} ${f(e.p99_ms).padStart(9)} ` +
        `${(f((e.error_rate || 0) * 100) + '%').padStart(8)}  ${e.passed ? 'PASS' : 'FAIL'}\n`;
    }
  }

  const res = { stdout: text };
  if (__ENV.SUMMARY_FILE) res[__ENV.SUMMARY_FILE] = JSON.stringify(out, null, 2);
  return res;
}
