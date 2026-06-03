# Oracle VPS Load

If a virtual machine (VM) has an average CPU utilization of less than 10%, network utilization of less than 10%, and memory utilization of less than 10% within 7 days, the VM may be marked as idle and terminated.

## cpp

`mock-load` is a small cross-platform C++ stability-test load generator. It samples the current CPU, memory, and network utilization before applying pressure. If a resource is already at or above the configured target, the matching pressure source backs off.

The default target is 10% for CPU, memory, and network.

### Build

Ubuntu:

```bash
cd cpp
cmake -S . -B build
cmake --build build --config Release
./build/mock-load --duration 300
```

Windows PowerShell:

```powershell
cd cpp
cmake -S . -B build
cmake --build build --config Release
.\build\Release\mock-load.exe --duration 300
```

If the selected Windows generator is not multi-config, the executable may be under `.\build\mock-load.exe`.

### Install

```bash
sudo mkdir -p /opt/mock-load
sudo cp ./build/mock-load /opt/mock-load/mock-load
sudo chmod +x /opt/mock-load/mock-load

sudo vi /etc/systemd/system/mock-load.service
```

```bash
[Unit]
Description=Mock load stability test
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/opt/mock-load/mock-load --duration 0 --cpu-target 1 --memory-target 1 --network-target 0.1 --network-cap-mbps 10
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target

```

```bash
sudo systemctl daemon-reload
sudo systemctl enable mock-load
sudo systemctl start mock-load
systemctl status mock-load
journalctl -u mock-load -f
sudo systemctl stop mock-load
sudo systemctl disable mock-load
```

### Examples

Run for five minutes with the default 10% targets:

```bash
mock-load --duration 300
```

Run until interrupted:

```bash
mock-load --duration 0
```

Set all targets to 8%:

```bash
mock-load --target 8
```

Set separate targets:

```bash
mock-load --cpu-target 10 --memory-target 10 --network-target 10
```

Use a remote UDP target for real network-interface traffic:

```bash
mock-load --network-host 192.0.2.10 --network-port 49010
```

By default, network pressure uses a local UDP sink on `127.0.0.1` to avoid sending traffic to another machine. For physical network testing, run a UDP receiver on the target host and pass `--network-host`.

### Options

```text
--target PERCENT             Set CPU, memory, and network targets.
--cpu-target PERCENT         Set the CPU target.
--memory-target PERCENT      Set the memory target.
--network-target PERCENT     Set the network target.
--duration SECONDS           Run time. Use 0 to run until Ctrl+C.
--sample-interval SECONDS    Measurement interval.
--network-cap-mbps MBPS      Fallback network capacity when link speed is unavailable.
--network-host HOST          UDP target host. Defaults to 127.0.0.1.
--network-port PORT          UDP target port. Defaults to 49010.
--no-local-sink              Do not start the local UDP receiver.
--max-memory-mb MB           Cap memory allocation. Use 0 for no cap.
--no-cpu                     Disable CPU pressure.
--no-memory                  Disable memory pressure.
--no-network                 Disable network pressure.
--quiet                      Print only startup and shutdown messages.
--help                       Show help.
```

### Notes

- CPU utilization is averaged across all logical CPUs.
- Memory pressure allocates and touches memory pages so the operating system commits them.
- Network utilization is estimated from interface byte counters and link speed when available. If link speed cannot be read, `--network-cap-mbps` is used.
- Local loopback network pressure uses `--network-cap-mbps` as its pacing capacity. With the default 100 Mbps capacity, a 10% network target sends about 10 Mbps locally.
- The controller is conservative: it estimates existing background load and only adds the pressure needed to approach the target.

## nodejs-one

mock-load ram: 72mb

```bash
cd /opt/oracle-vps-load
cd nodejs-one
```

## nodejs-separation

mock-cpu ram: 64mb
mock-memory ram: 63mb
mock-network ram: 70mb

### Quick

```bash
sudo chown ubuntu:ubuntu /opt
cd /opt
git clone https://github.com/rojarsmith/oracle-vps-load.git /opt/oracle-vps-load
cd /opt/oracle-vps-load
cd nodejs-separation
pm2 start ecosystem.config.js
pm2 save
pm2 startup
sudo env PATH=$PATH:/home/ubuntu/.nvm/versions/node/v24.16.0/bin /home/ubuntu/.nvm/versions/node/v24.16.0/lib/node_modules/pm2/bin/pm2 startup systemd -u ubuntu --hp /home/ubuntu
pm2 logs --lines 20
```

