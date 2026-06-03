module.exports = {
  apps: [
    {
      name: "mock-load",
      script: "./mock-load.js",
      restart_delay: 5000,
      max_restarts: 99,
      node_args: "--max-old-space-size=128",
    },
  ],
};
