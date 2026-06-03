module.exports = {
  apps: [
    {
      name: 'mock-cpu',
      script: './mock-cpu.js',
      restart_delay: 5000,
      max_restarts: 99,
      env: { NODE_ENV: 'production' }
    },
    {
      name: 'mock-memory',
      script: './mock-memory.js',
      restart_delay: 5000,
      max_restarts: 99,
      node_args: '--max-old-space-size=512',
      env: { NODE_ENV: 'production' }
    },
    {
      name: 'mock-network',
      script: './mock-network.js',
      restart_delay: 5000,
      max_restarts: 99,
      env: { NODE_ENV: 'production' }
    }
  ]
};
