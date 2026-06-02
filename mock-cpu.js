const os = require("os");

const TARGET_MIN = 15; // Target minimum CPU %

const CHECK_SEC = 10; // Remeasure every few seconds

// Measure CPU utilization (take 1-second samples)
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

// Burn CPU durationMs milliseconds
function burnCPU(durationMs) {
  const end = Date.now() + durationMs;
  let x = 0;
  while (Date.now() < end) x = Math.sqrt(Math.random() * 99999) * Math.PI;
  return x;
}

async function loop() {
  const current = await measureCPU();
  const gap = TARGET_MIN - current;

  if (gap <= 0) {
    console.log(
      `[mock-cpu] CPU ${current.toFixed(1)}% >= ${TARGET_MIN}% target, skip`,
    );
  } else {
    // Make up the gap% work in the next CHECK_SEC cycle
    const totalMs = CHECK_SEC * 1000;
    const workMs = totalMs * (gap / 100);
    console.log(
      `[mock-cpu] CPU ${current.toFixed(1)}%, make up ${gap.toFixed(1)}%, burn ${workMs.toFixed(0)}ms`,
    );
    burnCPU(workMs);
  }

  setTimeout(loop, CHECK_SEC * 1000);
}

console.log("[mock-cpu] started — measuring before burning");
loop();
