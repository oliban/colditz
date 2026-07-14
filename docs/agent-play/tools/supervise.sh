#!/bin/bash
# Keeps the dashboard server alive; logs restarts.
D="$(cd "$(dirname "$0")" && pwd)"
while true; do
  python3 "$D/server.py" >> /tmp/dashboard.log 2>&1
  echo "$(date) dashboard server exited (rc=$?), restarting in 2s" >> /tmp/dashboard.log
  sleep 2
done
