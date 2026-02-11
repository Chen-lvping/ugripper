#!/bin/bash
set -e

REQUEST_FILE="/tmp/umi_shutdown_request"

if [ ! -f "$REQUEST_FILE" ]; then
    echo "No shutdown request file found, skip."
    exit 0
fi

rm -f "$REQUEST_FILE"
echo "Shutdown request consumed, powering off..."

/usr/bin/systemctl poweroff
