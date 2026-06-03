// mock-load.js — Three-in-one, running only one process
const os = require("os");
const https = require("https");
const fs = require("fs");

// ── CPU ──────────────────────────────────────────
function measureCPU() {
  return new Promise((resolve) => {
    const s1 = os.cpus().map((c) => ({ ...c.times }));
    setTimeout(() => {
      const s2 = os.cpus();
      let idle = 0,
        total = 0;
      s2.forEach((cpu, i) => {
        const d = {
          user: cpu.times.user - s1[i].user,
          sys: cpu.times.sys - s1[i].sys,
          idle: cpu.times.idle - s1[i].idle,
          irq: cpu.times.irq - s1[i].irq,
          nice: cpu.times.nice - s1[i].nice,
        };
        const t = Object.values(d).reduce((a, b) => a + b, 0);
        idle += d.idle;
        total += t;
      });
      resolve(100 * (1 - idle / total));
    }, 1000);
  });
}

function burnCPU(ms) {
  const end = Date.now() + ms;
  let x = 0;
  while (Date.now() < end) x = Math.sqrt(Math.random() * 99999) * Math.PI;
}

async function cpuLoop() {
  const cur = await measureCPU();
  const gap = 15 - cur;
  if (gap > 0) {
    burnCPU(10000 * (gap / 100));
    console.log(`[cpu] ${cur.toFixed(1)}% → 補足 ${gap.toFixed(1)}%`);
  }
  setTimeout(cpuLoop, 10000);
}

// ── Memory ───────────────────────────────────────
const CHUNK_MB = 20;
const pool = [];

function checkMemory() {
  const pct = (1 - os.freemem() / os.totalmem()) * 100;
  const gap = 15 - pct;
  if (gap > 0) {
    const need = Math.ceil(
      ((gap / 100) * os.totalmem()) / 1024 / 1024 / CHUNK_MB,
    );
    for (let i = 0; i < need; i++) {
      const buf = Buffer.alloc(CHUNK_MB * 1024 * 1024);
      for (let j = 0; j < buf.length; j += 4096) buf[j] = 1;
      pool.push(buf);
    }
    console.log(`[mem] ${pct.toFixed(1)}% → 補 ${need} 塊`);
  } else if (gap < -5 && pool.length > 0) {
    pool.pop();
    console.log(`[mem] ${pct.toFixed(1)}% 過高 → 釋放一塊`);
  }
  setTimeout(checkMemory, 30000);
}

// ── Network ──────────────────────────────────────
const TARGETS = [
  "https://www.google.com",
  "https://cloudflare.com",
  "https://github.com",
];
let netDelay = 8000;

function readNetKbps() {
  try {
    const lines = fs.readFileSync("/proc/net/dev", "utf8").split("\n");
    let rx = 0,
      tx = 0;
    lines.slice(2).forEach((l) => {
      const p = l.trim().split(/\s+/);
      if (p.length < 10 || p[0].startsWith("lo")) return;
      rx += parseInt(p[1]) || 0;
      tx += parseInt(p[9]) || 0;
    });
    return { rx, tx, time: Date.now() };
  } catch {
    return null;
  }
}

let lastNet = readNetKbps();

function netRequest() {
  const cur = readNetKbps();
  if (cur && lastNet) {
    const kb =
      (cur.rx - lastNet.rx + (cur.tx - lastNet.tx)) /
      1024 /
      ((cur.time - lastNet.time) / 1000);
    netDelay =
      kb < 50
        ? Math.max(netDelay * 0.7, 2000)
        : Math.min(netDelay * 1.3, 60000);
    console.log(
      `[net] ${kb.toFixed(1)} KB/s → 下次 ${(netDelay / 1000).toFixed(0)}s`,
    );
    lastNet = cur;
  }
  const url = TARGETS[Math.floor(Math.random() * TARGETS.length)];
  https.get(url, (res) => res.resume()).on("error", () => {});
  setTimeout(netRequest, netDelay);
}

// ── Start ─────────────────────────────────────────
console.log("[mock-load] started");
cpuLoop();
checkMemory();
netRequest();
