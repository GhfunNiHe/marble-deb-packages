#!/bin/bash
# allow ModemManager to start inside a container
sudo mkdir -p /etc/systemd/system/ModemManager.service.d/
cat <<EOF | sudo tee /etc/systemd/system/ModemManager.service.d/override.conf
[Unit]
ConditionVirtualization=
EOF
sudo systemctl daemon-reload
sudo systemctl start ModemManager.service
