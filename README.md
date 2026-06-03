# Oracle VPS Load

If a virtual machine (VM) has an average CPU utilization of less than 10%, network utilization of less than 10%, and memory utilization of less than 10% within 7 days, the VM may be marked as idle and terminated.

## nodejs-separation

mock-cpu ram: 63.4mb
mock-memory ram: 62.6mb
mock-network ram: 69.2mb

### Quick

```bash
sudo chown ubuntu:ubuntu /opt
cd /opt
git clone https://github.com/rojarsmith/oracle-vps-load.git /opt/oracle-vps-load
cd /opt/oracle-vps-load
pm2 start ecosystem.config.js
pm2 save
pm2 startup
sudo env PATH=$PATH:/home/ubuntu/.nvm/versions/node/v24.16.0/bin /home/ubuntu/.nvm/versions/node/v24.16.0/lib/node_modules/pm2/bin/pm2 startup systemd -u ubuntu --hp /home/ubuntu
pm2 logs --lines 20
```

