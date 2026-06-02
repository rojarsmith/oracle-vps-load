const https = require("https");

const fs = require("fs");

const TARGET_MIN_KBPS = 50; // Minimum target bandwidth KB/s

const CHECK_SEC = 15;

const TARGETS = [
  "https://www.google.com",

  "https://cloudflare.com",

  "https://github.com",

  "https://httpbin.org/get",
];

// Read network traffic (Linux only)
function readNetBytes() {
  try {
    const lines = fs.readFileSync("/proc/net/dev", "utf8").split("\n");

    let rx = 0,
      tx = 0;

    lines.slice(2).forEach((line) => {
      const parts = line.trim().split(/\s+/);

      if (parts.length < 10 || parts[0].startsWith("lo")) return;

      rx += parseInt(parts[1], 10) || 0;

      tx += parseInt(parts[9], 10) || 0;
    });

    return { rx, tx };
  } catch {
    return null; // Skip measurement in non-Linux environment
  }
}

let lastBytes = readNetBytes();

let lastTime = Date.now();

let nextDelay = 5000; // Initial request interval 5s

function measureKbps() {
  const now = Date.now();

  const cur = readNetBytes();

  if (!cur || !lastBytes) return null;

  const elapsed = (now - lastTime) / 1000;

  const bytes = cur.rx - lastBytes.rx + (cur.tx - lastBytes.tx);

  const kbps = bytes / 1024 / elapsed;

  lastBytes = cur;

  lastTime = now;

  return kbps;
}

function request() {
  const kbps = measureKbps();

  if (kbps !== null) {
    const gap = TARGET_MIN_KBPS - kbps;

    if (gap <= 0) {
      // Sufficient bandwidth, slow down requests
      nextDelay = Math.min(nextDelay * 1.5, 60000);

      console.log(
        `[mock-network] ${kbps.toFixed(1)} KB/s >= ${TARGET_MIN_KBPS}, reduce frequency to ${(nextDelay / 1000).toFixed(0)}s`,
      );
    } else {
      // Insufficient bandwidth, speed up requests
      nextDelay = Math.max(nextDelay * 0.6, 2000);
      console.log(
        `[mock-network] ${kbps.toFixed(1)} KB/s < ${TARGET_MIN_KBPS}, increase frequency to ${(nextDelay / 1000).toFixed(0)}s`,
      );
    }
  }

  const url = TARGETS[Math.floor(Math.random() * TARGETS.length)];
  https
    .get(url, (res) => {
      let bytes = 0;
      res.on("data", (d) => {
        bytes += d.length;
      });
      res.on("end", () =>
        console.log(`[mock-network] ${url} → ${res.statusCode} (${bytes}B)`),
      );
    })
    .on("error", (e) => console.error(`[mock-network] error: ${e.message}`));

  setTimeout(request, nextDelay);
}

console.log("[mock-network] started — measuring before requesting");
request();
