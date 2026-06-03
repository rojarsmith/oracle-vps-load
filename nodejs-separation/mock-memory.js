const os = require("os");

const TARGET_MIN_PCT = 15; // Target minimum memory usage %

const CHUNK_MB = 20;

const CHECK_SEC = 30;

const pool = [];

function currentMemPct() {
  const total = os.totalmem();

  const used = total - os.freemem();

  return (used / total) * 100;
}

function allocChunk() {
  const buf = Buffer.alloc(CHUNK_MB * 1024 * 1024);

  for (let i = 0; i < buf.length; i += 4096) buf[i] = Math.random() * 255;

  return buf;
}

function freeAll() {
  pool.length = 0;

  global.gc && global.gc(); // Force GC if --expose-gc is enabled
}

function check() {
  const pct = currentMemPct();

  const gap = TARGET_MIN_PCT - pct;

  if (gap <= 0) {
    // The system has exceeded its limits; if we have allocated blocks, we will gradually release them.
    if (pool.length > 0) {
      pool.pop();
      console.log(
        `[mock-memory] ​​${pct.toFixed(1)}% >= ${TARGET_MIN_PCT}%, releases one block (leaving ${pool.length} blocks)`,
      );
    } else {
      console.log(
        `[mock-memory] ​​${pct.toFixed(1)}% >= ${TARGET_MIN_PCT}%, no padding needed`,
      );
    }
  } else {
    // Calculate how many blocks need to be padded
    const totalMB = os.totalmem() / 1024 / 1024;
    const needMB = (gap / 100) * totalMB;
    const needChunks = Math.ceil(needMB / CHUNK_MB);
    console.log(
      `[mock-memory] ​​${pct.toFixed(1)}%, padding with ${needMB.toFixed(0)}MB (+${needChunks} blocks)`,
    );
    for (let i = 0; i < needChunks; i++) pool.push(allocChunk());
  }

  // Periodic rotation to prevent GC
  if (pool.length > 0) {
    const idx = Math.floor(Math.random() * pool.length);
    pool[idx] = allocChunk();
  }

  setTimeout(check, CHECK_SEC * 1000);
}

console.log("[mock-memory] started — measuring before allocating");
check();
